"""Find APlayerCameraManager vtable + UpdateCamera index.

Approach:
1. Identify the APlayerCameraManager UClass static slot: DAT_142ad8140 from
   the existing notes is the PCM UClass pointer.
2. Find the constructor for APlayerCameraManager. UE4 ctors set the vtable
   pointer in the first instruction: `lea rax, [vtable]; mov [rcx], rax`.
3. To locate the ctor: scan all functions and check their first instruction
   for `lea r??, [rip + disp]` where the lea-target lies inside .rdata and
   the next instruction stores it to [rcx] or [rbx].
4. Once we have a candidate vtable address, dump the first N pointers.
5. Cross-reference against functions that:
   a) take (this, float) - hard to detect statically without types
   b) write to [this+0xBC4] (POV.Rotation pitch) or [this+0xBC8/0xBCC]
   c) call into FUN_1414eeeac region (ViewTarget construction context)
6. Also find writers to offset 0xBC4/0xBC8/0xBCC specifically.
"""

import json
import os
import re
import sys

import pyghidra

pyghidra.start()

from ghidra.app.decompiler import DecompInterface  # noqa: E402
from ghidra.util.task import ConsoleTaskMonitor  # noqa: E402


def main():
    binary = sys.argv[1]
    output_path = sys.argv[2]
    out_dir = os.path.dirname(output_path)
    decomp_dir = os.path.join(out_dir, "decompiled")
    os.makedirs(decomp_dir, exist_ok=True)
    project_dir = os.path.join(out_dir, "..", "ghidra-project")

    PCM_UCLASS = 0x142ad8140

    print(f"Opening {binary} (analyze=False)...")
    with pyghidra.open_program(binary, project_location=project_dir, project_name="abzu", analyze=False) as flat:
        program = flat.getCurrentProgram()
        af = program.getAddressFactory()
        fm = program.getFunctionManager()
        listing = program.getListing()
        memory = program.getMemory()
        ref_mgr = program.getReferenceManager()

        image_base = program.getImageBase().getOffset()

        report = {
            "image_base": f"0x{image_base:x}",
            "pcm_uclass_static": f"0x{PCM_UCLASS:x}",
            "functions_writing_pov_rotation": [],
            "pcm_uclass_xrefs": [],
            "vtable_candidates": [],
        }

        # ---- 1. Find writers to [this+0xBC4..0xBD8] (POV body) ----
        # Distinguishes from readers; we want store instructions like MOV [rcx+0xBC4], xmm0
        print("Scanning for STORE instructions writing into POV.Rotation block...")
        rotation_offsets = list(range(0xBC8, 0xBE0))  # POV.Rotation Pitch/Yaw/Roll = +0xBCC..+0xBD4
        # Be inclusive: 0xBC0+0x08 = ViewTarget.POV start = 0xBC8; POV.Rotation +0x0C = 0xBD4.
        # POV.Location is 0xBC8..0xBD3, POV.Rotation is 0xBD4..0xBDF. Wait check below.
        # FMinimalViewInfo (UE 4.12): Location(FVector, 12B) at 0x00, Rotation(FRotator, 12B) at 0x0C.
        # ViewTarget.Target ptr at 0x00 of FTViewTarget; POV at 0x08.
        # So POV.Location = +0xBC8; POV.Rotation = +0xBD4.
        target_offsets = set(range(0xBC8, 0xBE0 + 1))

        instr_iter = listing.getInstructions(True)
        store_hits = {}
        scanned = 0
        while instr_iter.hasNext():
            ins = instr_iter.next()
            scanned += 1
            if scanned % 1000000 == 0:
                print(f"  ...scanned {scanned}")
            mnem = ins.getMnemonicString()
            # Stores to memory: MOV [mem], reg; MOVSS [mem], xmm; MOVSD [mem], xmm
            if mnem not in ("MOV", "MOVSS", "MOVSD", "MOVAPS", "MOVUPS"):
                continue
            # Operand 0 must be a memory ref (the destination)
            if ins.getNumOperands() < 2:
                continue
            disp = ins.getDefaultOperandRepresentation(0)
            if disp is None or "[" not in disp:
                continue
            m = re.search(r"\[\s*(R[A-Z]+)\s*\+\s*0x([0-9a-f]+)\s*\]", disp, re.IGNORECASE)
            if not m:
                continue
            try:
                off = int(m.group(2), 16)
            except ValueError:
                continue
            if off in target_offsets:
                fn = fm.getFunctionContaining(ins.getAddress())
                if fn is None:
                    continue
                key = f"{fn.getEntryPoint()}"
                if key not in store_hits:
                    store_hits[key] = {
                        "function": fn.getName(),
                        "entry": key,
                        "hits": [],
                    }
                if len(store_hits[key]["hits"]) < 12:
                    store_hits[key]["hits"].append({
                        "addr": f"{ins.getAddress()}",
                        "mnemonic": mnem,
                        "operand0": disp,
                        "offset": f"0x{off:x}",
                    })

        writers = list(store_hits.values())
        writers.sort(key=lambda v: -len(v["hits"]))
        print(f"Functions writing into +0xBC8..+0xBE0: {len(writers)}")
        for w in writers[:25]:
            print(f"  {w['entry']} {w['function']} ({len(w['hits'])} stores)")
            for h in w["hits"][:4]:
                print(f"     {h['addr']}: {h['mnemonic']} {h['operand0']}")
        report["functions_writing_pov_rotation"] = writers

        # ---- 2. Find PCM ctor by scanning xrefs to PCM UClass static slot ----
        pcm_uclass_addr = af.getAddress(f"{PCM_UCLASS:x}")
        print(f"\nXrefs to PCM UClass slot {pcm_uclass_addr}:")
        it = ref_mgr.getReferencesTo(pcm_uclass_addr)
        seen = set()
        while it.hasNext():
            r = it.next()
            from_addr = r.getFromAddress()
            fn = fm.getFunctionContaining(from_addr)
            if fn is None:
                continue
            key = f"{fn.getEntryPoint()}"
            if key in seen:
                continue
            seen.add(key)
            report["pcm_uclass_xrefs"].append({
                "from": f"{from_addr}",
                "function": fn.getName(),
                "entry": key,
                "ref_type": str(r.getReferenceType()),
            })
            print(f"  {from_addr} in {fn.getName()} ({key})")

        # ---- 3. Decompile candidate writers ----
        ifc = DecompInterface()
        ifc.openProgram(program)
        monitor = ConsoleTaskMonitor()

        print("\nDecompiling top writers...")
        for w in writers[:8]:
            ea = af.getAddress(w["entry"])
            fn = fm.getFunctionAt(ea) or fm.getFunctionContaining(ea)
            if not fn:
                continue
            res = ifc.decompileFunction(fn, 60, monitor)
            if not res.decompileCompleted():
                print(f"  [{w['entry']}] decompile failed")
                continue
            c = res.getDecompiledFunction().getC()
            out_path = os.path.join(decomp_dir, f"{w['entry']}_{fn.getName()}.c")
            with open(out_path, "w", encoding="utf-8") as f:
                f.write(c)
            w["decompiled_file"] = out_path
            print(f"  -> {out_path}")

        with open(output_path, "w") as f:
            json.dump(report, f, indent=2)
        print(f"\nWrote {output_path}")


if __name__ == "__main__":
    main()
