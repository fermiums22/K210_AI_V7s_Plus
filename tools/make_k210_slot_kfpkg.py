#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import shutil
import tempfile
import zipfile
from pathlib import Path


def parse_u32(s: str) -> int:
    return int(s, 0)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="Input raw K210 app binary")
    ap.add_argument("--address", required=True, type=parse_u32, help="Flash address/offset, e.g. 0x00100000")
    ap.add_argument("--out", required=True, help="Output .kfpkg path")
    args = ap.parse_args()

    bin_path = Path(args.bin).resolve()
    out_path = Path(args.out).resolve()
    if not bin_path.is_file():
        raise SystemExit(f"input bin not found: {bin_path}")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    entry_name = bin_path.name
    flash_list = {
        "version": "0.1.0",
        "files": [
            {
                "address": args.address,
                "bin": entry_name,
                "sha256Prefix": False,
            }
        ],
    }

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        shutil.copyfile(bin_path, tmp / entry_name)
        (tmp / "flash-list.json").write_text(json.dumps(flash_list, indent=2), encoding="utf-8")
        with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            zf.write(tmp / "flash-list.json", "flash-list.json")
            zf.write(tmp / entry_name, entry_name)

    print(f"KFPKG_OK {out_path} address=0x{args.address:08X} bin={bin_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
