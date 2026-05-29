"""Try harder to find UpdateCamera in PCM vtable.

Approach: for every vtable slot, check the function's disassembly:
  - reads from [rcx + 0xBC0..0xBE0] (the ViewTarget block of PCM)
  - takes a float arg (xmm1 referenced)
  - calls another virtual via [vtable] (UpdateCamera typically calls
    DoUpdateCamera as a vtable dispatch)
  - or calls UpdateViewTarget, which dispatches more virtuals

Output: per-slot summary with offsets touched + xmm1 use + vtable calls.
"""

import json
import os
import re
import sys

import pyghidra

pyghidra.start()


def main():
    binary = sys.argv[1]
    output_path = sys.argv[2]
    out_dir = os.path.dirname(output_path)
    project_dir = os.path.join(out_dir, "..", "ghidra-project")

    VTABLE = 0x14206b400

    with pyghidra.open_program(binary, project_location=project_dir, project_name="abzu", analyze=False) as flat:
        program = flat.getCurrentProgram()
        af = program.getAddressFactory()
        fm = program.getFunctionManager()
        memory = program.getMemory()
        listing = program.getListing()

        # Read vtable
        slots = []
        for i in range(120):
            slot_va = VTABLE + i * 8
            lo = memory.getInt(af.getAddress(f"{slot_va:x}")) & 0xFFFFFFFF
            hi = memory.getInt(af.getAddress(f"{slot_va + 4:x}")) & 0xFFFFFFFF
            ptr = (hi << 32) | lo
            if ptr < 0x140000000 or ptr > 0x143000000:
                break
            slots.append((i, ptr))

        # Iterate each slot, analyze instructions
        out = []
        for idx, ptr in slots:
            ea = af.getAddress(f"{ptr:x}")
            fn = fm.getFunctionAt(ea) or fm.getFunctionContaining(ea)
            if not fn:
                continue
            body_iter = fn.getBody().getAddressRanges()
            xmm1_used = False
            vt_offsets = set()
            vt_calls = 0
            insns_scanned = 0
            # Iterate instructions in function body
            body = fn.getBody()
            it = listing.getInstructions(body, True)
            while it.hasNext():
                ins = it.next()
                insns_scanned += 1
                if insns_scanned > 5000:
                    break
                mnem = ins.getMnemonicString()
                # Check for xmm1 usage anywhere in operands
                for opi in range(ins.getNumOperands()):
                    op = ins.getDefaultOperandRepresentation(opi)
                    if op is None:
                        continue
                    if "XMM1" in op.upper() and "XMM10" not in op.upper() and "XMM11" not in op.upper() and "XMM12" not in op.upper() and "XMM13" not in op.upper() and "XMM14" not in op.upper() and "XMM15" not in op.upper():
                        xmm1_used = True
                    m = re.findall(r"\[\s*R[A-Z]+\s*\+\s*0x([0-9a-fA-F]+)\s*\]", op)
                    for hx in m:
                        off = int(hx, 16)
                        if 0xBC0 <= off <= 0xBE0:
                            vt_offsets.add(off)
                # call qword ptr [rax + 0x???] = vtable call
                if mnem == "CALL":
                    op = ins.getDefaultOperandRepresentation(0) or ""
                    if "[" in op and "+" in op and "0x" in op:
                        vt_calls += 1

            entry = {
                "index": idx,
                "func_va": f"0x{ptr:x}",
                "func_name": fn.getName(),
                "xmm1_used": xmm1_used,
                "view_target_offsets": [f"0x{o:x}" for o in sorted(vt_offsets)],
                "vtable_call_count": vt_calls,
                "size_insns": insns_scanned,
            }
            out.append(entry)

        # Print prioritized: takes float AND touches ViewTarget AND non-trivial
        out.sort(key=lambda e: (
            not (e["xmm1_used"] and e["view_target_offsets"]),
            -len(e["view_target_offsets"]),
        ))

        print("Top UpdateCamera candidates (xmm1 used + ViewTarget access):")
        for e in out:
            if not (e["xmm1_used"] and e["view_target_offsets"]):
                continue
            print(f"  vtable[{e['index']:3d}] {e['func_va']} {e['func_name']}: "
                  f"VT offsets {e['view_target_offsets']}, vtcalls={e['vtable_call_count']}, "
                  f"insns={e['size_insns']}")

        print("\nAll slots reading ViewTarget block:")
        for e in out:
            if e["view_target_offsets"]:
                print(f"  vtable[{e['index']:3d}] {e['func_va']} {e['func_name']}: "
                      f"xmm1={e['xmm1_used']}, VT offsets {e['view_target_offsets']}, "
                      f"vtcalls={e['vtable_call_count']}, insns={e['size_insns']}")

        with open(output_path, "w") as f:
            json.dump({"slots": out}, f, indent=2)
        print(f"\nWrote {output_path}")


if __name__ == "__main__":
    main()
