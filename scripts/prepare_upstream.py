#!/usr/bin/env python3
from pathlib import Path
import json
import re

ROOT = Path("upstream")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise SystemExit(f"patch point not found: {label}")
    return text.replace(old, new, 1)


def patch_module():
    main_path = ROOT / "soos" / "main.cpp"
    text = main_path.read_text(encoding="utf-8")

    # Embed the browser UI directly in the background module so the user only
    # has to open http://<3ds-ip>:8080/ in a normal browser.
    html = Path("web/index.html").read_text(encoding="utf-8")
    header = (
        "#pragma once\n"
        "static const char k3DSLinkIndex[] = " + json.dumps(html) + ";\n"
        "static const unsigned int k3DSLinkIndexLen = sizeof(k3DSLinkIndex) - 1;\n"
    )
    (ROOT / "soos" / "3dslink_web.h").write_text(header, encoding="utf-8")

    text = replace_once(
        text,
        '#include "utils.hpp"',
        '#include "utils.hpp"\n#include "3dslink_web.h"',
        "web header include",
    )

    helper = r'''
static bool gBrowserStream = false;

static int sendRawAll(int fd, const void* data, u32 len)
{
    const u8* p = (const u8*)data;
    u32 sent = 0;
    while(sent < len)
    {
        int r = send(fd, p + sent, len - sent, 0);
        if(r <= 0) return -1;
        sent += r;
    }
    return sent;
}

static void sendIndexPage(int fd)
{
    char h[256];
    int n = snprintf(h, sizeof(h),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %u\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        k3DSLinkIndexLen);
    if(n > 0) sendRawAll(fd, h, (u32)n);
    sendRawAll(fd, k3DSLinkIndex, k3DSLinkIndexLen);
}

static void sendStreamHeaders(int fd)
{
    static const char h[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Cache-Control: no-store, no-cache, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n\r\n";
    sendRawAll(fd, h, sizeof(h) - 1);
}
'''

    text = replace_once(
        text,
        "const int bufsoc_pak_data_offset = 8; // After 8 bytes, the real data begins.",
        helper + "\nconst int bufsoc_pak_data_offset = 8; // After 8 bytes, the real data begins.",
        "HTTP helpers",
    )

    old_wri = r'''    int wribuf(int flags = 0)
    {
        u32 mustwri = getPakSize() + bufsoc_pak_data_offset;
        int offs = 0;
        int ret = 0;

        while(mustwri)
        {
            if(mustwri >> 12)
                ret = send(socketid, &(bufferptr[offs]), 0x1000, flags);
            else
                ret = send(socketid, &(bufferptr[offs]), mustwri, flags);
            if(ret < 0) return -errno;
            mustwri -= ret;
            offs += ret;
        }

        return offs;
    }'''

    new_wri = r'''    int wribuf(int flags = 0)
    {
        // Browser mode uses a tiny framing format instead of the original
        // Chokistream protocol. Each packet is:
        // 3DSL | screen | part | parts | reserved | JPEG length (BE) | JPEG
        if(gBrowserStream && getPakType() == 0x01)
        {
            u32 payload = getPakSize();
            u8 screen = (getPakSubtype() & 0x10) ? 1 : 0;
            u8 part = getPakSubtypeB() & 0x07;
            u8 parts = (getPakSubtypeB() & 0x08) ? 8 : 1;
            u8 h[12] = {
                '3','D','S','L', screen, part, parts, 0,
                (u8)(payload >> 24), (u8)(payload >> 16),
                (u8)(payload >> 8), (u8)payload
            };
            if(sendRawAll(socketid, h, sizeof(h)) < 0) return -1;
            if(sendRawAll(socketid, bufferptr + bufsoc_pak_data_offset, payload) < 0) return -1;
            return payload + sizeof(h);
        }

        u32 mustwri = getPakSize() + bufsoc_pak_data_offset;
        int offs = 0;
        int ret = 0;

        while(mustwri)
        {
            if(mustwri >> 12)
                ret = send(socketid, &(bufferptr[offs]), 0x1000, flags);
            else
                ret = send(socketid, &(bufferptr[offs]), mustwri, flags);
            if(ret < 0) return -errno;
            mustwri -= ret;
            offs += ret;
        }

        return offs;
    }'''
    text = replace_once(text, old_wri, new_wri, "browser packet writer")

    text = replace_once(text, "const int port = 6464;", "const int port = 8080;", "HTTP port")

    # The upstream defaults are written before memset() and therefore get
    # erased. Browser mode needs deterministic settings with no PC handshake.
    text = replace_once(
        text,
        "    memset(cfgblk, 0, sizeof(cfgblk));",
        "    memset(cfgblk, 0, sizeof(cfgblk));\n"
        "    cfgblk[0] = 1;  // stream immediately; no Chokistream handshake\n"
        "    cfgblk[1] = 58; // JPEG quality: favor motion on Old 3DS\n"
        "    cfgblk[3] = 3;  // both screens\n"
        "    cfgblk[4] = 0;  // JPEG\n",
        "browser defaults",
    )

    accept_old = r'''                else
                {
                    soc = new bufsoc(cli, bufsoc_siz);
                    k = soc->pack();'''

    accept_new = r'''                else
                {
                    char request[1024];
                    memset(request, 0, sizeof(request));
                    int got = recv(cli, request, sizeof(request) - 1, 0);
                    if(got <= 0)
                    {
                        close(cli);
                        continue;
                    }

                    if(strstr(request, "GET /stream ") == request)
                    {
                        sendStreamHeaders(cli);
                        gBrowserStream = true;
                        cfgblk[0] = 1;
                        cfgblk[1] = 58;
                        cfgblk[3] = 3;
                        cfgblk[4] = 0;
                    }
                    else
                    {
                        sendIndexPage(cli);
                        close(cli);
                        continue;
                    }

                    soc = new bufsoc(cli, bufsoc_siz);
                    k = soc->pack();'''
    text = replace_once(text, accept_old, accept_new, "HTTP accept")

    send_old = '''                soc->wribuf();\n\n#if DEBUG_VERBOSE==1'''
    send_new = '''                if(soc->wribuf() < 0)\n                {\n                    delete soc;\n                    soc = nullptr;\n                    break;\n                }\n\n#if DEBUG_VERBOSE==1'''
    text = replace_once(text, send_old, send_new, "disconnect-aware stream send")

    end_old = '''    threadrunning = 0;\n}\n\n\nstatic FILE* f = nullptr;'''
    end_new = '''    gBrowserStream = false;\n    threadrunning = 0;\n}\n\n\nstatic FILE* f = nullptr;'''
    text = replace_once(text, end_old, end_new, "clear browser stream flag")

    main_path.write_text(text, encoding="utf-8")

    makefile = ROOT / "Makefile"
    m = makefile.read_text(encoding="utf-8")
    m = re.sub(r"^TARGET=.*$", "TARGET=3DSLinkModule", m, count=1, flags=re.M)
    m = re.sub(r"^APP_TITLE\s*:=.*$", "APP_TITLE       :=  3DSLink Module", m, count=1, flags=re.M)
    m = re.sub(r"^APP_DESCRIPTION\s*:=.*$", "APP_DESCRIPTION :=  Browser screen streaming service", m, count=1, flags=re.M)
    m = re.sub(r"^APP_AUTHOR\s*:=.*$", "APP_AUTHOR      :=  AllPawsonDeck / ChirunoMod contributors", m, count=1, flags=re.M)
    m = re.sub(r"^APP_PRODUCT_CODE\s*:=.*$", "APP_PRODUCT_CODE:=  CTR-N-3DSM", m, count=1, flags=re.M)
    m = re.sub(r"^APP_UNIQUE_ID\s*:=.*$", "APP_UNIQUE_ID   :=  0xCF109", m, count=1, flags=re.M)
    m = m.replace("-DARM11 -D_3DS", "-D__3DS__")
    makefile.write_text(m, encoding="utf-8")


def patch_loader():
    main_path = ROOT / "HzLoad" / "soos" / "main.c"
    text = main_path.read_text(encoding="utf-8")
    text = text.replace("#include <stdio.h>", "#include <stdio.h>\n#include <stdlib.h>\n#include <malloc.h>\n#include <unistd.h>\n#include <arpa/inet.h>")

    text = replace_once(
        text,
        '''    u64 titleid;\n    if(r & KEY_Y)\n        titleid = 0x000401300CF00F02ULL; // HzMod\n    else\n        titleid = 0x000401300CF00902ULL; // ChirunoMod''',
        '''    u64 titleid = 0x000401300CF10902ULL; // 3DSLink Module''',
        "loader module title id",
    )

    text = text.replace("0x000401300CF00F02ULL", "0x000401300CF10902ULL")
    text = text.replace("0x000401300CF00F09ULL", "0x000401300CF10902ULL")

    success_marker = '''        if(ret < 0)\n        {\n            gfxInitDefault();\n            consoleInit(GFX_BOTTOM, 0);\n            printf("\\nLaunchTitle (%i) failed: %08X\\nPlease refer to 3DS error codes for details\\n\\nPress SELECT to exit", pid, ret);\n            gfxFlushBuffers();\n            while(aptMainLoop())\n            {\n                hidScanInput();\n                if(hidKeysHeld() & KEY_SELECT) break;\n            }\n            gfxExit();\n        }'''

    success_replacement = success_marker + r'''
        else
        {
            gfxInitDefault();
            consoleInit(GFX_BOTTOM, 0);
            printf("\x1b[2;2H3DSLink is running!\n\n");

            u32* socbuf = (u32*)memalign(0x1000, 0x10000);
            if(socbuf && socInit(socbuf, 0x10000) >= 0)
            {
                struct in_addr a;
                a.s_addr = gethostid();
                printf("Open this on your computer:\n\nhttp://%s:8080/\n", inet_ntoa(a));
                socExit();
            }
            else
            {
                printf("Open your 3DS IP on port 8080.\n");
            }
            if(socbuf) free(socbuf);

            printf("\nPress A to return to HOME Menu.\n");
            gfxFlushBuffers();
            while(aptMainLoop())
            {
                hidScanInput();
                if(hidKeysDown() & KEY_A) break;
                svcSleepThread(2e7);
            }
            gfxExit();
        }'''
    text = replace_once(text, success_marker, success_replacement, "loader success screen")
    main_path.write_text(text, encoding="utf-8")

    makefile = ROOT / "HzLoad" / "Makefile"
    m = makefile.read_text(encoding="utf-8")
    m = re.sub(r"^TARGET\s*:=.*$", "TARGET          := 3DSLink", m, count=1, flags=re.M)
    m = re.sub(r"^APP_TITLE\s*:=.*$", "APP_TITLE       :=  3DSLink", m, count=1, flags=re.M)
    m = re.sub(r"^APP_DESCRIPTION\s*:=.*$", "APP_DESCRIPTION :=  Stream your 3DS to a browser", m, count=1, flags=re.M)
    m = re.sub(r"^APP_AUTHOR\s*:=.*$", "APP_AUTHOR      :=  AllPawsonDeck", m, count=1, flags=re.M)
    m = re.sub(r"^APP_PRODUCT_CODE\s*:=.*$", "APP_PRODUCT_CODE:=  CTR-N-3DSL", m, count=1, flags=re.M)
    m = re.sub(r"^APP_UNIQUE_ID\s*:=.*$", "APP_UNIQUE_ID   :=  0xCFAAF", m, count=1, flags=re.M)
    m = m.replace("CXXFLAGS\t:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11", "CXXFLAGS\t:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11")

    # Keep the user's PNG icon, but omit the old Chiruno/ChLoad extended banner.
    m = m.replace("$(OUTPUT).cia: $(OUTPUT)-stripped.elf $(TOPDIR)/$(BUILD)/banner.bin $(TOPDIR)/$(BUILD)/image.bin", "$(OUTPUT).cia: $(OUTPUT)-stripped.elf $(TOPDIR)/$(BUILD)/image.bin")
    m = m.replace(" -banner $(TOPDIR)/$(BUILD)/banner.bin", "")
    makefile.write_text(m, encoding="utf-8")


if __name__ == "__main__":
    if not ROOT.exists():
        raise SystemExit("upstream/ is missing; clone ChirunoMod first")
    patch_module()
    patch_loader()
    print("3DSLink patches applied")
