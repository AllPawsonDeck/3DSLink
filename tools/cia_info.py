#!/usr/bin/env python3
"""Print the title ID, version and key exheader fields of a CIA.

Used by CI as a sanity check that the CIAs carry the IDs the app relies on.
    python3 tools/cia_info.py build/app/3DSLink.cia [--expect-tid 000400000CFAAF00]
"""
import argparse
import struct
import sys


def align(x):
    return (x + 63) // 64 * 64


def parse(path):
    d = open(path, "rb").read()
    hdr_size, _type, _ver, cert, tik, tmd, _meta, _content = struct.unpack_from("<IHHIIIIQ", d, 0)
    tmd_off = align(hdr_size) + align(cert) + align(tik)
    sig_type = struct.unpack_from(">I", d, tmd_off)[0]
    sig_len = {0x10003: 0x200 + 0x3C, 0x10004: 0x100 + 0x3C, 0x10005: 0x3C + 0x40}[sig_type]
    tmd_hdr = tmd_off + 4 + sig_len
    title_id = struct.unpack_from(">Q", d, tmd_hdr + 0x4C)[0]
    title_version = struct.unpack_from(">H", d, tmd_hdr + 0x9C)[0]

    ncch = align(hdr_size) + align(cert) + align(tik) + align(tmd)
    assert d[ncch + 0x100:ncch + 0x104] == b"NCCH", "first content is not an NCCH"
    ex = d[ncch + 0x200:ncch + 0x600]
    flags = struct.unpack_from("<I", ex, 0x200 + 0x8)[0]  # ARM11 local caps: flag word
    priority = ex[0x200 + 0xF]
    services = [ex[0x250 + i * 8:0x258 + i * 8].rstrip(b"\0").decode() for i in range(34)]
    kernel_caps = struct.unpack_from("<28I", ex, 0x370)
    memtype = None
    for cap in kernel_caps:
        if cap >> 23 == 0b111111110:  # kernel flags descriptor
            memtype = {1: "Application", 2: "System", 3: "Base"}.get((cap >> 8) & 0xF)
    return {
        "title_id": f"{title_id:016X}",
        "version": f"{title_version >> 10}.{(title_version >> 4) & 0x3F}.{title_version & 0xF} ({title_version})",
        "name": ex[0:8].rstrip(b"\0").decode(),
        "memory": memtype,
        "priority": priority,
        "services": [s for s in services if s],
        "flags": hex(flags),
        "compressed": bool(ex[0xD] & 1),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cia")
    ap.add_argument("--expect-tid")
    ap.add_argument("--expect-uncompressed", action="store_true",
                    help="fail if .code is compressed (Luma's loader can't load such system modules)")
    args = ap.parse_args()
    info = parse(args.cia)
    for k, v in info.items():
        print(f"{k:10} {v}")
    if args.expect_tid and info["title_id"] != args.expect_tid.upper():
        sys.exit(f"unexpected title ID {info['title_id']}, wanted {args.expect_tid}")
    if args.expect_uncompressed and info["compressed"]:
        sys.exit(".code is compressed; set EnableCompress: false in the RSF")


if __name__ == "__main__":
    main()
