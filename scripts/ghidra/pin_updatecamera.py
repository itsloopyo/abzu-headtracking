"""Pin APlayerCameraManager::UpdateCamera (and DoUpdateCamera / UpdateViewTarget)
in the PCM vtable.

PCM vtable is at 0x14206b400. PCM extends AActor, whose vtable is long, so we
walk slots until the pointer stops looking like a .text function (the earlier
85-slot dump truncated before the PCM-specific camera virtuals).

UpdateCamera itself does NOT write the POV instance offsets - it delegates to
DoUpdateCamera -> UpdateViewTarget (writes ViewTarget.POV at this+0xBC8..) and
FillCameraCache (writes CameraCachePrivate). So for each slot we scan the
function AND its direct callees one level deep for writes into the PCM instance
POV band (0xB00..0xC20 with base = the `this` register). The slot whose own
body or callees write 0xBD4/0xBC8 and which takes a float (XMM1) is the camera
update path.
"""

import json
import os
import sys

import pyghidra
pyghidra.start()

from ghidra.app.decompiler import DecompInterface  # noqa: E402
from ghidra.util.task import ConsoleTaskMonitor    # noqa: E402

VTABLE_VA = 0x14206b400
MAX_SLOTS = 170
POV_LO, POV_HI = 0xB00, 0xC20


def instrs_iter(it):
    while it.hasNext():
        yield it.next()


def main():
    binary = sys.argv[1]
    output_path = sys.argv[2]
    out_dir = os.path.dirname(output_path)
    decomp_dir = os.path.join(out_dir, "decompiled")
    os.makedirs(decomp_dir, exist_ok=True)
    project_dir = os.path.join(out_dir, "..", "ghidra-project")

    with pyghidra.open_program(binary, project_location=project_dir,
                               project_name="abzu", analyze=False) as flat:
        program = flat.getCurrentProgram()
        af = program.getAddressFactory()
        fm = program.getFunctionManager()
        listing = program.getListing()
        mem = program.getMemory()

        text = None
        for blk in program.getMemory().getBlocks():
            if blk.getName() == ".text":
                text = (blk.getStart().getOffset(), blk.getEnd().getOffset())
        assert text, "no .text"

        ifc = DecompInterface()
        ifc.openProgram(program)
        monitor = ConsoleTaskMonitor()

        def addr(va):
            return af.getAddress(hex(va))

        def read_ptr(va):
            return mem.getLong(addr(va)) & 0xFFFFFFFFFFFFFFFF

        def scan_fn(fn):
            """Return (xmm1, pov_offsets:set, callees:set, n_calls, n_ins)."""
            xmm1 = False
            pov = set()
            callees = set()
            n_calls = 0
            n_ins = 0
            for ins in instrs_iter(listing.getInstructions(fn.getBody(), True)):
                n_ins += 1
                txt = ins.toString()
                mnem = ins.getMnemonicString().upper()
                if "XMM1" in txt:
                    xmm1 = True
                if mnem == "CALL":
                    n_calls += 1
                    for ref in ins.getReferencesFrom():
                        t = ref.getToAddress().getOffset()
                        if text[0] <= t <= text[1]:
                            callees.add(t)
                # only count instance-relative displacement writes/reads in band
                for tok in txt.replace(",", " ").replace("[", " ").replace("]", " ").split():
                    if tok.startswith("0x"):
                        try:
                            v = int(tok, 16)
                        except ValueError:
                            continue
                        if POV_LO <= v <= POV_HI:
                            pov.add(v)
            return xmm1, pov, callees, n_calls, n_ins

        results = []
        for i in range(MAX_SLOTS):
            try:
                fn_va = read_ptr(VTABLE_VA + i * 8)
            except Exception:
                break
            if not (text[0] <= fn_va <= text[1]):
                # left the code section -> end of vtable
                break
            fn = fm.getFunctionAt(addr(fn_va))
            if fn is None:
                results.append({"slot": i, "func_va": f"0x{fn_va:x}", "func_name": None,
                                "score": 0, "note": "no function"})
                continue

            xmm1, pov, callees, n_calls, n_ins = scan_fn(fn)

            # one level deep: union callees' POV-band touches
            callee_pov = set()
            for c in list(callees)[:30]:
                cfn = fm.getFunctionAt(addr(c))
                if cfn is None:
                    continue
                _, cpov, _, _, _ = scan_fn(cfn)
                callee_pov |= cpov

            all_pov = pov | callee_pov
            score = 0
            if xmm1:
                score += 2
            score += min(len(all_pov), 6)
            if 0xBD4 in all_pov:
                score += 6
            if 0xBC8 in all_pov:
                score += 4
            if 0xBD4 in pov or 0xBC8 in pov:
                score += 3  # writes POV directly in own body
            if 2 <= n_calls <= 14:
                score += 1

            results.append({
                "slot": i,
                "func_va": f"0x{fn_va:x}",
                "func_name": fn.getName(),
                "xmm1_float_arg": xmm1,
                "own_pov": sorted(f"0x{o:x}" for o in pov),
                "callee_pov": sorted(f"0x{o:x}" for o in callee_pov),
                "n_calls": n_calls,
                "n_insns": n_ins,
                "score": score,
            })

        ranked = sorted([r for r in results if r.get("func_name")],
                        key=lambda r: r["score"], reverse=True)

        # decompile the top 6 for eyeballing
        for r in ranked[:6]:
            fn = fm.getFunctionAt(addr(int(r["func_va"], 16)))
            try:
                res = ifc.decompileFunction(fn, 60, monitor)
                if res.decompileCompleted():
                    c = res.getDecompiledFunction().getC()
                    p = os.path.join(decomp_dir, f"uc_slot_{r['slot']:03d}_{r['func_va'][2:]}.c")
                    open(p, "w", encoding="utf-8").write(c)
                    r["decompiled_file"] = p
            except Exception as e:
                r["decompile_error"] = str(e)

        with open(output_path, "w", encoding="utf-8") as f:
            json.dump({"vtable": f"0x{VTABLE_VA:x}", "n_slots_walked": len(results),
                       "ranked": ranked}, f, indent=1)
        print(f"wrote {output_path}; walked {len(results)} slots; top:")
        for r in ranked[:10]:
            print(f"  score={r['score']:2d} slot={r['slot']:3d} {r['func_name']} "
                  f"xmm1={r['xmm1_float_arg']} own={r['own_pov']} callee={r['callee_pov']} calls={r['n_calls']}")


if __name__ == "__main__":
    main()
