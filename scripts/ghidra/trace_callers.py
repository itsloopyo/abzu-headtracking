"""Trace callers up from POV.Rotation writers to find UpdateCamera/UpdateViewTarget."""

import sys
import os
import pyghidra

pyghidra.start()


def main():
    binary = sys.argv[1]
    out_dir = os.path.dirname(sys.argv[2]) if len(sys.argv) > 2 else "."
    project_dir = os.path.join(out_dir, "..", "ghidra-project")

    # Yaw-only writer 1419d6b40 (writes [rbx+0xbd8])
    # also other POV writers
    targets = [
        0x1419d6b40,  # writes 0xbd8 yaw twice
        0x141a996d0,  # writes 0xbcc, 0xbc8 (Location.Y, X)
        0x140fe6410,  # 4 MOVSS to RSP+ - probably a struct construct
        0x140ac5a00,
        0x1412e1ac0,
        0x14078ddb0,
        0x1410e5670,
        0x1419d3bc0,
        0x140dd4040,
    ]

    with pyghidra.open_program(binary, project_location=project_dir, project_name="abzu", analyze=False) as flat:
        program = flat.getCurrentProgram()
        af = program.getAddressFactory()
        fm = program.getFunctionManager()
        ref_mgr = program.getReferenceManager()

        for target_va in targets:
            print(f"\n=== Callers of 0x{target_va:x} ===")
            addr = af.getAddress(f"{target_va:x}")
            it = ref_mgr.getReferencesTo(addr)
            seen = set()
            while it.hasNext():
                r = it.next()
                from_addr = r.getFromAddress()
                fn = fm.getFunctionContaining(from_addr)
                ref_type = str(r.getReferenceType())
                if "CALL" not in ref_type and "JUMP" not in ref_type and ref_type != "UNCONDITIONAL_CALL":
                    continue
                key = (fn.getEntryPoint() if fn else from_addr, ref_type)
                if key in seen:
                    continue
                seen.add(key)
                fname = fn.getName() if fn else "<no fn>"
                fentry = fn.getEntryPoint() if fn else None
                print(f"  {ref_type} from {from_addr} in {fname} ({fentry})")


if __name__ == "__main__":
    main()
