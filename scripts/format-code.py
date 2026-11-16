#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Format tracked project C/C++ files, honoring this scope's ignore globs."""
import argparse
import fnmatch
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
parser.add_argument("--check", action="store_true", help="report differences without modifying files")
parser.add_argument("--clang-format", default="clang-format")
args = parser.parse_args()
root = args.root.resolve()
version = subprocess.check_output([args.clang_format, "--version"], text=True).strip()
if "version 23.1.1" not in version:
    sys.exit("Use clang-format 23.1.1 for reproducible formatting; found " + version)
ignore = root / ".clang-format-ignore"
patterns = [line.strip() for line in ignore.read_text().splitlines()
            if line.strip() and not line.lstrip().startswith("#")]
tracked = subprocess.check_output(
    ["git", "-C", str(root), "ls-files", "-z", "--", "*.c", "*.h", "*.cc", "*.cpp", "*.hpp"],
).decode("utf-8").split("\0")
files = [name for name in tracked if name and not any(fnmatch.fnmatchcase(name, p) for p in patterns)]
mode = ["--dry-run", "--Werror"] if args.check else ["-i"]
for start in range(0, len(files), 32):
    subprocess.run([args.clang_format, "--style=file"] + mode + files[start:start + 32],
                   cwd=root, check=True)
print("{}: {} tracked C/C++ files {} with {}".format(
    root, len(files), "checked" if args.check else "formatted", version))
