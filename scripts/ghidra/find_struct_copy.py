"""Find MOVUPS/MOVAPS writes that span the FMinimalViewInfo block at +0xBC8.
A typical FMinimalViewInfo struct-copy emits MOVUPS [rdi+0xbc8], xmm0;
MOVUPS [rdi+0xbd8], xmm1; etc. Find them.

Also include any function that calls memcpy with dst including +0xBC8.
"""

import re, sys, os, json
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
        listing = program.getListing()

        # Find MOVUPS / MOVAPS / MOVDQU writes to base + 0xBC8 or +0xBD4
        # (FMinimalViewInfo block - includes Rotation+Pitch)
        targets = {0xBC8, 0xBD4, 0xBD8, 0xBE0, 0xBC0}
        hits = {}
        instr_iter = listing.getInstructions(True)
        scanned = 0
        while instr_iter.hasNext():
            ins = instr_iter.next()
            scanned += 1
            if scanned % 1000000 == 0:
                print(f"  scanned {scanned}")
            mnem = ins.getMnemonicString()
            if mnem not in ("MOVUPS", "MOVAPS", "MOVDQU", "MOVDQA"):
                continue
            if ins.getNumOperands() < 2:
                continue
            op0 = ins.getDefaultOperandRepresentation(0)
            if op0 is None or "[" not in op0:
                continue
            m = re.search(r"\[\s*(R[A-Z0-9]+)\s*\+\s*0x([0-9a-fA-F]+)\s*\]", op0)
            if not m:
                continue
            base = m.group(1).upper()
            if base in ("RSP",):
                continue
            try:
                off = int(m.group(2), 16)
            except ValueError:
                continue
            if off not in targets:
                continue
            fn = fm.getFunctionContaining(ins.getAddress())
            if fn is None:
                continue
            key = f"{fn.getEntryPoint()}"
            if key not in hits:
                hits[key] = {"name": fn.getName(), "entry": key, "writes": []}
            hits[key]["writes"].append({
                "addr": f"{ins.getAddress()}",
                "mnemonic": mnem,
                "operand0": op0,
                "offset": f"0x{off:x}",
                "base": base,
            })

        out = sorted(hits.values(), key=lambda h: -len(h["writes"]))
        print(f"\nFunctions with MOVUPS/MOVAPS into PCM rotation block: {len(out)}")
        for h in out[:40]:
            print(f"  {h['entry']} {h['name']} ({len(h['writes'])} 16B writes)")
            for w in h["writes"][:4]:
                print(f"     {w['addr']}: {w['mnemonic']} {w['operand0']}")

        with open(output_path, "w") as f:
            json.dump(out, f, indent=2)


if __name__ == "__main__":
    main()
