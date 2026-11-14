#!/usr/bin/env python3
"""Expose DXMT's private PE dependency to Wine's normal prefix installer."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile


ARCHITECTURES = {"i386-windows": (0x014C, 0x010B), "x86_64-windows": (0x8664, 0x020B)}


def validate_pe(data, architecture, path):
    machine, magic = ARCHITECTURES[architecture]
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError(f"Not a PE DLL: {path}")
    offset = struct.unpack_from("<I", data, 0x3C)[0]
    if offset < 64 or offset + 26 > len(data) or data[offset:offset + 4] != b"PE\0\0":
        raise ValueError(f"Invalid PE header: {path}")
    actual_machine = struct.unpack_from("<H", data, offset + 4)[0]
    characteristics = struct.unpack_from("<H", data, offset + 22)[0]
    actual_magic = struct.unpack_from("<H", data, offset + 24)[0]
    if (actual_machine, actual_magic) != (machine, magic) or not characteristics & 0x2000:
        raise ValueError(f"Wrong PE DLL architecture for {architecture}: {path}")


def require_unmapped(path):
    result = subprocess.run(["/usr/sbin/lsof", "-t", "--", str(path)],
                            capture_output=True, text=True, check=False)
    if result.returncode == 1 and not result.stdout and not result.stderr:
        return
    if result.returncode == 0 and result.stdout.strip():
        raise ValueError(f"Cannot replace a mapped DLL: {path}, PIDs {result.stdout.split()}")
    raise ValueError(f"Cannot verify DLL mappings: {path}: {result.stderr.strip()}")


def read_sources(dxmt_root):
    sources = []
    for architecture in ARCHITECTURES:
        source = Path(dxmt_root) / architecture / "winemetal.dll"
        data = source.read_bytes()
        validate_pe(data, architecture, source)
        sources.append((architecture, source, data))
    return sources


def stage(root, check=False):
    root = Path(root).resolve(strict=True)
    planned = []
    for architecture, source, data in read_sources(root / "lib/dxmt"):
        target = root / "lib/wine" / architecture / "winemetal.dll"
        if not target.parent.is_dir():
            raise ValueError(f"Missing Wine builtin directory: {target.parent}")
        if root not in target.parent.resolve().parents:
            raise ValueError(f"Wine builtin directory leaves the install root: {target.parent}")
        try:
            info = target.lstat()
        except FileNotFoundError:
            existing = None
        else:
            if not stat.S_ISREG(info.st_mode):
                raise ValueError(f"Expected a regular builtin DLL: {target}")
            existing = target.read_bytes()
        changed = existing != data
        if changed and check:
            raise ValueError(f"DXMT builtin DLL is missing or outdated: {target}")
        if changed and existing is not None:
            require_unmapped(target)
        planned.append((target, data, changed))

    # Validate both architectures and mapping checks before changing either file.
    result = []
    for target, data, changed in planned:
        if changed:
            temporary = None
            try:
                with tempfile.NamedTemporaryFile(dir=target.parent, prefix=".winemetal-", delete=False) as out:
                    temporary = Path(out.name)
                    out.write(data)
                    out.flush()
                    os.fsync(out.fileno())
                    os.fchmod(out.fileno(), 0o644)
                os.replace(temporary, target)
                temporary = None
            finally:
                if temporary is not None:
                    temporary.unlink(missing_ok=True)
            if target.read_bytes() != data:
                raise ValueError(f"DXMT builtin copy verification failed: {target}")
        result.append({"path": str(target), "sha256": hashlib.sha256(data).hexdigest(),
                       "state": "staged" if changed else "verified"})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runtime", help="WineForge install root, or DXMT root with --validate-source")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="Verify the package without changing files")
    mode.add_argument("--validate-source", action="store_true", help="Only validate both source DXMT PE DLLs")
    args = parser.parse_args()
    try:
        if args.validate_source:
            result = [{"path": str(source), "sha256": hashlib.sha256(data).hexdigest(), "state": "valid"}
                      for _, source, data in read_sources(args.runtime)]
        else:
            result = stage(args.runtime, args.check)
    except (OSError, ValueError, struct.error) as error:
        print(f"DXMT builtin staging failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
