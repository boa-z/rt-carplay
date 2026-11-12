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
    "modes": {"resources": [{"resourceID": i, "transferType": 1,
                              "transferPriority": 100, "takeConstraint": 100}
                             for i in (1, 2)]},
    "audioFormats": [
        {"type": 102 if i == 8 else 100, "audioType": "media",
         "audioOutputFormats": 0xC00000 if i == 8 else 0xCFFC}
        for i in range(9)
    ],
}, fmt=plistlib.FMT_BINARY)
offer = plistlib.dumps({
    "features": ["iAPChannel", "viewAreas", "mainBuffered", "bad\nfeature", "x" * 80, 7]
                + [f"feature{i}" for i in range(6, 20)],
    "osVersion": "27.3", "sourceVersion": "950.7.1\nINJECTED",
    "timingProtocol": "NTP", "ekey": b"SECRET_MUST_NOT_LOG",
    "deviceID": "DEVICE_MUST_NOT_LOG",
}, fmt=plistlib.FMT_BINARY)
invalid_offer = plistlib.dumps({"features": "not-an-array"}, fmt=plistlib.FMT_BINARY)
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
#include "airplay_info_plist.h"
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
source += function("core/airplay/airplay_rtsp_session.c", "static void log_setup_offer")
source += function("examples/rtthread_app/carplay_app.c", "static bool announcement_features_match")
source += function("examples/rtthread_app/carplay_app.c", "static uint64_t capability_features")
source += "\nstatic const uint8_t info[] = {" + ",".join(map(str, info)) + "};\n"
source += "static const uint8_t txt[] = {" + ",".join(map(str, txt)) + "};\n"
source += "static const uint8_t offer[] = {" + ",".join(map(str, offer)) + "};\n"
source += "static const uint8_t invalid_offer[] = {" + ",".join(map(str, invalid_offer)) + "};\n"
source += r'''
/* The writer consumes decoder availability through this seam. Exercise
 * video-only, PCM-only and compiled-compressed offers, without a codec change. */
static uint64_t compiled_formats;
uint64_t rtcp_audio_output_formats(void) { return compiled_formats; }

static void write_profiles(const char *directory)
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        compiled_formats = mode == 2 ? UINT64_C(0x70c0cffc) : UINT64_C(0xcffc);
        for (unsigned legacy = 0; legacy < 2; ++legacy) {
            airplay_info_plist_config_t config = {0};
            config.device_id = "98:03:CF:66:65:9B";
            config.bluetooth_id = "66:55:44:33:22:11";
            config.width_pixels = 1024; config.height_pixels = 600;
            config.audio_output = mode != 0;
            config.audio_compressed = mode == 2;
            config.features = capability_features(airplay_info_plist_features(&config), legacy != 0);
            uint8_t bytes[AIRPLAY_INFO_PLIST_MAX_BYTES], encoded[64];
            size_t length = 0;
            assert(airplay_info_plist_build(&config, bytes, sizeof(bytes), &length));
            char words[32], filename[1024];
            assert(airplay_txt_format_features(config.features, words, sizeof(words)));
            airplay_txt_record_t record = {"features", words};
            size_t count = airplay_txt_encode(&record, 1, encoded, sizeof(encoded));
            assert(count && announcement_features_match(bytes, length, encoded, count));
            int n = snprintf(filename, sizeof(filename), "%s/%u-%u.bplist", directory, mode, legacy);
            assert(n > 0 && (size_t)n < sizeof(filename));
            FILE *file = fopen(filename, "wb");
            assert(file && fwrite(bytes, 1, length, file) == length);
            assert(fclose(file) == 0);
        }
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    /* Narrow the old mask without enabling any previously absent capability.
     * Keep PCM-only and video-only builds from acquiring AAC or audio bits. */
    const uint64_t old = UINT64_C(0x615657aee2);
    assert(capability_features(old, false) == UINT64_C(0x6144540280));
    assert(capability_features(old, true) == old);
    assert(capability_features(UINT64_C(0x615647aee2), false) == UINT64_C(0x6144440280));
    assert(capability_features(UINT64_C(0x615203a4e2), false) == UINT64_C(0x6140000080));
    assert(capability_features(0, false) == 0);
    assert((capability_features(UINT64_MAX, false) & ~UINT64_C(0x6144540280)) == 0);
    airplay_rtsp_session_t session = {.config={info, sizeof(info)}};
    log_info_audio(&session);
    assert(strstr(logs, "features=0x000000615657aee2 audio_formats=9"));
    assert(strstr(logs, "info_bluetooth count=1"));
    assert(strstr(logs, "id=66:55:44:33:22:11"));
    assert(strstr(logs, "type=102 role=media output=0x0000000000c00000 input=0x0000000000000000"));
    assert(strstr(logs, "info_resource id=1 transfer=1 priority=100 take_constraint=100"));
    assert(strstr(logs, "info_resource id=2 transfer=1 priority=100 take_constraint=100"));
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
    airplay_bplist_doc_t doc;
    airplay_bplist_object_t root;
    assert(airplay_bplist_open(offer, sizeof(offer), &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    log_setup_offer(&doc, root);
    assert(strstr(logs, "setup_offer feature_count=20 limit=16"));
    assert(strstr(logs, "setup_offer index=2 name=mainBuffered"));
    assert(strstr(logs, "setup_offer index=3 name=bad?feature"));
    assert(strstr(logs, "setup_offer index=4 name=invalid_or_overlong"));
    assert(strstr(logs, "setup_offer index=5 name=invalid_or_overlong"));
    assert(strstr(logs, "setup_offer index=15 name=feature15"));
    assert(!strstr(logs, "setup_offer index=16"));
    assert(strstr(logs, "setup_peer osVersion=27.3"));
    assert(strstr(logs, "setup_peer sourceVersion=950.7.1?INJECTED"));
    assert(strstr(logs, "setup_peer timingProtocol=NTP"));
    assert(!strstr(logs, "SECRET_MUST_NOT_LOG") && !strstr(logs, "DEVICE_MUST_NOT_LOG"));
    assert(airplay_bplist_open(invalid_offer, sizeof(invalid_offer), &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    log_setup_offer(&doc, root);
    assert(strstr(logs, "setup_offer features=missing_or_invalid"));
    write_profiles(argv[1]);
    puts("Actual SDK printf + actual capability trace: 64-bit values, counts, Bluetooth IDs, TXT/plist and bounded/sanitized SETUP offers passed");
}
'''
with tempfile.TemporaryDirectory(prefix="rtcp-capabilities-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(source)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I" + str(root / "include"), "-I" + str(root / "core/airplay/include"),
                    "-I" + str(root / "core/discovery/include"), str(path / "test.c"),
                    str(root / "core/airplay/airplay_bplist.c"),
                    str(root / "core/airplay/airplay_info_plist.c"),
                    str(root / "core/airplay/airplay_hid_descriptor.c"),
                    str(root / "core/discovery/airplay_txt.c"), "-lm", "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test"), str(path)], check=True)
    for mode, expected in enumerate((0x6140000080, 0x6144440280, 0x6144540280)):
        explicit = plistlib.loads((path / f"{mode}-0.bplist").read_bytes())
        legacy = plistlib.loads((path / f"{mode}-1.bplist").read_bytes())
        assert explicit.pop("features") == expected
        assert legacy.pop("features") & expected == expected
        assert explicit == legacy, "profile changed something other than Features"
        formats = explicit.get("audioFormats", [])
        assert len(formats) == (0 if mode == 0 else 8 if mode == 1 else 9)
        assert all("audioInputFormats" not in entry for entry in formats)
    print("Independent plistlib: explicit/legacy profiles differ only in Features; video/PCM/compressed offers and encoded TXT match passed")
