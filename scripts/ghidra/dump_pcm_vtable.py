"""Dump APlayerCameraManager vtable at 0x14206b400 and identify UpdateCamera.

UpdateCamera in UE 4.12 APlayerCameraManager:
  - signature: void UpdateCamera(float DeltaTime)
  - body: calls DoUpdateCamera or runs UpdateViewTarget(...)
  - typically calls into FUN_1414eeeac region or similar
  - is preceded in vtable by lots of AActor virtuals (Tick, BeginPlay, etc.)

For each vtable entry we decompile a short prologue and look for telltale
behaviour. Output a JSON listing all vtable slots.

We dump up to 200 slots; UE 4.12 AActor + APlayerCameraManager vtable is
~120 slots.
"""

import json
import os
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

    VTABLE = 0x14206b400

    print(f"Opening {binary} (analyze=False)...")
    with pyghidra.open_program(binary, project_location=project_dir, project_name="abzu", analyze=False) as flat:
        program = flat.getCurrentProgram()
        af = program.getAddressFactory()
        fm = program.getFunctionManager()
        memory = program.getMemory()

        report = {
            "vtable": f"0x{VTABLE:x}",
            "slots": [],
        }

        ifc = DecompInterface()
        ifc.openProgram(program)
        monitor = ConsoleTaskMonitor()

        # Vtable pointers are 8 bytes each, little-endian.
        N_SLOTS = 200
        for i in range(N_SLOTS):
            slot_va = VTABLE + i * 8
            slot_addr = af.getAddress(f"{slot_va:x}")
            try:
                lo = memory.getInt(slot_addr) & 0xFFFFFFFF
                hi = memory.getInt(af.getAddress(f"{slot_va + 4:x}")) & 0xFFFFFFFF
                ptr = (hi << 32) | lo
            except Exception as e:
                print(f"  slot {i}: read failed - {e}")
                break
            if ptr == 0 or ptr < 0x140000000 or ptr > 0x143000000:
                # End of vtable or pad.
                print(f"  slot {i}: ptr=0x{ptr:x} out of range, stopping.")
                break
            ptr_addr = af.getAddress(f"{ptr:x}")
            fn = fm.getFunctionAt(ptr_addr) or fm.getFunctionContaining(ptr_addr)
            slot = {
                "index": i,
                "slot_addr": f"0x{slot_va:x}",
                "ptr": f"0x{ptr:x}",
                "function": fn.getName() if fn else None,
            }
            report["slots"].append(slot)
            if fn is None:
                print(f"  slot {i}: 0x{ptr:x} (no function)")
                continue
            print(f"  slot {i}: 0x{ptr:x} {fn.getName()}")

        # Decompile each slot's function (first 40 lines to find UpdateCamera)
        print("\nDecompiling vtable functions...")
        for slot in report["slots"]:
            if slot["function"] is None:
                continue
            ea = af.getAddress(slot["ptr"][2:])
            fn = fm.getFunctionAt(ea) or fm.getFunctionContaining(ea)
            if not fn:
                continue
            res = ifc.decompileFunction(fn, 30, monitor)
            if not res.decompileCompleted():
                continue
            c = res.getDecompiledFunction().getC()
            short_c = "\n".join(c.splitlines()[:40])
            slot["preview"] = short_c
            # Save full decompile too
            out_path = os.path.join(decomp_dir, f"vtable_{slot['index']:03d}_{slot['ptr'][2:]}_{fn.getName()}.c")
            with open(out_path, "w", encoding="utf-8") as f:
                f.write(c)
            slot["decompiled_file"] = out_path

        with open(output_path, "w") as f:
            json.dump(report, f, indent=2)
        print(f"Wrote {output_path}")


if __name__ == "__main__":
    main()
