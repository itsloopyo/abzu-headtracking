"""Find writers to PC + 0x3B0..0x3BB (ControlRotation).
Also find functions whose body accesses [base + 0x3B0] AND [base + 0x418]
(the PlayerCameraManager field; offsets known from notes).
"""
import os, re, sys, json, pyghidra
pyghidra.start()

def main():
    binary = sys.argv[1]
    output = sys.argv[2]
    out_dir = os.path.dirname(output)
    project_dir = os.path.join(out_dir, "..", "ghidra-project")

    with pyghidra.open_program(binary, project_location=project_dir, project_name="abzu", analyze=False) as flat:
        program = flat.getCurrentProgram()
        af = program.getAddressFactory()
        fm = program.getFunctionManager()
        listing = program.getListing()

        targets = {0x3B0, 0x3B4, 0x3B8}  # ControlRotation Pitch, Yaw, Roll
        hits = {}
        instr_iter = listing.getInstructions(True)
        scanned = 0
        while instr_iter.hasNext():
            ins = instr_iter.next()
            scanned += 1
            if scanned % 1000000 == 0:
                print(f"  scanned {scanned}")
            mnem = ins.getMnemonicString()
            if mnem not in ("MOV", "MOVSS"):
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
            if base == "RSP":
                continue
            try:
                off = int(m.group(2), 16)
            except ValueError:
                continue
            if off in targets:
                fn = fm.getFunctionContaining(ins.getAddress())
                if fn is None:
                    continue
                key = f"{fn.getEntryPoint()}"
                if key not in hits:
                    hits[key] = {"name": fn.getName(), "entry": key, "writes": []}
                hits[key]["writes"].append({
                    "addr": f"{ins.getAddress()}",
                    "mnem": mnem,
                    "operand0": op0,
                    "offset": f"0x{off:x}",
                    "base": base,
                })

        out = sorted(hits.values(), key=lambda h: -len(h["writes"]))
        # Filter to functions with all three offsets (full FRotator write)
        full_writers = [h for h in out if len(set(w["offset"] for w in h["writes"])) >= 3]
        print(f"\nFunctions writing full ControlRotation triplet: {len(full_writers)}")
        for h in full_writers[:20]:
            print(f"  {h['entry']} {h['name']} ({len(h['writes'])} writes)")
            for w in h["writes"][:6]:
                print(f"     {w['addr']}: {w['mnem']} {w['operand0']}")

        print(f"\nAll ControlRotation writers (top): {len(out)}")
        for h in out[:30]:
            print(f"  {h['entry']} {h['name']} ({len(h['writes'])} writes, offsets={sorted(set(w['offset'] for w in h['writes']))})")

        with open(output, "w") as f:
            json.dump({"all": out, "full": full_writers}, f, indent=2)


if __name__ == "__main__":
    main()
