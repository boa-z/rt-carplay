/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Linux AirPlay receiver simulator.
 *
 * It advertises _airplay._tcp over mDNS and serves the AirPlay RTSP endpoint
 * with a software Ed25519 identity, so a controller on the same LAN can
 * pair and stream its Screen DataStream.  Decoded H.264 is written to an
 * Annex-B dump when --dump-h264 is supplied; there is no video/audio sink.
 *
 * This is a protocol bench, not a product: the pairing code is static and
 * the process must stay in the foreground.
 */
#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "airplay_ed25519.h"
#include "airplay_info_plist.h"
#include "airplay_pairing_store.h"
#include "airplay_rtsp_server.h"
#include "airplay_txt.h"
#include "mdns_responder.h"
#include "rtcp/rtcp.h"
#include "rtcp_linux.h"

#define DEFAULT_NAME "rt-carplay"
#define DEFAULT_MODEL "AppleTV6,2"
#define DEFAULT_DEVICE_ID "02:00:00:00:00:01"
#define DEFAULT_PAIR_CODE "3939"
#define DEFAULT_SEED_FILE "rt-carplay-identity.seed"
#define DEFAULT_PAIRING_STORE "rt-carplay-pairings.bin"
#define DEFAULT_DUMP_FILE "rt-carplay-screen.h264"

typedef struct {
    const struct rtcp_os *os;
    airplay_ed25519_keypair_t keypair;
    uint8_t accessory_identifier[64];
    size_t accessory_identifier_len;
    const char *pairing_code;
    FILE *h264;
    unsigned frames;
    unsigned video_bytes;
} sim_t;

static volatile sig_atomic_t g_stop;

static void on_signal(int signal_number)
{
    (void)signal_number;
    g_stop = 1;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --name NAME          advertised name (default \"%s\")\n"
            "  --model MODEL        model string (default \"%s\")\n"
            "  --device-id MAC      stable device id (default \"%s\")\n"
            "  --pair-code CODE     Pair-Setup PIN (default \"%s\")\n"
            "  --port PORT          RTSP port (default %u)\n"
            "  --ip A.B.C.D         advertised IPv4 (default: first LAN address)\n"
            "  --seed-file PATH     Ed25519 seed (default \"%s\")\n"
            "  --pairing-store PATH controller table (default \"%s\")\n"
            "  --dump-h264 PATH     write the Screen stream (default \"%s\")\n"
            "  --no-dump            do not write H.264\n",
            program, DEFAULT_NAME, DEFAULT_MODEL, DEFAULT_DEVICE_ID,
            DEFAULT_PAIR_CODE, (unsigned)AIRPLAY_RTSP_DEFAULT_PORT,
            DEFAULT_SEED_FILE, DEFAULT_PAIRING_STORE, DEFAULT_DUMP_FILE);
}

/* ------------------------------------------------------------------ */
/* Identity and pairing persistence                                    */
/* ------------------------------------------------------------------ */

static bool read_exact(const char *path, uint8_t *bytes, size_t len)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    size_t got = fread(bytes, 1U, len, file);
    bool extra = fgetc(file) != EOF;
    fclose(file);
    return got == len && !extra;
}

static bool write_private(const char *path, const uint8_t *bytes, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    size_t offset = 0U;
    while (offset < len) {
        ssize_t written = write(fd, bytes + offset, len - offset);
        if (written <= 0) {
            close(fd);
            return false;
        }
        offset += (size_t)written;
    }
    close(fd);
    return true;
}

static bool load_or_create_seed(const char *path,
                                const struct rtcp_os *os,
                                uint8_t seed[AIRPLAY_ED25519_SEED_BYTES])
{
    if (read_exact(path, seed, AIRPLAY_ED25519_SEED_BYTES)) return true;
    if (os->random == NULL || !os->random(seed, AIRPLAY_ED25519_SEED_BYTES)) {
        fprintf(stderr, "no CSPRNG available to create an identity\n");
        return false;
    }
    if (!write_private(path, seed, AIRPLAY_ED25519_SEED_BYTES)) {
        fprintf(stderr, "cannot write identity seed '%s': %s\n", path,
                strerror(errno));
        return false;
    }
    fprintf(stderr, "generated a new Ed25519 identity in '%s'\n", path);
    return true;
}

typedef struct {
    const char *path;
} store_file_t;

static int store_load(uint8_t *blob, size_t capacity, size_t *len, void *context)
{
    store_file_t *file = context;
    *len = 0U;
    FILE *stream = fopen(file->path, "rb");
    if (stream == NULL) return errno == ENOENT ? RTCP_OK : RTCP_EIO;
    size_t got = fread(blob, 1U, capacity, stream);
    bool extra = fgetc(stream) != EOF;
    fclose(stream);
    if (got == capacity && extra) return RTCP_ENOMEM;
    *len = got;
    return RTCP_OK;
}

static int store_save(const uint8_t *blob, size_t len, void *context)
{
    store_file_t *file = context;
    char temporary[512];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", file->path) >=
        (int)sizeof(temporary)) {
        return RTCP_EINVAL;
    }
    if (!write_private(temporary, blob, len)) return RTCP_EIO;
    if (rename(temporary, file->path) != 0) {
        unlink(temporary);
        return RTCP_EIO;
    }
    return RTCP_OK;
}

/* ------------------------------------------------------------------ */
/* Pairing providers                                                   */
/* ------------------------------------------------------------------ */

static bool provider_random(uint8_t *output, size_t output_len, void *context)
{
    sim_t *sim = context;
    if (sim->os->random == NULL) return false;
    return sim->os->random(output, output_len);
}

static bool provider_sign(const uint8_t *message, size_t message_len,
                          uint8_t signature[64], void *context)
{
    sim_t *sim = context;
    return airplay_ed25519_sign(&sim->keypair, message, message_len, signature);
}

static bool provider_verify(const uint8_t public_key[32],
                            const uint8_t *message, size_t message_len,
                            const uint8_t signature[64], void *context)
{
    (void)context;
    return airplay_ed25519_verify(public_key, message, message_len, signature);
}

static void fill_pairing(sim_t *sim, airplay_rtsp_pairing_config_t *pairing)
{
    memset(pairing, 0, sizeof(*pairing));
    pairing->pair_setup.pairing_code = sim->pairing_code;
    pairing->pair_setup.accessory_identifier = sim->accessory_identifier;
    pairing->pair_setup.accessory_identifier_len =
        sim->accessory_identifier_len;
    pairing->pair_setup.accessory_public_key = sim->keypair.public_key;
    pairing->pair_setup.random = provider_random;
    pairing->pair_setup.sign_accessory = provider_sign;
    pairing->pair_setup.verify_controller = provider_verify;
    pairing->pair_setup.save_pairing = airplay_pairing_store_save_pairing;
    pairing->pair_setup.provider_context = sim;

    pairing->pair_verify.accessory_identifier = sim->accessory_identifier;
    pairing->pair_verify.accessory_identifier_len =
        sim->accessory_identifier_len;
    pairing->pair_verify.random = provider_random;
    pairing->pair_verify.sign_accessory = provider_sign;
    pairing->pair_verify.verify_controller = provider_verify;
    pairing->pair_verify.lookup_controller =
        airplay_pairing_store_lookup_controller;
    pairing->pair_verify.provider_context = sim;
}

/* ------------------------------------------------------------------ */
/* Screen sink                                                         */
/* ------------------------------------------------------------------ */

static int on_screen_message(airplay_screen_message_type_t type,
                             uint8_t *annex_b, size_t annex_b_len,
                             size_t prefix_capacity, uint64_t sender_time_ns,
                             uint64_t arrival_time_us, void *context)
{
    (void)prefix_capacity;
    (void)sender_time_ns;
    (void)arrival_time_us;
    sim_t *sim = context;
    if (type == AIRPLAY_SCREEN_MESSAGE_CONFIG_H264) {
        fprintf(stderr, "screen: H.264 configuration, %u bytes\n",
                (unsigned)annex_b_len);
    } else {
        ++sim->frames;
        sim->video_bytes += (unsigned)annex_b_len;
        if (sim->frames % 60U == 0U) {
            fprintf(stderr, "screen: %u frames, %u bytes\n", sim->frames,
                    sim->video_bytes);
        }
    }
    if (sim->h264 != NULL) {
        if (fwrite(annex_b, 1U, annex_b_len, sim->h264) != annex_b_len) {
            fprintf(stderr, "screen: dump write failed\n");
            fclose(sim->h264);
            sim->h264 = NULL;
        }
    }
    return RTCP_OK;
}

static void on_event_state(bool connected, void *context)
{
    (void)context;
    fprintf(stderr, "event channel: %s\n", connected ? "connected" : "closed");
}

/* ------------------------------------------------------------------ */
/* Network helpers                                                     */
/* ------------------------------------------------------------------ */

static bool lan_ipv4(uint8_t output[4])
{
    struct ifaddrs *addresses = NULL;
    if (getifaddrs(&addresses) != 0) return false;
    bool found = false;
    for (struct ifaddrs *entry = addresses; entry != NULL;
         entry = entry->ifa_next) {
        if (entry->ifa_addr == NULL ||
            entry->ifa_addr->sa_family != AF_INET ||
            (entry->ifa_flags & IFF_LOOPBACK) != 0 ||
            (entry->ifa_flags & IFF_UP) == 0) {
            continue;
        }
        const struct sockaddr_in *in =
            (const struct sockaddr_in *)entry->ifa_addr;
        memcpy(output, &in->sin_addr, 4U);
        found = true;
        break;
    }
    freeifaddrs(addresses);
    return found;
}

static void hex_encode(const uint8_t *bytes, size_t len, char *output)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0U; i < len; ++i) {
        output[i * 2U] = digits[bytes[i] >> 4U];
        output[i * 2U + 1U] = digits[bytes[i] & 0x0fU];
    }
    output[len * 2U] = '\0';
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *name = DEFAULT_NAME;
    const char *model = DEFAULT_MODEL;
    const char *device_id = DEFAULT_DEVICE_ID;
    const char *pair_code = DEFAULT_PAIR_CODE;
    const char *seed_path = DEFAULT_SEED_FILE;
    const char *store_path = DEFAULT_PAIRING_STORE;
    const char *dump_path = DEFAULT_DUMP_FILE;
    const char *ip_option = NULL;
    unsigned port = AIRPLAY_RTSP_DEFAULT_PORT;
    bool dump = true;

    for (int i = 1; i < argc; ++i) {
        const char *argument = argv[i];
        if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argument, "--no-dump") == 0) {
            dump = false;
        } else if (strcmp(argument, "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (strcmp(argument, "--model") == 0 && i + 1 < argc) {
            model = argv[++i];
        } else if (strcmp(argument, "--device-id") == 0 && i + 1 < argc) {
            device_id = argv[++i];
        } else if (strcmp(argument, "--pair-code") == 0 && i + 1 < argc) {
            pair_code = argv[++i];
        } else if (strcmp(argument, "--port") == 0 && i + 1 < argc) {
            port = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argument, "--ip") == 0 && i + 1 < argc) {
            ip_option = argv[++i];
        } else if (strcmp(argument, "--seed-file") == 0 && i + 1 < argc) {
            seed_path = argv[++i];
        } else if (strcmp(argument, "--pairing-store") == 0 && i + 1 < argc) {
            store_path = argv[++i];
        } else if (strcmp(argument, "--dump-h264") == 0 && i + 1 < argc) {
            dump_path = argv[++i];
        } else {
            fprintf(stderr, "unknown option: %s\n", argument);
            usage(argv[0]);
            return 2;
        }
    }
    if (port == 0U || port > 65535U) {
        fprintf(stderr, "invalid port\n");
        return 2;
    }

    uint8_t ipv4[4];
    if (ip_option != NULL) {
        struct in_addr parsed;
        if (inet_pton(AF_INET, ip_option, &parsed) != 1) {
            fprintf(stderr, "invalid --ip: %s\n", ip_option);
            return 2;
        }
        memcpy(ipv4, &parsed, 4U);
    } else if (!lan_ipv4(ipv4)) {
        fprintf(stderr, "warning: no LAN IPv4 address found; using 127.0.0.1\n");
        memset(ipv4, 0, sizeof(ipv4));
        ipv4[0] = 127U;
        ipv4[3] = 1U;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    sim_t sim = {0};
    sim.os = &rtcp_os_linux;
    sim.pairing_code = pair_code;
    if (sim.os->random == NULL) {
        fprintf(stderr, "the host port has no CSPRNG\n");
        return 1;
    }

    uint8_t seed[AIRPLAY_ED25519_SEED_BYTES];
    if (!load_or_create_seed(seed_path, sim.os, seed)) return 1;
    if (!airplay_ed25519_keypair_from_seed(seed, &sim.keypair)) return 1;
    memset(seed, 0, sizeof(seed));

    sim.accessory_identifier_len = strlen(device_id);
    if (sim.accessory_identifier_len > sizeof(sim.accessory_identifier)) {
        fprintf(stderr, "device id is too long\n");
        return 1;
    }
    memcpy(sim.accessory_identifier, device_id, sim.accessory_identifier_len);
    airplay_pairing_store_backend_t backend = {
        .load = store_load,
        .save = store_save,
        .context = &(store_file_t){.path = store_path},
    };
    if (airplay_pairing_store_init(&backend) != RTCP_OK) {
        fprintf(stderr, "cannot load pairing store '%s'\n", store_path);
        return 1;
    }

    uint8_t info_plist[AIRPLAY_INFO_PLIST_MAX_BYTES];
    airplay_info_plist_config_t info = {0};
    info.device_id = device_id;
    info.name = name;
    info.model = model;
    info.width_pixels = 1920U;
    info.height_pixels = 1080U;
    info.max_fps = 30U;
    size_t info_len = 0U;
    if (!airplay_info_plist_build(&info, info_plist, sizeof(info_plist),
                                  &info_len) ||
        info_len > AIRPLAY_RTSP_SESSION_INFO_MAX_BYTES) {
        fprintf(stderr, "cannot build /info plist\n");
        return 1;
    }

    if (dump) {
        sim.h264 = fopen(dump_path, "wb");
        if (sim.h264 == NULL) {
            fprintf(stderr, "cannot open '%s': %s\n", dump_path,
                    strerror(errno));
            return 1;
        }
    }

    airplay_rtsp_server_config_t server_config = {0};
    server_config.os = sim.os;
    server_config.net = &rtcp_net_linux;
    fill_pairing(&sim, &server_config.pairing);
    server_config.info_plist = info_plist;
    server_config.info_plist_len = info_len;
    server_config.port = (uint16_t)port;
    server_config.on_screen_message = on_screen_message;
    server_config.screen_message_context = &sim;
    server_config.on_event_state = on_event_state;

    airplay_rtsp_server_t *server = NULL;
    int result = airplay_rtsp_server_start(&server_config, &server);
    if (result != RTCP_OK) {
        fprintf(stderr, "RTSP server failed to start: %d\n", result);
        if (sim.h264 != NULL) fclose(sim.h264);
        return 1;
    }

    char public_key_hex[AIRPLAY_ED25519_PUBLIC_KEY_BYTES * 2U + 1U];
    hex_encode(sim.keypair.public_key, AIRPLAY_ED25519_PUBLIC_KEY_BYTES,
               public_key_hex);
    airplay_txt_record_t records[AIRPLAY_TXT_MAX_RECORDS];
    airplay_txt_config_t txt_config = {
        .device_id = device_id,
        .model = model,
        .source_version = "950.7.1",
        .protocol_version = "1.1",
        .pairing_id = device_id,
        .public_key = public_key_hex,
    };
    size_t record_count = airplay_txt_build(&txt_config, records,
                                            AIRPLAY_TXT_MAX_RECORDS);
    uint8_t txt[512];
    size_t txt_len = airplay_txt_encode(records, record_count, txt,
                                        sizeof(txt));
    mdns_responder_config_t mdns_config = {
        .host_name = "rt-carplay",
        .instance_name = name,
        .port = airplay_rtsp_server_get_port(server),
        .txt = txt,
        .txt_len = txt_len,
    };
    memcpy(mdns_config.ipv4, ipv4, sizeof(ipv4));
    mdns_responder_t *mdns = mdns_responder_create(&mdns_config);
    int mdns_result = mdns == NULL ? RTCP_EINVAL
                                   : mdns_responder_open(mdns, &rtcp_net_linux);
    if (mdns_result != RTCP_OK) {
        fprintf(stderr,
                "mDNS responder failed to start (rc=%d, %u TXT records, %u "
                "bytes)\n",
                mdns_result, (unsigned)record_count, (unsigned)txt_len);
        mdns_responder_destroy(mdns);
        airplay_rtsp_server_stop(server);
        if (sim.h264 != NULL) fclose(sim.h264);
        return 1;
    }

    fprintf(stderr,
            "rt-carplay simulator\n"
            "  name        : %s\n"
            "  address     : %u.%u.%u.%u:%u\n"
            "  model       : %s\n"
            "  device id   : %s\n"
            "  pairing PIN : %s\n"
            "  H.264 dump  : %s\n"
            "Open Screen Mirroring on the controller and pick \"%s\".  "
            "Enter the PIN if asked.  Ctrl-C stops.\n",
            name, ipv4[0], ipv4[1], ipv4[2], ipv4[3],
            (unsigned)airplay_rtsp_server_get_port(server), model, device_id,
            pair_code, dump ? dump_path : "(disabled)", name);

    while (!g_stop) {
        /* mDNS waits briefly; the RTSP poll is non-blocking so Screen frames
         * are drained at roughly this cadence. */
        (void)mdns_responder_poll(mdns, &rtcp_net_linux, 10);
        (void)airplay_rtsp_server_poll(server, 0);
    }

    fprintf(stderr, "\nstopping: %u screen frames received\n", sim.frames);
    mdns_responder_close(mdns, &rtcp_net_linux);
    mdns_responder_destroy(mdns);
    airplay_rtsp_server_stop(server);
    if (sim.h264 != NULL) fclose(sim.h264);
    airplay_pairing_store_deinit();
    airplay_ed25519_keypair_clear(&sim.keypair);
    return 0;
}
