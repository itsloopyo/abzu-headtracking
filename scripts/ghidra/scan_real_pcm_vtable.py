import os, sys, json
import pyghidra
pyghidra.start()
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

binary, slots_file, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
out_dir = os.path.dirname(out_path); dd=os.path.join(out_dir,"decompiled"); os.makedirs(dd,exist_ok=True)
pj = os.path.join(out_dir, "..", "ghidra-project")
IMAGE_BASE = 0x140000000
POV_LO, POV_HI = 0xBC8, 0xBE4   # POV: Loc 0xBC8, Rot 0xBD4, FOV 0xBE0

slots=[]
for line in open(slots_file):
    p=line.split()
    if len(p)==2: slots.append((int(p[0]), int(p[1],16)))

def instrs(it):
    while it.hasNext(): yield it.next()

with pyghidra.open_program(binary, project_location=pj, project_name="abzu", analyze=False) as flat:
    p=flat.getCurrentProgram(); af=p.getAddressFactory(); fm=p.getFunctionManager(); listing=p.getListing()
    text=None
    for b in p.getMemory().getBlocks():
        if b.getName()==".text": text=(b.getStart().getOffset(), b.getEnd().getOffset())
    ifc=DecompInterface(); ifc.openProgram(p); mon=ConsoleTaskMonitor()
    def a(va): return af.getAddress(hex(va))
    def scan(fn):
        xmm1=False; pov=set(); callees=set(); ncall=0
        for ins in instrs(listing.getInstructions(fn.getBody(), True)):
            t=ins.toString(); m=ins.getMnemonicString().upper()
            if "XMM1" in t: xmm1=True
            if m=="CALL":
                ncall+=1
                for r in ins.getReferencesFrom():
                    to=r.getToAddress().getOffset()
                    if text and text[0]<=to<=text[1]: callees.add(to)
            for tok in t.replace(","," ").replace("["," ").replace("]"," ").split():
                if tok.startswith("0x"):
                    try: v=int(tok,16)
                    except: continue
                    if POV_LO<=v<=POV_HI: pov.add(v)
        return xmm1,pov,callees,ncall
    res=[]
    seen=set()
    for slot,rva in slots:
        va=IMAGE_BASE+rva
        if va in seen: 
            continue
        seen.add(va)
        fn=fm.getFunctionAt(a(va))
        if not fn: continue
        xmm1,pov,callees,ncall=scan(fn)
        cpov=set()
        for c in list(callees)[:24]:
            cf=fm.getFunctionAt(a(c))
            if cf: _,cp,_,_=scan(cf); cpov|=cp
        allp=pov|cpov
        score=(6 if 0xBD4 in pov else 0)+(4 if 0xBC8 in pov else 0)+(3 if 0xBD4 in cpov else 0)+(2 if xmm1 else 0)+min(len(allp),4)
        if score>0 or 0xBD4 in allp:
            res.append({"slot":slot,"rva":hex(rva),"va":hex(va),"name":fn.getName(),
                        "xmm1":xmm1,"own_pov":sorted(hex(x) for x in pov),
                        "callee_pov":sorted(hex(x) for x in cpov),"ncalls":ncall,"score":score})
    res.sort(key=lambda r:r["score"],reverse=True)
    for r in res[:8]:
        fn=fm.getFunctionAt(a(int(r["va"],16)))
        rr=ifc.decompileFunction(fn,60,mon)
        if rr.decompileCompleted():
            fp=os.path.join(dd,f"real_slot{r['slot']:03d}_{r['va'][2:]}.c")
            open(fp,"w",encoding="utf-8").write(rr.getDecompiledFunction().getC()); r["decompiled"]=fp
    json.dump({"vtable_rva":"0x1EEC868","ranked":res}, open(out_path,"w"), indent=1)
    print(f"scanned {len(seen)} unique fns; candidates: {len(res)}")
    for r in res[:10]:
        print(f"  score={r['score']:2d} slot={r['slot']:3d} {r['name']} va={r['va']} xmm1={r['xmm1']} own={r['own_pov']} callee={r['callee_pov']} ncalls={r['ncalls']}")
