"""Headless camera-offset discovery for AbzuGame using PyGhidra.

Imports the binary into a fresh Ghidra project, runs auto-analysis, then
walks defined strings + xrefs to UE class names and dumps everything to
JSON for offline analysis."""

import json
import os
import sys

import pyghidra

_UE_STRINGS_OLD = []
UE_STRINGS = [
    "Engine is initialized.",
    "GEngine initialized",
    "Engine initialized.",
    "FEngineLoop",
    "Initializing FEngineLoop",
    "Object class Engine cannot be loaded",
    "Engine.Engine",
    "engine.ini",
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
    "r.SetRes",
    "r.MaxFPS",
    "r.ScreenPercentage",
]


def collect_strings(program):
    """All defined strings -> dict of value -> list of address strings."""
    by_value = {}
    listing = program.getListing()
    mem = program.getMemory()
    for block in mem.getBlocks():
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
            if sval in UE_STRINGS:
                by_value.setdefault(sval, []).append(str(d.getAddress()))
    return by_value


def xrefs_to(program, addr_str):
    addr = program.getAddressFactory().getAddress(addr_str)
    if addr is None:
        return []
    refs = []
    it = program.getReferenceManager().getReferencesTo(addr)
    while it.hasNext():
        r = it.next()
        from_addr = r.getFromAddress()
        fn = program.getFunctionManager().getFunctionContaining(from_addr)
        refs.append({
            "from": str(from_addr),
            "type": str(r.getReferenceType()),
            "function": {
                "name": fn.getName(),
                "entry": str(fn.getEntryPoint()),
            } if fn is not None else None,
        })
    return refs


def main():
    if len(sys.argv) < 3:
        print("usage: find_offsets.py <binary> <output.json>", file=sys.stderr)
        sys.exit(2)
    binary = sys.argv[1]
    output = sys.argv[2]

    project_dir = os.path.join(os.path.dirname(output), "..", "ghidra-project")
    os.makedirs(os.path.dirname(output), exist_ok=True)

    print(f"Opening {binary} (this auto-analyzes on first import)...")
    with pyghidra.open_program(binary, project_location=project_dir, project_name="abzu") as flat:
        program = flat.getCurrentProgram()
        print(f"Program: {program.getName()}, base {program.getImageBase()}")

        strings = collect_strings(program)
        print(f"Found {sum(len(v) for v in strings.values())} string matches.")

        result = {
            "program": program.getName(),
            "image_base": str(program.getImageBase()),
            "strings": {},
        }
        for name in UE_STRINGS:
            addrs = strings.get(name, [])
            entry = {"addresses": addrs, "xrefs": []}
            for a in addrs:
                for r in xrefs_to(program, a):
                    entry["xrefs"].append({"string_at": a, **r})
            result["strings"][name] = entry
            print(f"  {name}: {len(addrs)} addr(s), {len(entry['xrefs'])} xref(s)")

        with open(output, "w") as f:
            json.dump(result, f, indent=2)
        print(f"Wrote {output}")


if __name__ == "__main__":
    main()
