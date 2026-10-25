#!/usr/bin/env python3
"""Reject descendant output symlinks before build tools can follow them."""
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
for value in sys.argv[2:]:
    path = pathlib.Path(value)
    if not path.is_relative_to(root):
        raise SystemExit(f"Output is outside build root: {path}")
    for candidate in [path, *path.parents]:
        if candidate == root:
            break
        if candidate.is_symlink():
            raise SystemExit(f"Refusing symlink in native output path: {candidate}")
    if path.exists():
        for candidate in path.rglob("*"):
            if candidate.is_symlink():
                raise SystemExit(f"Refusing symlink in native output path: {candidate}")
