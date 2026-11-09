/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Luban-Lite audio sink: decrypted AirPlay PCM -> "sound0".
 *
 * The AirPlay poll thread calls carplay_audio_sink_pcm(); this file copies
 * the packet into a byte ring and wakes a writer thread.  The writer thread
 * owns the RT-Thread audio device: it configures the codec from the format
 * carried with the queued audio, starts the replay stream once, waits for the
 * power amplifier to settle and then feeds rt_device_write() in small chunks.
 * rt_device_write() blocks when the 4 KB replay pool is full, which paces
 * playback without ever stalling the network poll.
 */
#include "audio_sink_lubanlite.h"

#include <rtthread.h>
#include <rtdevice.h>
#include <drivers/audio.h>

#include <string.h>

#include "rtcp/rtcp.h"

#define TAG "carplay-aud"
#define SINK_DEVICE_NAME "sound0"

#define SINK_RING_BYTES (64U * 1024U)
#define SINK_WRITE_BYTES 2048U
#define SINK_THREAD_STACK_BYTES (8U * 1024U)
#define SINK_THREAD_PRIORITY 18U
#define SINK_THREAD_TICK 20U
#define SINK_WAKE_TIMEOUT_MS 50U
#define SINK_AMP_SETTLE_MS 200U /* aplay's power-amplifier settle value */
#define SINK_LOG_EVERY_PACKETS 500U
/* A sender streams continuously (silence is zero PCM), so a gap this long
 * means the stream stopped or the phone reconnected: drop the stale tail
 * instead of playing it into the next stream. */
#define SINK_STREAM_GAP_MS 500U
/* The AirPlay volume is in dB over [-144, 0].  Below -45 dB everything is
 * inaudible, so [-45, 0] maps linearly onto the codec's 0..100 mixer range. */
#define SINK_VOLUME_FLOOR_DB (-45.0f)
#define SINK_VOLUME_MAX_DB 0.0f

typedef struct {
    rt_device_t dev;
    rt_thread_t thread;
    rt_sem_t wake;
    rt_mutex_t lock;

    uint8_t *ring;
    size_t capacity;
    size_t head; /* next write */
    size_t tail; /* next read */
    size_t used;

    /* Ring format, written under lock and carried with each popped chunk. */
    uint32_t rate;
    uint8_t channels;
    bool format_valid;
    rt_tick_t last_tick;

    /* Device format, owned by the writer thread. */
    uint32_t device_rate;
    uint8_t device_channels;
    bool device_started;
    bool device_opened;

    /* Sender volume: queued under lock, applied by the writer thread. */
    bool volume_known;  /* a SET_PARAMETER volume has been received */
    bool volume_pending;
    int volume_level;   /* requested codec percent, 0..100 */
    int applied_volume; /* percent applied to the current stream, -1 = none */

    volatile bool stop;
    volatile bool running;
    volatile bool thread_running;

    unsigned packets;
    unsigned dropped_bytes;
    unsigned write_errors;
    unsigned replays;
    unsigned config_errors;
} audio_sink_t;

static audio_sink_t s_sink;

/* ------------------------------------------------------------------ */
/* Ring buffer                                                         */
/* ------------------------------------------------------------------ */

static size_t ring_push(audio_sink_t *sink, const uint8_t *data, size_t len)
{
    size_t free_bytes = sink->capacity - sink->used;
    if (len > free_bytes) return 0U;

    size_t first = sink->capacity - sink->head;
    if (first > len) first = len;
    memcpy(sink->ring + sink->head, data, first);
    if (len > first) memcpy(sink->ring, data + first, len - first);
    sink->head = (sink->head + len) % sink->capacity;
    sink->used += len;
    return len;
}

static size_t ring_pop(audio_sink_t *sink, uint8_t *out, size_t max,
                       uint32_t *rate, uint8_t *channels)
{
    size_t available = sink->used < max ? sink->used : max;
    if (available == 0U) return 0U;

    size_t first = sink->capacity - sink->tail;
    if (first > available) first = available;
    memcpy(out, sink->ring + sink->tail, first);
    if (available > first) memcpy(out + first, sink->ring, available - first);
    sink->tail = (sink->tail + available) % sink->capacity;
    sink->used -= available;
    *rate = sink->rate;
    *channels = sink->channels;
    return available;
}

static void ring_flush(audio_sink_t *sink)
{
    sink->head = 0U;
    sink->tail = 0U;
    sink->used = 0U;
}

/* ------------------------------------------------------------------ */
/* Audio device (writer thread only)                                   */
/* ------------------------------------------------------------------ */

static bool write_caps(audio_sink_t *sink, uint32_t rate, uint8_t channels)
{
    struct rt_audio_caps caps;
    memset(&caps, 0, sizeof(caps));
    caps.main_type = AUDIO_TYPE_OUTPUT;
    caps.sub_type = AUDIO_DSP_PARAM;
    caps.udata.config.samplerate = rate;
    caps.udata.config.channels = channels;
    caps.udata.config.samplebits = 16;
    rt_err_t result = rt_device_control(sink->dev, AUDIO_CTL_CONFIGURE, &caps);
    if (result != RT_EOK) {
        ++sink->config_errors;
        if (sink->config_errors == 1U || sink->config_errors % 100U == 0U) {
            rt_kprintf("[" TAG "] configure %u Hz %u ch failed: %d\n",
                       (unsigned)rate, (unsigned)channels, (int)result);
        }
        return false;
    }
    return true;
}

/* Maps the sender's dB scale onto the codec's 0..100 mixer range.  AirPlay
 * sends [-144, 0]; below -45 dB the output is inaudible, so that part of the
 * range is folded onto 0.  The comparison also catches NaN. */
static int volume_db_to_percent(float volume_db)
{
    if (!(volume_db > SINK_VOLUME_FLOOR_DB)) return 0;
    if (volume_db >= SINK_VOLUME_MAX_DB) return 100;
    const float span = SINK_VOLUME_MAX_DB - SINK_VOLUME_FLOOR_DB;
    return (int)((volume_db - SINK_VOLUME_FLOOR_DB) * (100.0f / span) + 0.5f);
}

static bool apply_volume(audio_sink_t *sink, int level)
{
    if (!sink->device_started || level == sink->applied_volume) return true;

    struct rt_audio_caps caps;
    memset(&caps, 0, sizeof(caps));
    caps.main_type = AUDIO_TYPE_MIXER;
    caps.sub_type = AUDIO_MIXER_VOLUME;
    caps.udata.value = level;
    rt_err_t result = rt_device_control(sink->dev, AUDIO_CTL_CONFIGURE, &caps);
    if (result != RT_EOK) {
        ++sink->config_errors;
        if (sink->config_errors == 1U || sink->config_errors % 100U == 0U) {
            rt_kprintf("[" TAG "] volume %d%% failed: %d\n", level,
                       (int)result);
        }
        return false;
    }
    sink->applied_volume = level;
    return true;
}

/* Applies a volume queued by the poll thread.  Called from the writer thread on
 * every wake, so a request that arrives before playback starts is applied by
 * start_device() and a failed mixer write is retried on the next wake. */
static void service_volume(audio_sink_t *sink)
{
    if (!sink->device_started || sink->lock == NULL) return;

    bool known;
    bool pending;
    int level;
    if (rt_mutex_take(sink->lock, RT_WAITING_FOREVER) != RT_EOK) return;
    known = sink->volume_known;
    pending = sink->volume_pending;
    level = sink->volume_level;
    (void)rt_mutex_release(sink->lock);

    if (!known || (!pending && sink->applied_volume == level)) return;
    if (!apply_volume(sink, level)) return;

    if (rt_mutex_take(sink->lock, RT_WAITING_FOREVER) == RT_EOK) {
        if (sink->volume_level == level) sink->volume_pending = false;
        (void)rt_mutex_release(sink->lock);
    }
}

static bool start_device(audio_sink_t *sink, uint32_t rate, uint8_t channels)
{
    if (!write_caps(sink, rate, channels)) return false;
    int stream = AUDIO_STREAM_REPLAY;
    rt_err_t result = rt_device_control(sink->dev, AUDIO_CTL_START, &stream);
    if (result != RT_EOK) {
        ++sink->config_errors;
        if (sink->config_errors == 1U || sink->config_errors % 100U == 0U) {
            rt_kprintf("[" TAG "] replay start failed: %d\n", (int)result);
        }
        return false;
    }
    sink->device_started = true;
    sink->device_rate = rate;
    sink->device_channels = channels;
    ++sink->replays;
    /* The mixer state may not survive a STOP/START cycle, so apply the
     * sender's volume to the new stream even when nothing new arrived. */
    sink->applied_volume = -1;
    service_volume(sink);
    rt_kprintf("[" TAG "] playback %u Hz %u ch 16-bit\n", (unsigned)rate,
               (unsigned)channels);
    /* The codec enables the power amplifier on start; aplay waits 200 ms
     * before the first sample so the first words are not clipped. */
    rt_thread_mdelay(SINK_AMP_SETTLE_MS);
    return true;
}

static void stop_device(audio_sink_t *sink)
{
    if (sink->dev != NULL && sink->device_started) {
        int stream = AUDIO_STREAM_REPLAY;
        (void)rt_device_control(sink->dev, AUDIO_CTL_STOP, &stream);
        sink->device_started = false;
    }
}

/* ------------------------------------------------------------------ */
/* Writer thread                                                       */
/* ------------------------------------------------------------------ */

static void sink_thread(void *argument)
{
    audio_sink_t *sink = argument;
    sink->thread_running = true;
    rt_kprintf("[" TAG "] writer thread running\n");

    while (!sink->stop) {
        /* The poll thread wakes us for PCM and for a volume change alike; a
         * timeout still services a volume queued before playback started. */
        if (rt_sem_take(sink->wake, SINK_WAKE_TIMEOUT_MS) != RT_EOK) {
            service_volume(sink);
            continue;
        }
        service_volume(sink);
        while (!sink->stop) {
            uint8_t chunk[SINK_WRITE_BYTES];
            uint32_t rate = 0U;
            uint8_t channels = 0U;
            size_t got;

            if (rt_mutex_take(sink->lock, RT_WAITING_FOREVER) != RT_EOK) break;
            got = ring_pop(sink, chunk, sizeof(chunk), &rate, &channels);
            (void)rt_mutex_release(sink->lock);
            if (got == 0U) break;

            if (!sink->device_started) {
                if (!start_device(sink, rate, channels)) {
                    /* The chunk is lost with the failed start; count it so
                     * carplay_status exposes a stuck codec. */
                    ++sink->write_errors;
                    continue;
                }
            } else if (rate != sink->device_rate ||
                       channels != sink->device_channels) {
                /* A new SETUP negotiated another layout; the codec can be
                 * reconfigured in place, no amplifier toggle needed. */
                if (write_caps(sink, rate, channels)) {
                    sink->device_rate = rate;
                    sink->device_channels = channels;
                }
            }
            rt_size_t written = rt_device_write(sink->dev, 0, chunk, got);
            if (written != got) {
                if (sink->write_errors == 0U || sink->write_errors % 100U == 0U) {
                    rt_kprintf("[" TAG "] write %u/%u failed\n",
                               (unsigned)written, (unsigned)got);
                }
                ++sink->write_errors;
            }
        }
    }

    stop_device(sink);
    rt_kprintf("[" TAG "] stopped: %u packets, %u replay(s), %u dropped bytes, "
               "%u write errors\n",
               sink->packets, sink->replays, sink->dropped_bytes,
               sink->write_errors);
    sink->thread_running = false;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int carplay_audio_sink_start(void)
{
    audio_sink_t *sink = &s_sink;
    if (sink->running || sink->thread_running) return RTCP_ESTATE;

    memset(sink, 0, sizeof(*sink));
    sink->applied_volume = -1;

    sink->dev = rt_device_find(SINK_DEVICE_NAME);
    if (sink->dev == NULL) {
        rt_kprintf("[" TAG "] %s not found (CONFIG_AIC_USING_AUDIO?)\n",
                   SINK_DEVICE_NAME);
        return RTCP_ENOENT;
    }
    if (rt_device_open(sink->dev, RT_DEVICE_OFLAG_WRONLY) != RT_EOK) {
        rt_kprintf("[" TAG "] cannot open %s\n", SINK_DEVICE_NAME);
        sink->dev = NULL;
        return RTCP_EIO;
    }
    sink->device_opened = true;

    sink->ring = rt_malloc(SINK_RING_BYTES);
    sink->wake = rt_sem_create("caud", 0, RT_IPC_FLAG_FIFO);
    sink->lock = rt_mutex_create("caudq", RT_IPC_FLAG_FIFO);
    if (sink->ring == NULL || sink->wake == NULL || sink->lock == NULL) {
        if (sink->ring != NULL) rt_free(sink->ring);
        if (sink->wake != NULL) (void)rt_sem_delete(sink->wake);
        if (sink->lock != NULL) (void)rt_mutex_delete(sink->lock);
        (void)rt_device_close(sink->dev);
        memset(sink, 0, sizeof(*sink));
        return RTCP_ENOMEM;
    }
    sink->capacity = SINK_RING_BYTES;

    sink->stop = false;
    sink->running = true;
    sink->thread = rt_thread_create("carplay-aud", sink_thread, sink,
                                    SINK_THREAD_STACK_BYTES,
                                    SINK_THREAD_PRIORITY, SINK_THREAD_TICK);
    if (sink->thread == NULL) {
        sink->running = false;
        rt_free(sink->ring);
        (void)rt_sem_delete(sink->wake);
        (void)rt_mutex_delete(sink->lock);
        (void)rt_device_close(sink->dev);
        memset(sink, 0, sizeof(*sink));
        return RTCP_ENOMEM;
    }
    (void)rt_thread_startup(sink->thread);
    return RTCP_OK;
}

void carplay_audio_sink_stop(void)
{
    audio_sink_t *sink = &s_sink;
    if (!sink->running) return;

    sink->running = false;
    sink->stop = true;
    if (sink->wake != NULL) (void)rt_sem_release(sink->wake);
    /* rt_device_write() blocks in the replay data queue until the audio ISR
     * consumes a block.  A stalled codec would keep the writer there, so wait
     * with a bound and leak the sink rather than free memory still in use. */
    unsigned guard = 500U;
    while (sink->thread_running && guard-- != 0U) {
        rt_thread_mdelay(10);
    }
    if (sink->thread_running) {
        rt_kprintf("[" TAG "] writer did not stop; resources left allocated\n");
        return;
    }

    if (sink->dev != NULL && sink->device_opened) {
        (void)rt_device_close(sink->dev);
        sink->device_opened = false;
    }
    sink->dev = NULL;
    if (sink->wake != NULL) {
        (void)rt_sem_delete(sink->wake);
        sink->wake = NULL;
    }
    if (sink->lock != NULL) {
        (void)rt_mutex_delete(sink->lock);
        sink->lock = NULL;
    }
    if (sink->ring != NULL) {
        rt_free(sink->ring);
        sink->ring = NULL;
    }
    sink->thread = NULL;
    sink->capacity = 0U;
    sink->thread_running = false;
}

bool carplay_audio_sink_is_running(void)
{
    return s_sink.running;
}

size_t carplay_audio_sink_pending(void)
{
    audio_sink_t *sink = &s_sink;
    if (!sink->running || sink->lock == NULL) return 0U;
    if (rt_mutex_take(sink->lock, RT_WAITING_FOREVER) != RT_EOK) return 0U;
    size_t pending = sink->used;
    (void)rt_mutex_release(sink->lock);
    return pending;
}

int carplay_audio_sink_pcm(uint32_t sample_rate, uint8_t channels,
                           const uint8_t *data, size_t len)
{
    audio_sink_t *sink = &s_sink;
    if (!sink->running || sink->lock == NULL) return RTCP_ESTATE;
    if (sample_rate == 0U || channels == 0U || data == NULL || len == 0U) {
        return RTCP_EINVAL;
    }

    bool accepted = false;
    if (rt_mutex_take(sink->lock, RT_WAITING_FOREVER) != RT_EOK) {
        return RTCP_ESTATE;
    }
    rt_tick_t now = rt_tick_get();
    bool gap = sink->format_valid &&
               (rt_tick_t)(now - sink->last_tick) >
                   rt_tick_from_millisecond(SINK_STREAM_GAP_MS);
    if (!sink->format_valid || sample_rate != sink->rate ||
        channels != sink->channels || gap) {
        /* A new stream (or a new layout): discard whatever was queued for
         * the old one instead of playing it into the new stream. */
        if (sink->format_valid && sink->used != 0U) {
            rt_kprintf("[" TAG "] %s, flushing %u queued bytes\n",
                       gap && sample_rate == sink->rate &&
                               channels == sink->channels
                           ? "stream restarted"
                           : "format changed",
                       (unsigned)sink->used);
        }
        ring_flush(sink);
        sink->rate = sample_rate;
        sink->channels = channels;
        sink->format_valid = true;
    }
    sink->last_tick = now;
    if (ring_push(sink, data, len) == len) {
        ++sink->packets;
        accepted = true;
    } else {
        sink->dropped_bytes += (unsigned)len;
        if (sink->dropped_bytes == (unsigned)len ||
            sink->packets % SINK_LOG_EVERY_PACKETS == 0U) {
            rt_kprintf("[" TAG "] ring full, dropped %u bytes total\n",
                       sink->dropped_bytes);
        }
    }
    (void)rt_mutex_release(sink->lock);

    if (accepted) (void)rt_sem_release(sink->wake);
    return RTCP_OK;
}

void carplay_audio_sink_stats(unsigned *packets, unsigned *dropped_bytes,
                              unsigned *write_errors, unsigned *replays)
{
    if (packets != NULL) *packets = s_sink.packets;
    if (dropped_bytes != NULL) *dropped_bytes = s_sink.dropped_bytes;
    if (write_errors != NULL) *write_errors = s_sink.write_errors;
    if (replays != NULL) *replays = s_sink.replays;
}

void carplay_audio_sink_set_volume_db(float volume_db)
{
    audio_sink_t *sink = &s_sink;
    if (!sink->running || sink->lock == NULL) return;

    /* A NaN or out-of-range value must never reach the codec: clamp first. */
    if (!(volume_db > SINK_VOLUME_FLOOR_DB)) volume_db = SINK_VOLUME_FLOOR_DB;
    if (volume_db > SINK_VOLUME_MAX_DB) volume_db = SINK_VOLUME_MAX_DB;
    int level = volume_db_to_percent(volume_db);

    if (rt_mutex_take(sink->lock, RT_WAITING_FOREVER) != RT_EOK) return;
    bool changed = !sink->volume_known || sink->volume_level != level;
    sink->volume_known = true;
    sink->volume_level = level;
    sink->volume_pending = true;
    (void)rt_mutex_release(sink->lock);

    if (changed) {
        /* rt_kprintf has no float support: print tenths of a dB by hand. */
        int tenths = (int)(-volume_db * 10.0f + 0.5f);
        if (tenths == 0) {
            rt_kprintf("[" TAG "] sender volume 0 dB -> %d%%\n", level);
        } else {
            rt_kprintf("[" TAG "] sender volume -%d.%d dB -> %d%%\n",
                       tenths / 10, tenths % 10, level);
        }
    }
    (void)rt_sem_release(sink->wake);
}

int carplay_audio_sink_volume_percent(void)
{
    audio_sink_t *sink = &s_sink;
    if (!sink->running || sink->lock == NULL) return -1;
    if (rt_mutex_take(sink->lock, RT_WAITING_FOREVER) != RT_EOK) return -1;
    int percent = sink->volume_known ? sink->volume_level : -1;
    (void)rt_mutex_release(sink->lock);
    return percent;
}
