"""Decompile a set of functions identified by FindCameraOffsets and dump
the C-like output to .lab/ghidra-out/decompiled/.

Targets the UClass auto-registration functions that contain UProperty
offset literals - that's where APlayerCameraManager::ViewTarget,
ViewTarget.POV.Rotation etc.'s byte offsets are stored as immediates."""

import json
import os
import sys

import pyghidra
pyghidra.start()
from ghidra.app.decompiler import DecompInterface  # noqa: E402
from ghidra.util.task import ConsoleTaskMonitor    # noqa: E402


TARGET_FUNCTIONS = [
    "140557730",  # "Engine.Engine" xref - likely UEngine init
]


def main():
    if len(sys.argv) < 3:
        print("usage: decompile_functions.py <binary> <output_dir>", file=sys.stderr)
        sys.exit(2)
    binary = sys.argv[1]
    output_dir = sys.argv[2]
    os.makedirs(output_dir, exist_ok=True)

    project_dir = os.path.join(os.path.dirname(output_dir), "..", "ghidra-project")

    with pyghidra.open_program(binary, project_location=project_dir, project_name="abzu", analyze=False) as flat:
        program = flat.getCurrentProgram()
        af = program.getAddressFactory()
        fm = program.getFunctionManager()

        ifc = DecompInterface()
        ifc.openProgram(program)
        monitor = ConsoleTaskMonitor()

        for ea_hex in TARGET_FUNCTIONS:
            ea = af.getAddress(ea_hex)
            fn = fm.getFunctionAt(ea) or fm.getFunctionContaining(ea)
            if fn is None:
                print(f"[skip] no function at {ea_hex}")
                continue
            print(f"[decompile] {fn.getName()} @ {fn.getEntryPoint()}")
            res = ifc.decompileFunction(fn, 60, monitor)
            if not res.decompileCompleted():
                print(f"  failed: {res.getErrorMessage()}")
                continue
            c = res.getDecompiledFunction().getC()
            out_path = os.path.join(output_dir, f"{ea_hex}_{fn.getName()}.c")
            with open(out_path, "w", encoding="utf-8") as f:
                f.write(c)
            print(f"  -> {out_path} ({len(c)} chars)")


if __name__ == "__main__":
    main()
