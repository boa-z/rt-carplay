#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run actual capability diagnostics through the SDK's small printf implementation."""
from pathlib import Path
import os
import plistlib
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
sdk = Path(sys.argv[1]) if len(sys.argv) > 1 else root.parent / "luban-lite-jc-carplay-oss"
kservice = (sdk / "kernel/rt-thread/src/kservice.c").read_text()
start = kservice.index("#ifdef RT_KPRINTF_USING_LONGLONG\nrt_inline int divide")
end = kservice.index("RTM_EXPORT(rt_snprintf);") + len("RTM_EXPORT(rt_snprintf);")
formatter = kservice[start:end]


def function(path, name):
    source = (root / path).read_text()
    start = source.index("static ", source.index(name) - 32)
    return source[start:source.index("\n}\n", start) + 3]


info = plistlib.dumps({
    "features": 0x615657AEE2, "bluetoothIDs": ["66:55:44:33:22:11"],
    "audioFormats": [
        {"type": 102 if i == 8 else 100, "audioType": "media",
         "audioOutputFormats": 0xC00000 if i == 8 else 0xCFFC}
        for i in range(9)
    ],
}, fmt=plistlib.FMT_BINARY)
txt = b""
for entry in (b"model=ArtInChip", b"features=0x5657aee2,0x61"):
    txt += bytes([len(entry)]) + entry

source = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "airplay_bplist.h"
#include "airplay_txt.h"
typedef size_t rt_size_t;
typedef uint8_t rt_uint8_t;
typedef uint16_t rt_uint16_t;
typedef uint32_t rt_uint32_t;
typedef int16_t rt_int16_t;
typedef int32_t rt_int32_t;
typedef uintptr_t rt_ubase_t;
#define rt_inline static inline
#define RT_WEAK
#define RTM_EXPORT(x)
#define _ISDIGIT(c) ((unsigned)((c) - '0') < 10)
#define RT_PRINTF_PRECISION /* enabled in kservice.c itself */
/* Deliberately leave RT_KPRINTF_USING_LONGLONG undefined, as on D213. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
'''
source += formatter
source += r'''
#pragma GCC diagnostic pop
static char logs[8192];
static void capture(const char *tag, const char *fmt, ...)
{
    (void)tag;
    size_t used = strlen(logs);
    va_list args;
    va_start(args, fmt);
    int n = rt_vsnprintf(logs + used, sizeof(logs) - used, fmt, args);
    va_end(args);
    assert(n >= 0 && (size_t)n + used + 2 < sizeof(logs));
    strcat(logs, "\n");
}
#define RTCP_LOGI capture
#define TAG "test"
typedef struct { struct { const uint8_t *info_plist; size_t info_plist_len; } config; } airplay_rtsp_session_t;
'''
source += function("core/airplay/airplay_rtsp_session.c", "static void log_info_audio")
source += function("examples/rtthread_app/carplay_app.c", "static bool announcement_features_match")
source += "\nstatic const uint8_t info[] = {" + ",".join(map(str, info)) + "};\n"
source += "static const uint8_t txt[] = {" + ",".join(map(str, txt)) + "};\n"
source += r'''
int main(void)
{
    airplay_rtsp_session_t session = {.config={info, sizeof(info)}};
    log_info_audio(&session);
    assert(strstr(logs, "features=0x000000615657aee2 audio_formats=9"));
    assert(strstr(logs, "info_bluetooth count=1"));
    assert(strstr(logs, "id=66:55:44:33:22:11"));
    assert(strstr(logs, "type=102 role=media output=0x0000000000c00000 input=0x0000000000000000"));
    assert(!strchr(logs, '%'));
    assert(announcement_features_match(info, sizeof(info), txt, sizeof(txt)));
    uint8_t corrupt[sizeof(txt)]; memcpy(corrupt, txt, sizeof(txt));
    corrupt[sizeof(corrupt)-1] = '0';
    assert(!announcement_features_match(info, sizeof(info), corrupt, sizeof(corrupt)));
    assert(!announcement_features_match(info, sizeof(info), txt, sizeof(txt)-1));
    assert(!announcement_features_match(info, sizeof(info), txt, 0));
    uint8_t duplicate[2*sizeof(txt)];
    memcpy(duplicate, txt, sizeof(txt)); memcpy(duplicate+sizeof(txt), txt, sizeof(txt));
    assert(!announcement_features_match(info, sizeof(info), duplicate, sizeof(duplicate)));
    puts("Actual SDK printf + actual capability trace: 64-bit values, following counts, Bluetooth IDs and encoded TXT/plist match passed");
}
'''
with tempfile.TemporaryDirectory(prefix="rtcp-capabilities-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(source)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I" + str(root / "core/airplay/include"),
                    "-I" + str(root / "core/discovery/include"), str(path / "test.c"),
                    str(root / "core/airplay/airplay_bplist.c"),
                    str(root / "core/discovery/airplay_txt.c"), "-lm", "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
