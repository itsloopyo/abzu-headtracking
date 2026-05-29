"""Hunt for UpdateCamera and find what writes POV.Rotation per-frame.

Strategy:
- The PCM is at some heap address. Per-frame writes happen via the vtable.
- A leaf write happens at MOVSS [rcx + 0xbd4..0xbdc].
- We narrow to writes whose BASE register holds 'this' (the PCM instance):
  the function takes 'this' in RCX, may save to RBX/RDI/RSI.
- We also include MOVSS writes (most rotation field writes are MOVSS xmm0).

Critically: we filter to functions whose body has both:
  - a write to [base + 0xbd4] OR [base + 0xbd8] OR [base + 0xbdc]
  - a read or write to [base + 0xbc0..0xbc8] in the same function (confirming
    'base' really is the PCM)

Also: search the binary for strings "DoUpdateCamera", "UpdateViewTarget",
"BlendPostProcessSettings" and similar. Even shipping UE 4.12 leaves some
debug strings.
"""

import os
import re
import sys
import json
import pyghidra

pyghidra.start()


def main():
    binary = sys.argv[1]
    output_path = sys.argv[2]
    out_dir = os.path.dirname(output_path)
    project_dir = os.path.join(out_dir, "..", "ghidra-project")

    with pyghidra.open_program(binary, project_location=project_dir, project_name="abzu", analyze=False) as flat:
        program = flat.getCurrentProgram()
        af = program.getAddressFactory()
        fm = program.getFunctionManager()
        memory = program.getMemory()
        listing = program.getListing()

        # ---- search strings ----
        searches = [
            "DoUpdateCamera", "UpdateViewTarget", "BlendPostProcessSettings",
            "ApplyCameraModifiers", "GetCameraView", "FillCameraCache",
            "EndCameraFade", "DisplayDebug", "PlayerCameraManager.cpp",
            "FMinimalViewInfo", "CalcCamera", "ProcessViewRotation",
            "CalcWeaponSwayAndShake", "OnBecomeActiveCamera",
            "GetCameraViewPoint", "GetActorEyesViewPoint",
        ]
        searches_set = set(searches)
        hits = {}
        block_iter = memory.getBlocks()
        for block in block_iter:
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
                if sval in searches_set:
                    hits.setdefault(sval, []).append(f"{d.getAddress()}")

        print("Strings found:")
        for name, addrs in hits.items():
            print(f"  {name}: {addrs}")

        # ---- now scan functions for writes that satisfy our condition ----
        # build: function -> list of stores at +0xbd4/8/c with base reg
        rot_offsets = {0xBD4, 0xBD8, 0xBDC}
        vt_ptr_offsets = set(range(0xBC0, 0xBE0))

        # per-fn collected
        fn_stores = {}  # entry -> {writes:[], reads:[], base_regs:set}

        instr_iter = listing.getInstructions(True)
        scanned = 0
        while instr_iter.hasNext():
            ins = instr_iter.next()
            scanned += 1
            if scanned % 1000000 == 0:
                print(f"  scanned {scanned}")
            mnem = ins.getMnemonicString()
            n = ins.getNumOperands()
            for opi in range(n):
                op = ins.getDefaultOperandRepresentation(opi)
                if op is None or "[" not in op:
                    continue
                m = re.search(r"\[\s*(R[A-Z0-9]+)\s*\+\s*0x([0-9a-fA-F]+)\s*\]", op)
                if not m:
                    continue
                base = m.group(1).upper()
                # skip stack
                if base in ("RSP", "RBP_FP"):
                    continue
                try:
                    off = int(m.group(2), 16)
                except ValueError:
                    continue
                fn = fm.getFunctionContaining(ins.getAddress())
                if fn is None:
                    continue
                key = f"{fn.getEntryPoint()}"
                fs = fn_stores.setdefault(key, {"name": fn.getName(), "writes": [], "vt_refs": [], "bases": set()})
                is_store = (opi == 0 and mnem in ("MOV", "MOVSS", "MOVSD", "MOVAPS", "MOVUPS"))
                if off in rot_offsets and is_store:
                    fs["writes"].append({
                        "addr": f"{ins.getAddress()}",
                        "mnemonic": mnem,
                        "operand0": op,
                        "offset": f"0x{off:x}",
                        "base": base,
                    })
                    fs["bases"].add(base)
                if off in vt_ptr_offsets:
                    fs["vt_refs"].append({
                        "addr": f"{ins.getAddress()}",
                        "mnemonic": mnem,
                        "operand": op,
                        "offset": f"0x{off:x}",
                        "base": base,
                    })

        # filter: writes to rotation triplet
        candidates = []
        for entry, info in fn_stores.items():
            if not info["writes"]:
                continue
            # confirm same base register also accesses 0xBC0/0xBC8
            base_in_writes = info["bases"]
            confirmed = False
            for vr in info["vt_refs"]:
                if vr["base"] in base_in_writes:
                    confirmed = True
                    break
            candidates.append({
                "entry": entry,
                "name": info["name"],
                "writes": info["writes"][:8],
                "vt_refs": info["vt_refs"][:8],
                "bases": sorted(info["bases"]),
                "confirmed_base_match": confirmed,
                "n_writes": len(info["writes"]),
            })

        candidates.sort(key=lambda c: (not c["confirmed_base_match"], -c["n_writes"]))

        print("\n=== Functions writing rotation triplet 0xBD4/0xBD8/0xBDC ===")
        for c in candidates[:30]:
            mark = "*" if c["confirmed_base_match"] else " "
            print(f"{mark} {c['entry']} {c['name']}: {c['n_writes']} writes (bases {c['bases']})")
            for w in c["writes"][:5]:
                print(f"     {w['addr']}: {w['mnemonic']} {w['operand0']}")

        with open(output_path, "w") as f:
            json.dump({"strings": hits, "candidates": candidates}, f, indent=2)


if __name__ == "__main__":
    main()
