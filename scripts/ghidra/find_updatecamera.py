"""Identify which vtable slot is UpdateCamera.

Strategy:
1. Build the list of POV.Rotation writer functions (already known).
2. For each vtable slot in PCM, decompile and check if it calls any of
   those writers, or if it itself reads ViewTarget at [this+0xBC0].
3. Slots that read ViewTarget+0xBC0 AND take a float arg are UpdateCamera.

Print: vtable index, function VA, evidence snippet.
"""

import json
import os
import sys
import re

import pyghidra

pyghidra.start()

from ghidra.app.decompiler import DecompInterface  # noqa: E402
from ghidra.util.task import ConsoleTaskMonitor  # noqa: E402


# Known POV-region writer functions from prior script.
POV_WRITERS = {
    "14078ddb0", "140fe6410", "1410e5670", "1419d3bc0",
    "140130a80", "1408729d0", "140dd4040", "1411088b0",
    "1412e1ac0", "1413e9410", "14067ff30", "140761610",
    "140ac5a00", "1419d6b40", "141a996d0", "140c62270",
    "14069acb0",  # ctor
}


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

        ifc = DecompInterface()
        ifc.openProgram(program)
        monitor = ConsoleTaskMonitor()

        candidates = []
        for idx, ptr in slots:
            ea = af.getAddress(f"{ptr:x}")
            fn = fm.getFunctionAt(ea) or fm.getFunctionContaining(ea)
            if not fn:
                continue
            res = ifc.decompileFunction(fn, 60, monitor)
            if not res.decompileCompleted():
                continue
            c = res.getDecompiledFunction().getC()

            # Look for any of the writer hexes in the decompile (would
            # appear as FUN_xxxxx calls)
            calls_to_writers = []
            for w in POV_WRITERS:
                if f"FUN_{w}" in c:
                    calls_to_writers.append(w)
            # Look for ViewTarget access at +0xbc0 or +0xbc8..+0xbdc
            uses_viewtarget = bool(re.search(r"\+\s*0xbc[048c]\b|\+\s*0xbd[048c]\b|0x178\s*\)|0x179|0x17a|0x17b|0x17c|0x17d|0x17e|0x17f", c, re.IGNORECASE))
            # The vtable slot 21 had param_1[0x178] reads/writes; that's
            # 0x178*8 = 0xBC0, which is ViewTarget. So include that as
            # evidence.

            if calls_to_writers or uses_viewtarget:
                preview_lines = []
                for line in c.splitlines():
                    if any(w in line for w in POV_WRITERS) or re.search(r"0xbc[048c]|0xbd[048c]|0x178|0x179|0x17a|0x17b|0x17c|0x17d|0x17e|0x17f", line, re.IGNORECASE):
                        preview_lines.append(line.strip())
                candidates.append({
                    "index": idx,
                    "slot_va": f"0x{VTABLE + idx*8:x}",
                    "func_va": f"0x{ptr:x}",
                    "func_name": fn.getName(),
                    "calls_to_pov_writers": calls_to_writers,
                    "uses_viewtarget": uses_viewtarget,
                    "evidence": preview_lines[:10],
                })

        # Print summary
        print(f"\n=== UpdateCamera candidates ({len(candidates)}) ===")
        for c in candidates:
            print(f"\nvtable[{c['index']}] @ {c['slot_va']} -> {c['func_va']} {c['func_name']}")
            print(f"  calls writers: {c['calls_to_pov_writers']}")
            print(f"  uses viewtarget: {c['uses_viewtarget']}")
            for e in c["evidence"]:
                print(f"  | {e}")

        with open(output_path, "w") as f:
            json.dump({"candidates": candidates}, f, indent=2)
        print(f"\nWrote {output_path}")


if __name__ == "__main__":
    main()
