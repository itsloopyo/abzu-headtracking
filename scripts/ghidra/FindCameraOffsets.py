# Ghidra headless script - find UE 4.12 camera offsets in AbzuGame-Win64-Shipping.exe.
#
# Strategy:
# 1. Find well-known UE class-name strings in .rdata.
# 2. For each, dump all xrefs (functions that reference the string).
# 3. For APlayerCameraManager / APlayerController xrefs, scan the function
#    body for member-access patterns: `mov reg, [rcx+disp32]` and similar,
#    where `disp32` is a candidate offset to ViewTarget / POV / Rotation.
# 4. Look for the GEngine global write pattern: `mov [rip+disp32], rcx`
#    near the start of UEngine-related functions.
# 5. Dump everything to .lab/ghidra-out/abzu-offsets.json.
#
# Jython 2.7. Run via:
#   analyzeHeadless <project-dir> <project-name> -import AbzuGame-Win64-Shipping.exe
#                   -postScript FindCameraOffsets.py <output-json>

import json
import sys

from ghidra.app.script import GhidraScript
from ghidra.program.model.symbol import RefType, SourceType
from ghidra.program.model.listing import CodeUnit
from ghidra.util.task import TaskMonitor

UE_STRINGS = [
    "PlayerCameraManager",
    "APlayerCameraManager",
    "UpdateCamera",
    "DoUpdateCamera",
    "ApplyCameraModifiers",
    "GetCameraViewPoint",
    "ViewTarget",
    "BlendViewTarget",
    "SetViewTarget",
    "FMinimalViewInfo",
    "PlayerController",
    "APlayerController",
    "GetPlayerViewPoint",
    "UpdateRotation",
    "LocalPlayer",
    "ULocalPlayer",
    "GameInstance",
    "UGameInstance",
    "GEngine",
    "UEngine",
    "InitializeObjectReferences",
]

def find_string_address(program, s):
    listing = program.getListing()
    # Strings live in .rdata as UTF-16 (TCHAR) for UE. Try both.
    found = []
    mem = program.getMemory()
    for block in mem.getBlocks():
        if not block.isInitialized():
            continue
        name = block.getName()
        if name not in (".rdata", ".data", ".text"):
            continue
        try:
            data_iter = listing.getDefinedData(block.getStart(), True)
            for d in data_iter:
                if d.getAddress().compareTo(block.getEnd()) > 0:
                    break
                try:
                    val = d.getValue()
                except:
                    continue
                if val is None:
                    continue
                if isinstance(val, unicode) or isinstance(val, str):
                    if val == s:
                        found.append(str(d.getAddress()))
        except Exception as ex:
            pass
    return found

def xrefs_to(program, addr_str):
    addr = program.getAddressFactory().getAddress(addr_str)
    if addr is None:
        return []
    refs = []
    rm = program.getReferenceManager()
    it = rm.getReferencesTo(addr)
    while it.hasNext():
        r = it.next()
        refs.append({
            "from": str(r.getFromAddress()),
            "type": str(r.getReferenceType()),
        })
    return refs

def containing_function(program, addr_str):
    addr = program.getAddressFactory().getAddress(addr_str)
    if addr is None:
        return None
    fm = program.getFunctionManager()
    func = fm.getFunctionContaining(addr)
    if func is None:
        return None
    return {
        "name": func.getName(),
        "entry": str(func.getEntryPoint()),
    }

def main():
    args = getScriptArgs()
    if len(args) < 1:
        print("ERROR: missing output JSON path")
        sys.exit(2)
    output_path = args[0]

    program = getCurrentProgram()
    out = {
        "program": program.getName(),
        "image_base": str(program.getImageBase()),
        "strings": {},
    }

    print("Scanning %d UE strings..." % len(UE_STRINGS))
    for s in UE_STRINGS:
        print("  " + s)
        addresses = find_string_address(program, s)
        entry = {"addresses": [], "xrefs": []}
        for a in addresses:
            entry["addresses"].append(a)
            for r in xrefs_to(program, a):
                fn = containing_function(program, r["from"])
                entry["xrefs"].append({
                    "string_at": a,
                    "from": r["from"],
                    "type": r["type"],
                    "function": fn,
                })
        out["strings"][s] = entry

    print("Writing " + output_path)
    f = open(output_path, "w")
    try:
        f.write(json.dumps(out, indent=2))
    finally:
        f.close()
    print("Done.")

main()
