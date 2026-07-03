#!/usr/bin/env python3
from __future__ import annotations

import argparse
import ctypes
import shutil
import subprocess
from pathlib import Path

DRIVE_REMOVABLE = 2


def drive_type(root: str) -> int:
    return ctypes.windll.kernel32.GetDriveTypeW(ctypes.c_wchar_p(root))


def volume_info(root: str) -> dict[str, str | None]:
    name = ctypes.create_unicode_buffer(261)
    fs = ctypes.create_unicode_buffer(261)
    serial = ctypes.c_uint32()
    max_comp = ctypes.c_uint32()
    flags = ctypes.c_uint32()
    ok = ctypes.windll.kernel32.GetVolumeInformationW(
        ctypes.c_wchar_p(root), name, len(name), ctypes.byref(serial),
        ctypes.byref(max_comp), ctypes.byref(flags), fs, len(fs)
    )
    if not ok:
        return {"label": None, "fs": None, "serial": None}
    return {"label": name.value, "fs": fs.value, "serial": f"{serial.value:08X}"}


def logical_drives() -> list[str]:
    mask = ctypes.windll.kernel32.GetLogicalDrives()
    out: list[str] = []
    for i in range(26):
        if mask & (1 << i):
            out.append(chr(ord("A") + i) + ":\\")
    return out


def pick_drive(explicit: str | None) -> str:
    if explicit:
        d = explicit.strip().upper().rstrip("\\/")
        if len(d) == 1 and d.isalpha():
            d += ":"
        root = d + "\\"
        if not Path(root).exists():
            raise SystemExit(f"ERROR: drive not found: {root}")
        return root

    removable = [d for d in logical_drives() if drive_type(d) == DRIVE_REMOVABLE]
    if not removable:
        print("No removable drives found. Pass exact drive, e.g. --drive E:")
        raise SystemExit(2)
    if len(removable) > 1:
        print("Multiple removable drives found:")
        for d in removable:
            vi = volume_info(d)
            print(f"  {d} fs={vi['fs']} label={vi['label']}")
        print("Pass exact one, e.g. --drive E:")
        raise SystemExit(2)
    return removable[0]


def list_root(root: str) -> None:
    print("\n[root] first entries:")
    try:
        items = sorted(Path(root).iterdir(), key=lambda p: (not p.is_dir(), p.name.lower()))
    except Exception as exc:
        print(f"ERROR: cannot list root: {exc}")
        return
    if not items:
        print("  <empty>")
        return
    for p in items[:80]:
        kind = "DIR " if p.is_dir() else "FILE"
        try:
            size = "" if p.is_dir() else f" {p.stat().st_size} bytes"
        except Exception:
            size = " ? bytes"
        print(f"  {kind} {p.name}{size}")
    if len(items) > 80:
        print(f"  ... +{len(items) - 80} more")


def check_known_files(root: str) -> None:
    print("\n[known files]")
    names = ["flash.json", "main.py", "boot.py", "k210", "esp", "firmware", "update", "capture"]
    found = []
    try:
        for p in Path(root).rglob("*"):
            if len(found) >= 60:
                break
            low = p.name.lower()
            if any(x in low for x in names):
                found.append(p)
    except Exception as exc:
        print(f"  scan stopped: {exc}")
    if not found:
        print("  no obvious project/update files found")
        return
    for p in found:
        try:
            rel = p.relative_to(root)
        except Exception:
            rel = p
        print(f"  {rel}")


def run_chkdsk(root: str) -> int:
    drive = root[:2]
    print("\n[chkdsk read-only]")
    print(f"Running: chkdsk {drive}")
    try:
        p = subprocess.run(["chkdsk", drive], text=True, encoding="mbcs", errors="replace")
        return p.returncode
    except Exception as exc:
        print(f"ERROR: chkdsk failed to start: {exc}")
        return 99


def main() -> int:
    ap = argparse.ArgumentParser(description="Read-only microSD checker for K210 project")
    ap.add_argument("--drive", help="Drive letter, e.g. E: . If omitted, auto-picks single removable drive")
    ap.add_argument("--no-chkdsk", action="store_true", help="Skip read-only chkdsk")
    args = ap.parse_args()

    root = pick_drive(args.drive)
    dt = drive_type(root)
    vi = volume_info(root)
    total, used, free = shutil.disk_usage(root)

    print("=== K210 microSD PC check ===")
    print(f"drive : {root}")
    print(f"type  : {dt} ({'REMOVABLE' if dt == DRIVE_REMOVABLE else 'not-removable'})")
    print(f"fs    : {vi['fs']}")
    print(f"label : {vi['label']}")
    print(f"serial: {vi['serial']}")
    print(f"size  : {total / (1024**3):.2f} GiB")
    print(f"free  : {free / (1024**3):.2f} GiB")

    fs = str(vi["fs"] or "").upper()
    if fs == "EXFAT":
        print("WARN : exFAT detected. For K210/FatFs use FAT32 for this project.")
    elif fs and fs not in ("FAT", "FAT32"):
        print("WARN : filesystem is not FAT/FAT32; K210 SD driver may not like it")

    list_root(root)
    check_known_files(root)

    rc = 0
    if not args.no_chkdsk:
        rc = run_chkdsk(root)
        print(f"\n[chkdsk rc] {rc}")
        if rc != 0:
            print("WARN: chkdsk reported a non-zero result. Do NOT run /F yet until we see the output.")

    print("\nDONE: read-only check finished; no writes were requested by this script.")
    return 0 if rc in (0, 1) else 1


if __name__ == "__main__":
    raise SystemExit(main())
