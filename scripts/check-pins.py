#!/usr/bin/env python3
"""check-pins.py - the build-dependency pins recorded in three places agree with each other.

pins.toml is the record, server-win64.yml is what builds the Windows artifact, and
scripts/windows-install.ps1 is what verifies the same archive on the user's machine before it
unpacks anything. The two hash checks are only worth something if both sides mean the same file: a
copy that drifts leaves CI building against one archive while the installer runs another, which is
exactly what the pin exists to prevent.

Run from the repository root:

    python3 scripts/check-pins.py

Exits 1 and names each disagreement; prints one line per pin when everything agrees.
"""

from __future__ import annotations

import pathlib
import re
import sys
import tomllib

import yaml

WINDOWS_WORKFLOW = pathlib.Path(".github/workflows/server-win64.yml")
INSTALLER = pathlib.Path("scripts/windows-install.ps1")


def installer_constant(name: str) -> str:
    """The value of a PowerShell `$Name = '...'` assignment at the start of a line."""
    match = re.search(rf"^\${re.escape(name)} = '([^']*)'", INSTALLER.read_text(), re.M)
    return match.group(1) if match else ""


def main() -> int:
    pins = tomllib.loads(pathlib.Path("pins.toml").read_text())["artifact"]["win64"]["build_deps"]
    env = yaml.safe_load(WINDOWS_WORKFLOW.read_text())["env"]

    checks = [
        ("boost version (pins.toml / BOOST_VERSION)", pins["boost"]["version"], env.get("BOOST_VERSION")),
        ("boost sha256 (pins.toml / BOOST_SHA256)", pins["boost"]["sha256"], env.get("BOOST_SHA256")),
        ("mysql version (pins.toml / MYSQL_VERSION)", pins["mysql"]["version"], env.get("MYSQL_VERSION")),
        ("mysql sha256 (pins.toml / MYSQL_SHA256)", pins["mysql"]["sha256"], env.get("MYSQL_SHA256")),
        ("mysql version (pins.toml / installer $MysqlVersion)", pins["mysql"]["version"], installer_constant("MysqlVersion")),
        ("mysql sha256 (pins.toml / installer $MysqlSha256Default)", pins["mysql"]["sha256"], installer_constant("MysqlSha256Default")),
        ("mysql url (pins.toml / installer $MysqlUrlDefault)", pins["mysql"]["url"], installer_constant("MysqlUrlDefault")),
    ]

    bad = [(label, want, got) for label, want, got in checks if want != got]
    for label, want, got in bad:
        print(f"::error::{label}: pins.toml says {want!r}, the copy says {got!r}")
    if bad:
        return 1
    for label, want, _ in checks:
        print(f"agrees: {label} = {want}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
