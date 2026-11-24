#!/usr/bin/env python3
"""check-module-wiring.py - every module in modules/ is named by every build surface.

A module that a build does not copy is not compiled, and a module that thor-install.sh does not
fetch is not on the device, yet both look fine in the source tree. The surfaces that have to name
each module are:

    pins.toml                          path = "modules/<name>"
    .github/workflows/server-aarch64.yml   the copy list into core/modules
    .github/workflows/server-x86_64.yml    the same
    .github/workflows/server-win64.yml     the same
    scripts/thor-install.sh            the fetch/extract list

Only modules present in this repository are checked: mod-playerbots is cloned from its own repo,
so the pinned module is not a directory here. scripts/windows-install.ps1 is deliberately absent
from the list: it consumes the built artifact and checks conf filenames, not module paths.

Run from the repository root:

    python3 scripts/check-module-wiring.py [--repo PATH]

Exits 1 and prints "<surface>: <module>" for every module a surface does not name.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
import tomllib

PIN = "pins.toml"
WORKFLOWS = (
    ".github/workflows/server-aarch64.yml",
    ".github/workflows/server-x86_64.yml",
    ".github/workflows/server-win64.yml",
)
INSTALLER = "scripts/thor-install.sh"


def repo_modules(root: pathlib.Path) -> list[str]:
    """Module directory names under modules/ that carry source or a conf."""
    modules = root / "modules"
    names = []
    for entry in sorted(modules.iterdir()):
        if entry.is_dir() and ((entry / "src").is_dir() or (entry / "conf").is_dir()):
            names.append(entry.name)
    return names


def pins_paths(root: pathlib.Path) -> set[str]:
    """Every `path` value recorded in pins.toml, from any table."""
    paths: set[str] = set()

    def walk(table: object) -> None:
        if isinstance(table, dict):
            for key, value in table.items():
                if key == "path" and isinstance(value, str):
                    paths.add(value)
                else:
                    walk(value)
        elif isinstance(table, list):
            for value in table:
                walk(value)

    walk(tomllib.loads((root / PIN).read_text()))
    return paths


def mentions(text: str, name: str) -> bool:
    """`modules/<name>` as a whole path, not a prefix of a longer module name."""
    return re.search(rf"modules/{re.escape(name)}(?![A-Za-z0-9_.-])", text) is not None


def main() -> int:
    parser = argparse.ArgumentParser(description="Check that every repo module is wired into every build surface.")
    parser.add_argument("--repo", default=".", help="repository root (default: the current directory)")
    args = parser.parse_args()
    root = pathlib.Path(args.repo).resolve()

    surfaces = (PIN, *WORKFLOWS, INSTALLER)
    texts: dict[str, str] = {}
    missing_files = [surface for surface in surfaces if not (root / surface).is_file()]
    for surface in surfaces:
        if surface not in missing_files:
            texts[surface] = (root / surface).read_text()

    problems = [f"::error::{surface}: file not found" for surface in missing_files]

    pinned = pins_paths(root) if PIN not in missing_files else set()
    for name in repo_modules(root):
        wanted = f"modules/{name}"
        if PIN in texts and wanted not in pinned:
            problems.append(f"{PIN}: {name}")
        for workflow in WORKFLOWS:
            if workflow in texts and not mentions(texts[workflow], name):
                problems.append(f"{workflow}: {name}")
        if INSTALLER in texts and not mentions(texts[INSTALLER], name):
            problems.append(f"{INSTALLER}: {name}")

    for problem in problems:
        print(problem)
    if problems:
        return 1

    for name in repo_modules(root):
        print(f"wired: modules/{name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
