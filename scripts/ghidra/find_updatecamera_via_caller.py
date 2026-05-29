"""Pin APlayerCameraManager::UpdateCamera by its caller.

Decoupling needs a game-thread function to hook that runs once per frame after
the camera POV is computed. The canonical UE4 caller is
APlayerController::UpdateCameraManager(float DeltaSeconds):

    PCM = this->PlayerCameraManager;          // load [PC + 0x418]
    PCM->UpdateCamera(DeltaSeconds);          // virtual call (**(*PCM + SLOT))(PCM, dt)

So we scan every function for: a load of [reg + 0x418] into a register, followed
within the same function by an indirect CALL through that register's pointed-to
vtable at a fixed displacement, with a float live in XMM (the DeltaTime arg).
The displacement / 8 is UpdateCamera's vtable index; resolving it against the
PCM vtable (0x14206b400) yields UpdateCamera's address.

We corroborate by decompiling each caller + the resolved callee and dumping
them for eyeballing. PC->PlayerCameraManager offset 0x418 is confirmed in
.lab/NOTES.md (FUN_1414efc30).
"""

import json
import os
import sys

import pyghidra
pyghidra.start()

from ghidra.app.decompiler import DecompInterface  # noqa: E402
from ghidra.util.task import ConsoleTaskMonitor    # noqa: E402

PCM_VTABLE = 0x14206b400
PCM_PTR_OFFSET = 0x418  # APlayerController::PlayerCameraManager


def instrs(it):
    while it.hasNext():
        yield it.next()


def main():
    binary, output_path = sys.argv[1], sys.argv[2]
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
        for blk in mem.getBlocks():
            if blk.getName() == ".text":
                text = (blk.getStart().getOffset(), blk.getEnd().getOffset())

        def a(va):
            return af.getAddress(hex(va))

        pcm_vt_funcs = []
        for i in range(85):
            v = mem.getLong(a(PCM_VTABLE + i * 8)) & 0xFFFFFFFFFFFFFFFF
            pcm_vt_funcs.append(v)

        callers = []
        fiter = fm.getFunctions(True)
        for fn in fiter:
            body_ins = list(instrs(listing.getInstructions(fn.getBody(), True)))
            # find any instruction that references displacement 0x418
            loads_418 = False
            for ins in body_ins:
                txt = ins.toString()
                if "0x418" in txt and "[" in txt and ins.getMnemonicString().upper() == "MOV":
                    loads_418 = True
                    break
            if not loads_418:
                continue

            # collect indirect-call displacements + whether XMM (float) appears nearby
            vcall_disps = []
            xmm_seen_recent = False
            for idx, ins in enumerate(body_ins):
                t = ins.toString()
                if "XMM" in t:
                    xmm_seen_recent = True
                m = ins.getMnemonicString().upper()
                if m == "CALL" and "[" in t:
                    # indirect call; capture displacement if present
                    disp = None
                    for tok in t.replace(",", " ").replace("[", " ").replace("]", " ").split():
                        if tok.startswith("0x"):
                            try:
                                disp = int(tok, 16)
                            except ValueError:
                                pass
                    vcall_disps.append({"disp": disp, "xmm_recent": xmm_seen_recent,
                                        "addr": ins.getAddress().toString(), "text": t})
                # reset xmm window every ~8 insns
                if idx % 8 == 0:
                    xmm_seen_recent = False

            if vcall_disps:
                callers.append({
                    "func": fn.getName(),
                    "entry": fn.getEntryPoint().toString(),
                    "vcalls": vcall_disps,
                })

        # Tally candidate UpdateCamera slots: indirect calls with a float arg whose
        # displacement, /8, indexes a real PCM vtable function.
        slot_votes = {}
        for c in callers:
            for vc in c["vcalls"]:
                d = vc["disp"]
                if d is None or d <= 0 or d % 8 != 0:
                    continue
                slot = d // 8
                if slot >= 85:
                    continue
                key = slot
                slot_votes.setdefault(key, {"slot": slot, "disp": hex(d),
                                            "pcm_func": hex(pcm_vt_funcs[slot]),
                                            "votes": 0, "float_votes": 0, "from": []})
                slot_votes[key]["votes"] += 1
                if vc["xmm_recent"]:
                    slot_votes[key]["float_votes"] += 1
                if len(slot_votes[key]["from"]) < 8:
                    slot_votes[key]["from"].append({"caller": c["func"], "at": vc["addr"]})

        ranked = sorted(slot_votes.values(),
                        key=lambda s: (s["float_votes"], s["votes"]), reverse=True)

        # decompile the top few callees + their callers for eyeballing
        ifc = DecompInterface()
        ifc.openProgram(program)
        mon = ConsoleTaskMonitor()
        for s in ranked[:6]:
            fn = fm.getFunctionAt(a(int(s["pcm_func"], 16)))
            if fn is None:
                continue
            res = ifc.decompileFunction(fn, 60, mon)
            if res.decompileCompleted():
                p = os.path.join(decomp_dir, f"uccand_slot{s['slot']:03d}_{s['pcm_func'][2:]}.c")
                open(p, "w", encoding="utf-8").write(res.getDecompiledFunction().getC())
                s["decompiled_file"] = p

        with open(output_path, "w", encoding="utf-8") as f:
            json.dump({"pcm_vtable": hex(PCM_VTABLE),
                       "n_callers_touching_0x418": len(callers),
                       "ranked_slots": ranked}, f, indent=1)
        print(f"wrote {output_path}; callers touching 0x418: {len(callers)}")
        for s in ranked[:10]:
            print(f"  slot={s['slot']:3d} disp={s['disp']} pcm_func={s['pcm_func']} "
                  f"float_votes={s['float_votes']} votes={s['votes']}")


if __name__ == "__main__":
    main()
