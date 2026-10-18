#!/usr/bin/env python3
"""Print the GitHub release notes for VERSION: its CHANGELOG.md section
followed by the install instructions. Exits with an error if CHANGELOG.md has
no section for VERSION, so a release can't go out without a changelog.

    python3 tools/release_notes.py > notes.md
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def section(changelog: str, version: str) -> str:
    m = re.search(rf"^## {re.escape(version)}\b.*?$\n(.*?)(?=^## |\Z)", changelog, re.M | re.S)
    return m.group(1).strip() if m else ""


def main() -> None:
    version = (ROOT / "VERSION").read_text().strip()
    changes = section((ROOT / "CHANGELOG.md").read_text(), version)
    if not changes:
        sys.exit(f"CHANGELOG.md has no '## {version}' section; add one before releasing.")
    install = (ROOT / "docs" / "release-notes.md").read_text().strip()
    print(f"## What's new in {version}\n\n{changes}\n\n---\n\n{install}")


if __name__ == "__main__":
    main()
