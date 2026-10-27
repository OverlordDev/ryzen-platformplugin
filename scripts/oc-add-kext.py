#!/usr/bin/env python3
"""Add the RyzenPlatform kext to an OpenCore config.plist.

    python3 scripts/oc-add-kext.py /path/to/config.plist RyzenPlatform.kext

Idempotent: if the BundlePath is already in Kernel -> Add, it does nothing.
Writes through a temp file and re-reads it before replacing the original.
"""
import plistlib
import sys
import os

COMMENT = "RyzenPlatform 0.1 shadow (read-only CPPC / Tctl)"


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip())
        return 2

    path, bundle = sys.argv[1], sys.argv[2]
    with open(path, "rb") as f:
        cfg = plistlib.load(f)

    kernel = cfg.setdefault("Kernel", {})
    entries = kernel.setdefault("Add", [])

    for e in entries:
        if e.get("BundlePath") == bundle:
            print(f"{bundle} already in Kernel->Add, nothing to do")
            return 0

    exe = bundle[:-len(".kext")] if bundle.endswith(".kext") else bundle
    entries.append({
        "Arch": "Any",
        "BundlePath": bundle,
        "Comment": COMMENT,
        "Enabled": True,
        "ExecutablePath": f"Contents/MacOS/{exe}",
        "MaxKernel": "",
        "MinKernel": "",
        "PlistPath": "Contents/Info.plist",
    })

    tmp = path + ".tmp-addkext"
    with open(tmp, "wb") as f:
        plistlib.dump(cfg, f, fmt=plistlib.FMT_XML, sort_keys=False)

    # Re-read what we just wrote before touching the real file.
    with open(tmp, "rb") as f:
        check = plistlib.load(f)
    got = check["Kernel"]["Add"][-1]
    assert got["BundlePath"] == bundle, got
    assert len(check["Kernel"]["Add"]) == len(entries)

    os.replace(tmp, path)
    print(f"added {bundle} as Kernel->Add[{len(entries) - 1}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
