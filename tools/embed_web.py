#!/usr/bin/env python3
"""Gzip the viewer page into a C array that the service serves at /."""
import gzip
import sys
from pathlib import Path


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit("usage: embed_web.py web/index.html out.c")
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    data = gzip.compress(src.read_bytes(), compresslevel=9, mtime=0)
    rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 16]) for i in range(0, len(data), 16)]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(
        f"/* Generated from {src.name} by tools/embed_web.py. Do not edit. */\n"
        "const unsigned char web_index_gz[] = {\n    "
        + ",\n    ".join(rows)
        + f"\n}};\nconst unsigned int web_index_gz_len = {len(data)};\n"
    )


if __name__ == "__main__":
    main()
