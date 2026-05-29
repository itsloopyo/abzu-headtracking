"""Find AController::ControlRotation offset, APlayerCameraManager vtable
location + UpdateCamera index, and writers to POV.Rotation at +0xBC0/+0xBD4.

Outputs a JSON report under .lab/ghidra-out/."""

import json
import os
import re
import sys

import pyghidra

pyghidra.start()

from ghidra.app.decompiler import DecompInterface  # noqa: E402
from ghidra.util.task import ConsoleTaskMonitor  # noqa: E402
from ghidra.program.model.symbol import RefType  # noqa: E402


def collect_string_addresses(program, names):
    """Return {string_value: [Address...]} for the given names."""
    by_value = {n: [] for n in names}
    listing = program.getListing()
    mem = program.getMemory()
    name_set = set(names)
    for block in mem.getBlocks():
        if not block.isInitialized():
            continue
        if block.getName() not in (".rdata", ".data"):
            continue
        data_iter = listing.getDefinedData(block.getStart(), True)
        for d in data_iter:
            if d.getAddress().compareTo(block.getEnd()) > 0:
                break
            try:
                val = d.getValue()
            except Exception:
                continue
            if val is None:
                continue
            sval = str(val)
            if sval in name_set:
                by_value[sval].append(d.getAddress())
    return by_value


def xrefs_to(program, addr):
    refs = []
    it = program.getReferenceManager().getReferencesTo(addr)
    while it.hasNext():
        r = it.next()
        from_addr = r.getFromAddress()
        fn = program.getFunctionManager().getFunctionContaining(from_addr)
        refs.append({
            "from": f"{from_addr}",
            "type": str(r.getReferenceType()),
            "function": (
                {"name": fn.getName(), "entry": f"{fn.getEntryPoint()}"} if fn else None
            ),
        })
    return refs


def decompile(ifc, fn, monitor, timeout=120):
    res = ifc.decompileFunction(fn, timeout, monitor)
    if not res.decompileCompleted():
        return None
    return res.getDecompiledFunction().getC()


def find_property_offset(c_text, prop_name):
    """UE4 property registration emits a call where the LAST integer arg
    is the byte offset. The pattern in Ghidra decompile looks like:

        FUN_xxxxxxxx(L"ControlRotation", ...);
        FUN_yyyyyyyy(lVar7, uVar6, 0, 0x2c4);

    So we find the line containing prop_name and then scan a small window
    of following lines for the next FUN_xxx call with an integer literal
    as the final argument."""
    lines = c_text.splitlines()
    hits = []
    for i, line in enumerate(lines):
        if re.search(rf'"{re.escape(prop_name)}"', line) or re.search(rf'L"{re.escape(prop_name)}"', line):
            window = lines[i:i + 12]
            for j, w in enumerate(window):
                m = re.search(r"FUN_[0-9a-f]+\(\s*[^)]*?,\s*(?:0x)?([0-9a-fA-FxX]+)\s*\)\s*;", w)
                if m:
                    hits.append({
                        "line_no": i + 1,
                        "match_line": line.strip(),
                        "offset_line": w.strip(),
                        "captured_raw": m.group(1),
                    })
                    break
    return hits


def vtable_scan_for_class_string(program, class_string_addr, fp):
    """For a UE4 class registration string like "PlayerCameraManager", scan
    .rdata for an 8-byte pointer to the class's vtable. Heuristic: UE4's
    Z_Construct calls reference the string. The vtable is somewhere else.

    Better strategy: dump the registration function and grep for the actual
    vtable pointer assignment. Skipped here - we instead look at the FUN_
    that creates a UClass and search references for vtable-style patterns.
    """
    return []


def find_constructor_vtable_writes(program, ifc, monitor, class_string_addr, log):
    """Find the C++ constructor for APlayerCameraManager. UE4 constructors
    typically start with `*(void**)this = &vtable;`. We can't trivially
    find the ctor by name, but we can find vtables by:

      1. Look at references to FUN_1414ed7f0 (the Z_Construct for PCM).
         These call sites are usually in a static initializer next to
         the StaticClass() implementation.
      2. Alternative: find a function whose first instruction is
         `lea rax, [vtable]; mov [rcx], rax` - the standard MSVC ctor
         prologue.

    For now we return the analysis info via the log and let the user
    inspect.
    """
    pass


def main():
    if len(sys.argv) < 3:
        print("usage: find_camera_hook_targets.py <binary> <output.json>", file=sys.stderr)
        sys.exit(2)
    binary = sys.argv[1]
    output_path = sys.argv[2]
    out_dir = os.path.dirname(output_path)
    decomp_dir = os.path.join(out_dir, "decompiled")
    os.makedirs(decomp_dir, exist_ok=True)

    project_dir = os.path.join(out_dir, "..", "ghidra-project")

    target_strings = [
        "ControlRotation",
        "Controller",
        "AController",
        "PlayerCameraManager",
        "APlayerCameraManager",
        "UpdateCamera",
        "DoUpdateCamera",
        "BlueprintUpdateCamera",
        "ViewTarget",
        "PlayerController",
        "APlayerController",
        "RootComponent",
    ]

    print(f"Opening {binary} (analyze=False)...")
    with pyghidra.open_program(binary, project_location=project_dir, project_name="abzu", analyze=False) as flat:
        program = flat.getCurrentProgram()
        af = program.getAddressFactory()
        fm = program.getFunctionManager()
        listing = program.getListing()
        memory = program.getMemory()

        report = {
            "program": program.getName(),
            "image_base": f"{program.getImageBase()}",
            "strings": {},
            "control_rotation": {},
            "update_camera": {},
            "pov_rotation_writers": [],
        }

        # 1. Find string locations and xrefs
        strings = collect_string_addresses(program, target_strings)
        for name, addrs in strings.items():
            entry = {"addresses": [f"{a}" for a in addrs], "xrefs": []}
            for a in addrs:
                for r in xrefs_to(program, a):
                    entry["xrefs"].append({"string_at": f"{a}", **r})
            report["strings"][name] = entry
            print(f"  {name}: {len(addrs)} addr(s), {len(entry['xrefs'])} xref(s)")

        ifc = DecompInterface()
        ifc.openProgram(program)
        monitor = ConsoleTaskMonitor()

        # 2. For each xref function on "ControlRotation", decompile and
        #    extract the property offset literal.
        cr_funcs = set()
        for xref in report["strings"]["ControlRotation"]["xrefs"]:
            if xref.get("function") and xref["function"].get("entry"):
                cr_funcs.add(xref["function"]["entry"])
        print(f"\nFunctions referencing 'ControlRotation': {len(cr_funcs)}")
        for ea_hex in sorted(cr_funcs):
            ea = af.getAddress(ea_hex)
            fn = fm.getFunctionAt(ea) or fm.getFunctionContaining(ea)
            if not fn:
                continue
            c = decompile(ifc, fn, monitor)
            if c is None:
                print(f"  [{ea_hex}] decompile failed")
                continue
            out_path = os.path.join(decomp_dir, f"{ea_hex}_{fn.getName()}.c")
            with open(out_path, "w", encoding="utf-8") as f:
                f.write(c)
            offsets = find_property_offset(c, "ControlRotation")
            print(f"  [{ea_hex}] {fn.getName()} - {len(offsets)} candidate offset line(s)")
            for o in offsets:
                print(f"     line {o['line_no']}: {o['offset_line']}")
            report["control_rotation"][ea_hex] = {
                "function_name": fn.getName(),
                "decompiled_file": out_path,
                "offset_candidates": offsets,
            }

        # 3. Find APlayerCameraManager UClass static slot and any vtable
        #    near the BlueprintUpdateCamera entry. Decompile 1414ed7f0
        #    (PCM Z_Construct) and look for `BlueprintUpdateCamera` to
        #    pin its UFunction. The native UpdateCamera is the C++ virtual
        #    not the UFunction - we want vtable.
        #
        #    Strategy: find a function in the binary whose entry is the
        #    address right before/after suspected vtable entries. We
        #    look for a vtable by finding 8 consecutive pointers in
        #    .rdata that all point to functions, with the first one being
        #    a destructor (returns this) and where one slot references
        #    ViewTarget+0xBC0.
        #
        #    Cheap version: enumerate every function that reads or
        #    writes [rcx+0xBC0..rcx+0xBD8] (the ViewTarget block in PCM)
        #    and report them. UpdateCamera will be the prominent one.
        print("\nScanning for functions touching [rcx+0xBC0..0xBE0] (ViewTarget region)...")
        vt_writers = []
        # Iterate over all defined instructions; collect functions where
        # an instruction operand contains a displacement in 0xBC0..0xBE0
        # off RCX or RDI/RBX/RSI (common this-register saves).
        instr_iter = listing.getInstructions(True)
        seen_fns = {}
        scanned = 0
        while instr_iter.hasNext():
            ins = instr_iter.next()
            scanned += 1
            if scanned % 500000 == 0:
                print(f"  ...scanned {scanned} instructions")
            n = ins.getNumOperands()
            for opi in range(n):
                # Look at operand display string - it's fast and avoids
                # parsing the operand objects.
                disp = ins.getDefaultOperandRepresentation(opi)
                if disp is None:
                    continue
                # Match e.g. "[RCX + 0xbc0]", "[RDI + 0xbc4]"
                m = re.search(r"\[\s*(R[A-Z]+)\s*\+\s*0x([0-9a-f]+)\s*\]", disp, re.IGNORECASE)
                if not m:
                    continue
                try:
                    off = int(m.group(2), 16)
                except ValueError:
                    continue
                if 0xBC0 <= off <= 0xBE0:
                    fn = fm.getFunctionContaining(ins.getAddress())
                    if fn is None:
                        continue
                    key = f"{fn.getEntryPoint()}"
                    if key not in seen_fns:
                        seen_fns[key] = {
                            "function": fn.getName(),
                            "entry": key,
                            "hits": [],
                        }
                    if len(seen_fns[key]["hits"]) < 8:
                        seen_fns[key]["hits"].append({
                            "addr": f"{ins.getAddress()}",
                            "mnemonic": ins.getMnemonicString(),
                            "operand": disp,
                            "offset": f"0x{off:x}",
                        })
        vt_writers = list(seen_fns.values())
        # sort by smallest offset hit first (UpdateCamera reads many)
        vt_writers.sort(key=lambda v: -len(v["hits"]))
        print(f"  found {len(vt_writers)} functions touching the ViewTarget region.")
        for v in vt_writers[:30]:
            print(f"    {v['entry']} {v['function']} ({len(v['hits'])} hits)")
        report["pov_rotation_writers"] = vt_writers

        # Decompile the top few candidates
        top = vt_writers[:6]
        print("\nDecompiling top candidates...")
        for v in top:
            ea = af.getAddress(v["entry"])
            fn = fm.getFunctionAt(ea) or fm.getFunctionContaining(ea)
            if not fn:
                continue
            c = decompile(ifc, fn, monitor)
            if c is None:
                print(f"  [{v['entry']}] decompile failed")
                continue
            out_path = os.path.join(decomp_dir, f"{v['entry']}_{fn.getName()}.c")
            with open(out_path, "w", encoding="utf-8") as f:
                f.write(c)
            v["decompiled_file"] = out_path
            print(f"  -> {out_path}")

        with open(output_path, "w") as f:
            json.dump(report, f, indent=2)
        print(f"\nWrote {output_path}")


if __name__ == "__main__":
    main()
