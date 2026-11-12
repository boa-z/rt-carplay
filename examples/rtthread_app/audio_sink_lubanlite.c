/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Luban-Lite audio worker: bounded independent RTP queues -> AAC/Opus/LPCM
 * decode -> stereo mixer/resampler -> sound0. The network callback only copies
 * one authenticated payload under a bounded queue lock; all decoding and device
 * writes belong to the worker. */
#include "audio_sink_lubanlite.h"

#include <rtthread.h>
#include "rtcp_rtthread.h"
#include <rtdevice.h>
#include <drivers/audio.h>

#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <math.h>
#include "audio_jitter.h"
#include "audio_mixer.h"
#include "audio_codec.h"
#include "audio_worker_stack.h"
#include "audio_focus.h"
#if defined(RTCARPLAY_MICROPHONE)
#include "audio_input_lubanlite.h"
#endif

#include "rtcp/rtcp.h"

#define TAG "carplay-aud"
#define SINK_DEVICE_NAME "sound0"

#define SINK_THREAD_STACK_BYTES CARPLAY_AUDIO_WORKER_STACK_BYTES
#define SINK_THREAD_PRIORITY 18U
#define SINK_THREAD_TICK 20U
#define SINK_AMP_SETTLE_MS 200U /* aplay's power-amplifier settle value */
/* The AirPlay volume is in dB over [-144, 0].  Below -45 dB everything is
 * inaudible, so [-45, 0] maps linearly onto the codec's 0..100 mixer range. */
#define SINK_VOLUME_FLOOR_DB (-45.0f)
#define SINK_VOLUME_MAX_DB 0.0f
#define PROBE_MAX_FRAMES (48000U * 2U)
enum { PROBE_IDLE, PROBE_CAPTURE, PROBE_READY, PROBE_REPLAY };
enum { PROBE_CMD_CAPTURE=1, PROBE_CMD_REPLAY, PROBE_CMD_STOP, PROBE_CMD_CLEAR };

typedef struct {
    atomic_uint decoded, frames, empty, init_errors, decode_errors;
    atomic_uint negative, gap_frames, resets, mix_drops, queued, queue_busy;
    atomic_uint peak_pcm, decode_max_us, arrival_max_ms;
} lane_diagnostics_t;

typedef struct {
    rt_device_t dev;
    rt_thread_t thread;
    rt_sem_t wake;
    rt_mutex_t lock;

    rtcp_audio_jitter_t *queue[RTCP_AUDIO_LANES];
    rtcp_audio_format_t format[RTCP_AUDIO_LANES];
    unsigned generation[RTCP_AUDIO_LANES];
    uint64_t last_arrival[RTCP_AUDIO_LANES];
    rtcp_audio_mixer_t *mixer;
    rtcp_audio_focus_t focus;
    bool ducked, local_critical;
    float duck_db;
    uint32_t duck_ms;
    uint32_t local_timestamp;

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

    atomic_bool stop;
    atomic_bool running;
    atomic_bool thread_running;

    atomic_uint packets;
    atomic_uint dropped_bytes;
    atomic_uint write_errors;
    atomic_uint replays;
    atomic_uint config_errors;
    lane_diagnostics_t diag[RTCP_AUDIO_LANES];
    atomic_uint output_frames, output_peak, device_errors, write_max_us;
    atomic_bool direct_requested, direct_active;
    /* Optional two-second decoded-PCM probe. Buffer and cursor are writer
     * owned; shell/producer threads only exchange commands and snapshots. */
    int16_t *probe_pcm;
    size_t probe_cursor;
    unsigned probe_generation;
    atomic_uint probe_command, probe_state, probe_frames, probe_rate, probe_ignored;
    /* Writer-owned workspace; never copied onto the nested codec stack. */
    uint8_t packet[RTCP_AUDIO_PACKET_MAX];
    int16_t pcm[5760], silence[960], chunk[960];
} audio_sink_t;

static audio_sink_t s_sink;

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

    /* Remote volume is software-only: local critical alerts bypass it. */
    unsigned gain = level == 0 ? 0U : (unsigned)(powf(10.0f, ((float)level-100.0f)*0.45f/20.0f)*32768.0f);
    rtcp_audio_mixer_master(sink->mixer, gain, level == 0);
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

static void maximum(atomic_uint *value,unsigned sample)
{
    unsigned old=atomic_load(value);
    while(sample>old && !atomic_compare_exchange_weak(value,&old,sample)) {}
}
static unsigned pcm_peak(const int16_t *pcm,size_t samples)
{
    unsigned peak=0;
    for(size_t i=0;i<samples;++i) {
        int sample=pcm[i];unsigned mag=(unsigned)(sample<0?-sample:sample);
        if(mag>peak)peak=mag;
    }
    return peak;
}

/* ------------------------------------------------------------------ */
/* Writer thread                                                       */
/* ------------------------------------------------------------------ */

static uint64_t sink_now_us(void)
{
    return rtcp_os_rtthread.now_us();
}

/* Writer only: rate switches stop the old replay before configure. */
static bool output_samples(audio_sink_t *sink,const int16_t *pcm,size_t frames,
                           unsigned rate,unsigned channels)
{
    if(sink->device_started && (sink->device_rate!=rate || sink->device_channels!=channels))stop_device(sink);
    if(!sink->device_started && !start_device(sink,rate,(uint8_t)channels)) {
        ++sink->write_errors;++sink->device_errors;return false;
    }
    maximum(&sink->output_peak,pcm_peak(pcm,frames*channels));
    size_t bytes=frames*channels*2,offset=0;
    uint64_t began=sink_now_us();
    while(offset<bytes && !sink->stop) {
        size_t written=rt_device_write(sink->dev,0,(const uint8_t *)pcm+offset,bytes-offset);
        if(!written || written>bytes-offset) {++sink->write_errors;++sink->device_errors;break;}
        offset+=written;
    }
    maximum(&sink->write_max_us,(unsigned)(sink_now_us()-began));
    sink->output_frames+=(unsigned)(offset/(channels*2));
    return offset==bytes;
}
/* Debug direct media output must yield to other remote and local audio. */
static bool media_only(audio_sink_t *sink)
{
    bool alone=true;
    (void)rt_mutex_take(sink->lock,RT_WAITING_FOREVER);
    uint64_t now=sink_now_us();
    for(unsigned i=0;i<RTCP_AUDIO_LANES;++i)if(i!=2) {
        rtcp_audio_jitter_stats_t q;rtcp_audio_jitter_stats(sink->queue[i],&q);
        if(q.queued || (sink->last_arrival[i] && now>=sink->last_arrival[i] && now-sink->last_arrival[i]<500000))alone=false;
    }
    (void)rt_mutex_release(sink->lock);
    for(unsigned i=0;i<RTCP_AUDIO_LANES;++i)if(i!=2) {
        rtcp_audio_lane_stats_t m;rtcp_audio_mixer_stats(sink->mixer,i,&m);
        if(m.queued_frames)alone=false;
    }
    return alone;
}

static bool output_media(audio_sink_t *sink,int16_t *pcm,size_t frames,
                         unsigned rate,unsigned media_gain)
{
    if(sink->device_started && sink->device_rate!=rate)stop_device(sink);
    if(!sink->device_started && !start_device(sink,rate,2)) {
        ++sink->write_errors;++sink->device_errors;return false;
    }
    unsigned master=sink->applied_volume<0?32768:sink->applied_volume==0?0:
        (unsigned)(powf(10,((float)sink->applied_volume-100)*0.45f/20)*32768);
    unsigned gain=(unsigned)((uint64_t)media_gain*master/32768);
    for(size_t i=0;i<frames*2;++i)pcm[i]=(int16_t)((int64_t)pcm[i]*gain/32768);
    atomic_store(&sink->direct_active,true);
    return output_samples(sink,pcm,frames,rate,2);
}

static void probe_request(audio_sink_t *sink)
{
    unsigned cmd=atomic_exchange(&sink->probe_command,0);
    if(!cmd)return;
    if(cmd==PROBE_CMD_CAPTURE) {
        if(!sink->probe_pcm)sink->probe_pcm=malloc(PROBE_MAX_FRAMES*2*sizeof(int16_t));
        sink->probe_frames=0;sink->probe_rate=0;sink->probe_ignored=0;
        sink->probe_state=sink->probe_pcm?PROBE_CAPTURE:PROBE_IDLE;
        rt_kprintf("[carplay-aud] probe %s\n",sink->probe_pcm?"capturing decoded media (2 seconds)":"allocation failed");
    } else if(cmd==PROBE_CMD_REPLAY) {
        if(sink->probe_state!=PROBE_READY || !sink->probe_frames) {
            rt_kprintf("[carplay-aud] probe replay requires a ready capture\n");return;
        }
        /* Discard live media during replay; do not replay a stale backlog
         * afterwards. A fresh decoder is created on the next live packet. */
        (void)rt_mutex_take(sink->lock,RT_WAITING_FOREVER);
        sink->probe_state=PROBE_REPLAY;
        rtcp_audio_jitter_flush(sink->queue[2]);++sink->generation[2];
        (void)rt_mutex_release(sink->lock);
        rtcp_audio_mixer_flush(sink->mixer,2);sink->probe_cursor=0;
        rt_kprintf("[carplay-aud] probe replay frames=%u rate=%u\n",sink->probe_frames,sink->probe_rate);
    } else {
        sink->probe_state=sink->probe_frames?PROBE_READY:PROBE_IDLE;
        if(cmd==PROBE_CMD_CLEAR) {
            free(sink->probe_pcm);sink->probe_pcm=NULL;
            sink->probe_frames=0;sink->probe_rate=0;sink->probe_state=PROBE_IDLE;
        }
    }
}

static void probe_capture(audio_sink_t *sink,const int16_t *pcm,size_t frames,
                          unsigned rate,unsigned gen)
{
    if(sink->probe_state!=PROBE_CAPTURE || rate<8000 || rate>48000)return;
    unsigned have=sink->probe_frames;
    if(!have) {sink->probe_rate=rate;sink->probe_generation=gen;}
    if(rate!=sink->probe_rate || gen!=sink->probe_generation) {
        sink->probe_state=PROBE_READY;
        rt_kprintf("[carplay-aud] probe capture ended: media stream changed\n");return;
    }
    unsigned target=rate*2;
    if(target>PROBE_MAX_FRAMES)target=PROBE_MAX_FRAMES;
    if(frames>target-have)frames=target-have;
    memcpy(sink->probe_pcm+have*2,pcm,frames*2*sizeof(int16_t));
    sink->probe_frames=have+(unsigned)frames;
    if(sink->probe_frames==target) {
        sink->probe_state=PROBE_READY;
        rt_kprintf("[carplay-aud] probe ready frames=%u rate=%u\n",sink->probe_frames,sink->probe_rate);
    }
}

static void sink_thread(void *argument)
{
    audio_sink_t *sink = argument;
    rtcp_audio_decoder_t *decoders[RTCP_AUDIO_LANES] = {0};
    unsigned generation[RTCP_AUDIO_LANES] = {0};
    unsigned stack_report_packets=0;
    unsigned trace_count[RTCP_AUDIO_LANES]={0};
    unsigned gap_trace_count[RTCP_AUDIO_LANES]={0};
    uint32_t previous_ts[RTCP_AUDIO_LANES]={0};
    bool previous_path=false;
    unsigned last_gain[RTCP_AUDIO_LANES];
    for(unsigned i=0;i<RTCP_AUDIO_LANES;++i)last_gain[i]=32768;
    uint32_t expected[RTCP_AUDIO_LANES] = {0};
    bool timeline[RTCP_AUDIO_LANES] = {false};
    rt_kprintf("[" TAG "] writer running: independent streams, 48kHz stereo mixer\n");
    while (!sink->stop) {
        /* Drain decoded audio under sound0 backpressure. A network wake is
         * not a playback clock: waiting every chunk adds gaps, while rendering
         * an incomplete chunk inserts silence at AAC packet boundaries. */
        if (sink->probe_state!=PROBE_REPLAY && rtcp_audio_mixer_buffered(sink->mixer) < 480)
            (void)rt_sem_take(sink->wake, rt_tick_from_millisecond(10));
        probe_request(sink);
        service_volume(sink);
        bool requested=atomic_load(&sink->direct_requested);
        if(requested!=previous_path) {
            rtcp_audio_mixer_flush(sink->mixer,2);timeline[2]=false;previous_path=requested;
        }
        rtcp_audio_focus_t focus;
        bool ducked,critical; float duck_db; uint32_t ramp;
        (void)rt_mutex_take(sink->lock, RT_WAITING_FOREVER);
        focus=sink->focus;ducked=sink->ducked;duck_db=sink->duck_db;ramp=sink->duck_ms;critical=sink->local_critical;
        (void)rt_mutex_release(sink->lock);
        rtcp_audio_lane_stats_t alt,local;
        rtcp_audio_mixer_stats(sink->mixer,1,&alt);rtcp_audio_mixer_stats(sink->mixer,RTCP_AUDIO_LOCAL_LANE,&local);
        if (critical && !local.queued_frames) {
            (void)rt_mutex_take(sink->lock, RT_WAITING_FOREVER);
            rtcp_audio_jitter_stats_t pending;
            rtcp_audio_jitter_stats(sink->queue[RTCP_AUDIO_LOCAL_LANE], &pending);
            if (!pending.queued) { sink->local_critical=false; critical=false; }
            (void)rt_mutex_release(sink->lock);
        }
        focus.navigation_active=alt.queued_frames!=0;
        focus.critical_active=critical && local.queued_frames!=0;
        focus.local_active=!critical && local.queued_frames!=0;
        rtcp_audio_focus_policy_t policy;rtcp_audio_focus_policy(&focus,&policy);
        if(ducked)policy.media_gain=(unsigned)((uint64_t)policy.media_gain*(unsigned)(powf(10,duck_db/20)*32768)/32768);
        unsigned gains[RTCP_AUDIO_LANES]={policy.main_gain,policy.alternate_gain,policy.media_gain,
            policy.main_gain,policy.main_gain,policy.alternate_gain,policy.main_gain,policy.media_gain,policy.local_gain};
        rtcp_audio_lane_stats_t call,siri;
        rtcp_audio_mixer_stats(sink->mixer,3,&call);rtcp_audio_mixer_stats(sink->mixer,4,&siri);
        if(call.queued_frames || siri.queued_frames){gains[0]=gains[2]=gains[6]=gains[7]=0;}
        if(focus.critical_active)for(unsigned i=0;i<AIRPLAY_AUDIO_MAX_STREAMS;++i)gains[i]=0;
        for(unsigned i=0;i<RTCP_AUDIO_LANES;++i)if(gains[i]!=last_gain[i]) {
            rtcp_audio_mixer_gain(sink->mixer,i,gains[i],focus.critical_active ? 0 : ramp*48U);last_gain[i]=gains[i];
        }
        if(sink->probe_state==PROBE_REPLAY) {
            if(focus.call_active || focus.siri_active || critical || !media_only(sink)) {
                sink->probe_state=PROBE_READY;
                rt_kprintf("[carplay-aud] probe replay interrupted by other audio\n");
            } else {
                size_t n=(size_t)sink->probe_frames-sink->probe_cursor;
                if(n>1024)n=1024;
                memcpy(sink->pcm,sink->probe_pcm+sink->probe_cursor*2,n*2*sizeof(int16_t));
                bool complete=output_media(sink,sink->pcm,n,sink->probe_rate,gains[2]);
                sink->probe_cursor+=n;
                if(!complete || sink->probe_cursor==sink->probe_frames) {
                    stop_device(sink); /* flush the final partial replay block */
                    sink->probe_state=PROBE_READY;
                    rt_kprintf("[carplay-aud] probe replay %s\n",complete?"done":"write failed");
                }
                continue;
            }
        }
        for (unsigned lane = 0; lane < RTCP_AUDIO_LANES && !sink->stop; ++lane) {
            for (unsigned budget = 0; budget < 8; ++budget) {
                uint8_t *packet=sink->packet;
                rtcp_audio_format_t format;
                uint32_t timestamp;
                unsigned gen;
                int got;
                rtcp_audio_lane_stats_t stats;
                rtcp_audio_mixer_stats(sink->mixer, lane, &stats);
                atomic_store(&sink->diag[lane].queued,(unsigned)stats.queued_frames);
                if (stats.queued_frames > 2880) { /* bounded decode ahead */
                    (void)rt_mutex_take(sink->lock, RT_WAITING_FOREVER);
                    bool unchanged = generation[lane] == sink->generation[lane];
                    (void)rt_mutex_release(sink->lock);
                    if (unchanged) break; /* flush TEARDOWN/critical replacements promptly */
                }
                if (rt_mutex_take(sink->lock, RT_WAITING_FOREVER) != RT_EOK) break;
                gen = sink->generation[lane];
                format = sink->format[lane];
                got = rtcp_audio_jitter_pop(sink->queue[lane], packet, RTCP_AUDIO_PACKET_MAX, &timestamp, sink_now_us());
                (void)rt_mutex_release(sink->lock);
                if (gen != generation[lane]) {
                    rtcp_audio_decoder_destroy(decoders[lane]); decoders[lane] = NULL;
                    rtcp_audio_mixer_flush(sink->mixer, lane); generation[lane] = gen; timeline[lane] = false;
                    trace_count[lane]=0;gap_trace_count[lane]=0;++sink->diag[lane].resets;
                }
                if (got <= 0) break;
                if (!decoders[lane] && rtcp_audio_decoder_create(&format, &decoders[lane]) != RTCP_OK) {
                    ++sink->write_errors;++sink->diag[lane].init_errors; continue;
                }
                uint64_t decode_began=sink_now_us();
                int16_t *pcm=sink->pcm; size_t samples = 0;
                if (rtcp_audio_decoder_decode(decoders[lane], packet, (size_t)got, pcm, 5760, &samples) != RTCP_OK) {
                    ++sink->write_errors;++sink->diag[lane].decode_errors; continue;
                }
                lane_diagnostics_t *d=&sink->diag[lane];
                maximum(&d->decode_max_us,(unsigned)(sink_now_us()-decode_began));
                ++d->decoded;d->frames+=(unsigned)(samples/format.channels);
                maximum(&d->peak_pcm,pcm_peak(pcm,samples));
                if(trace_count[lane]<8) {
                    rt_kprintf("[carplay-aud] lane=%u codec=%u rate=%u clock=%u payload=%u ts=%u step=%d frames=%u\n",
                      lane,(unsigned)format.codec,format.sample_rate,format.clock_rate,(unsigned)got,timestamp,
                      trace_count[lane]?(int32_t)(timestamp-previous_ts[lane]):0,(unsigned)(samples/format.channels));
                    ++trace_count[lane];
                }
                previous_ts[lane]=timestamp;
                /* FAAD priming may emit no samples; it does not establish a
                 * decoded timeline and must not synthesize a following gap. */
                if(!samples) {++d->empty;continue;}
                if(lane==2 && format.channels==2)
                    probe_capture(sink,pcm,samples/2,format.sample_rate,gen);
                bool direct=requested && lane==2 && format.channels==2 &&
                    !focus.call_active && !focus.siri_active && !critical && media_only(sink);
                if(direct) {
                    rtcp_audio_mixer_flush(sink->mixer,2);timeline[2]=false;
                    (void)output_media(sink,pcm,samples/2,format.sample_rate,gains[2]);
                    if(sink->packets>=stack_report_packets+200) {
                        carplay_audio_stack_report(TAG);stack_report_packets=sink->packets;
                    }
                    continue;
                }
                if (timeline[lane]) {
                    int32_t gap = (int32_t)(timestamp - expected[lane]);
                    if(gap && gap_trace_count[lane]<8) {
                        rt_kprintf("[carplay-aud] gap lane=%u expected=%u ts=%u delta=%d frames=%u\n",
                          lane,expected[lane],timestamp,gap,(unsigned)(samples/format.channels));
                        ++gap_trace_count[lane];
                    }
                    if (gap < 0) {++d->negative;continue;}
                    if (gap > 0 && (uint64_t)gap * 1000 / format.clock_rate <= 100) {
                        int16_t *silence=sink->silence;
                        size_t missing = (size_t)((uint64_t)gap * format.sample_rate / format.clock_rate);
                        d->gap_frames+=(unsigned)missing;
                        while (missing) {
                            size_t n = missing > 480 ? 480 : missing;
                            if(rtcp_audio_mixer_push(sink->mixer,lane,silence,n,format.sample_rate,format.channels)!=RTCP_OK)
                                d->mix_drops+=(unsigned)n;
                            missing -= n;
                        }
                    } else if (gap > 0) rtcp_audio_mixer_flush(sink->mixer, lane);
                }
                timeline[lane] = true;
                expected[lane] = timestamp + (uint32_t)((uint64_t)(samples/format.channels)*format.clock_rate/format.sample_rate);
                if (samples && rtcp_audio_mixer_push(sink->mixer, lane, pcm, samples/format.channels,
                                                     format.sample_rate, format.channels) != RTCP_OK)
                    { sink->dropped_bytes += (unsigned)(samples * 2);d->mix_drops+=(unsigned)(samples/format.channels); }
            }
        }
        if (rtcp_audio_mixer_buffered(sink->mixer) < 480) {
            /* A short, finite local alert must not wait for another packet.
             * Preserve its existing padded playback behavior. */
            rtcp_audio_lane_stats_t pending_local;
            rtcp_audio_mixer_stats(sink->mixer, RTCP_AUDIO_LOCAL_LANE, &pending_local);
            if (!pending_local.queued_frames) continue;
        }
        /* Apply a queued volume before rendering the first samples too. */
        if(sink->device_started && sink->device_rate!=48000)stop_device(sink);
        if (!sink->device_started && !start_device(sink, 48000, 2)) { ++sink->write_errors; continue; }
        if(sink->packets>=stack_report_packets+200) {
            carplay_audio_stack_report(TAG);stack_report_packets=sink->packets;
        }
        int16_t *chunk=sink->chunk;
        size_t frames = rtcp_audio_mixer_render(sink->mixer, chunk, 480);
        if (!frames) continue;
        atomic_store(&sink->direct_active,false);
        bool complete=output_samples(sink,chunk,frames,48000,2);
#if defined(RTCARPLAY_MICROPHONE)
        if(complete)carplay_audio_input_reference(chunk,frames);
#else
        (void)complete;
#endif

    }
    for (unsigned i = 0; i < RTCP_AUDIO_LANES; ++i) rtcp_audio_decoder_destroy(decoders[i]);
    stop_device(sink);
    free(sink->probe_pcm);sink->probe_pcm=NULL;sink->probe_state=PROBE_IDLE;
    carplay_audio_stack_report(TAG);
    rt_kprintf("[" TAG "] stopped: %u packets, %u replay(s), %u dropped bytes, %u errors\n",
               sink->packets, sink->replays, sink->dropped_bytes, sink->write_errors);
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

    sink->mixer = rtcp_audio_mixer_create(8192);
    rtcp_audio_mixer_auto_duck(sink->mixer,false);
    sink->duck_ms=100;
    for (unsigned i = 0; i < RTCP_AUDIO_LANES; ++i) sink->queue[i] = rtcp_audio_jitter_create(i==RTCP_AUDIO_LOCAL_LANE?4:16, 20000);
    sink->wake = rt_sem_create("caud", 0, RT_IPC_FLAG_FIFO);
    sink->lock = rt_mutex_create("caudq", RT_IPC_FLAG_FIFO);
    bool queues_ok=true;
    for(unsigned i=0;i<RTCP_AUDIO_LANES;++i)if(!sink->queue[i])queues_ok=false;
    if (!sink->mixer || !queues_ok || !sink->wake || !sink->lock) {
        rtcp_audio_mixer_destroy(sink->mixer);
        for (unsigned i = 0; i < RTCP_AUDIO_LANES; ++i) rtcp_audio_jitter_destroy(sink->queue[i]);
        if (sink->wake != NULL) (void)rt_sem_delete(sink->wake);
        if (sink->lock != NULL) (void)rt_mutex_delete(sink->lock);
        (void)rt_device_close(sink->dev);
        memset(sink, 0, sizeof(*sink));
        return RTCP_ENOMEM;
    }


    sink->stop = false;
    sink->running = true;
    sink->thread = rt_thread_create("carplay-aud", sink_thread, sink,
                                    SINK_THREAD_STACK_BYTES,
                                    SINK_THREAD_PRIORITY, SINK_THREAD_TICK);
    if (sink->thread == NULL) {
        sink->running = false;
        rtcp_audio_mixer_destroy(sink->mixer);
        for (unsigned i = 0; i < RTCP_AUDIO_LANES; ++i) rtcp_audio_jitter_destroy(sink->queue[i]);
        (void)rt_sem_delete(sink->wake);
        (void)rt_mutex_delete(sink->lock);
        (void)rt_device_close(sink->dev);
        memset(sink, 0, sizeof(*sink));
        return RTCP_ENOMEM;
    }
    sink->thread_running = true; /* closes stop-before-entry race */
    if (rt_thread_startup(sink->thread) != RT_EOK) {
        sink->thread_running = false;
        (void)rt_thread_delete(sink->thread);
        carplay_audio_sink_stop(); return RTCP_EIO;
    }
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
    rtcp_audio_mixer_destroy(sink->mixer); sink->mixer = NULL;
    for (unsigned i = 0; i < RTCP_AUDIO_LANES; ++i) { rtcp_audio_jitter_destroy(sink->queue[i]); sink->queue[i] = NULL; }
    sink->thread = NULL;
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
    size_t pending = 0;
    for (unsigned i = 0; i < RTCP_AUDIO_LANES; ++i) {
        rtcp_audio_jitter_stats_t stats;
        rtcp_audio_jitter_stats(sink->queue[i], &stats); pending += stats.queued;
    }
    (void)rt_mutex_release(sink->lock);
    return pending;
}

int carplay_audio_sink_packet(uint16_t type, const rtcp_audio_format_t *format,
                               const uint8_t *data, size_t len, uint32_t timestamp, uint64_t arrival)
{
    audio_sink_t *sink = &s_sink;
    if (!sink->running || sink->lock == NULL) return RTCP_ESTATE;
    if (type < 100 || type > 102 || !format || !format->sample_rate ||
        (format->channels != 1 && format->channels != 2) || !data || !len) return RTCP_EINVAL;
    unsigned i = format->stream_slot_valid ? format->stream_slot : (unsigned)(type-100);
    if(i>=AIRPLAY_AUDIO_MAX_STREAMS || airplay_audio_slot_type(i)!=type)return RTCP_EINVAL;
    /* A high-priority network thread can preempt the writer while it holds
     * this short copy-only lock. Give priority inheritance a bounded chance
     * to release it instead of silently dropping every such datagram. No
     * decoding/device I/O is allowed while holding the queue lock. */
    if (rt_mutex_take(sink->lock, rt_tick_from_millisecond(2)) != RT_EOK) {
        ++sink->diag[i].queue_busy;sink->dropped_bytes+=(unsigned)len;
        return RTCP_ETIMEDOUT;
    }
    if(i==2 && sink->probe_state==PROBE_REPLAY) {
        ++sink->probe_ignored;
        (void)rt_mutex_release(sink->lock);return RTCP_OK;
    }
    if ((sink->last_arrival[i] && arrival > sink->last_arrival[i] && arrival-sink->last_arrival[i] > 500000) ||
        sink->format[i].sample_rate != format->sample_rate || sink->format[i].channels != format->channels ||
        sink->format[i].codec != format->codec || sink->format[i].clock_rate != format->clock_rate) {
        rtcp_audio_jitter_flush(sink->queue[i]); sink->format[i] = *format; ++sink->generation[i];
    }
    if(sink->last_arrival[i] && arrival>sink->last_arrival[i])
        maximum(&sink->diag[i].arrival_max_ms,(unsigned)((arrival-sink->last_arrival[i])/1000));
    sink->last_arrival[i] = arrival;
    int result = rtcp_audio_jitter_push(sink->queue[i], data, len, timestamp, arrival);
    if (result == RTCP_OK) ++sink->packets; else sink->dropped_bytes += (unsigned)len;
    (void)rt_mutex_release(sink->lock);
    if (result == RTCP_OK) (void)rt_sem_release(sink->wake);
    return result;
}
void carplay_audio_sink_slot_stop(unsigned slot)
{
    audio_sink_t *sink=&s_sink;
    if(!sink->running || slot>=AIRPLAY_AUDIO_MAX_STREAMS || !sink->lock)return;
    (void)rt_mutex_take(sink->lock,RT_WAITING_FOREVER);
    rtcp_audio_jitter_flush(sink->queue[slot]);++sink->generation[slot];
    (void)rt_mutex_release(sink->lock);(void)rt_sem_release(sink->wake);
}
void carplay_audio_sink_stream_stop(uint16_t type)
{
    for(unsigned i=0;i<AIRPLAY_AUDIO_MAX_STREAMS;++i)
        if(airplay_audio_slot_type(i)==type)carplay_audio_sink_slot_stop(i);
}
int carplay_audio_sink_pcm(uint32_t rate, uint8_t channels, const uint8_t *data, size_t len)
{
    /* Compatibility entry for local PCM producers; serialized on their owner. */
    static uint32_t timestamp;
    rtcp_audio_format_t f = {.sample_rate=rate, .channels=channels, .clock_rate=rate};
    uint32_t ts = timestamp;
    if (channels) timestamp += (uint32_t)(len/(2U*channels));
    return carplay_audio_sink_packet(100, &f, data, len, ts, sink_now_us());
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
            rt_kprintf("[" TAG "] output volume 0 dB -> %d%%\n", level);
        } else {
            rt_kprintf("[" TAG "] output volume -%d.%d dB -> %d%%\n",
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

void carplay_audio_sink_modes(const airplay_audio_modes_t *modes)
{
    audio_sink_t *sink=&s_sink;
    if(!modes || !sink->running || !sink->lock)return;
    (void)rt_mutex_take(sink->lock,RT_WAITING_FOREVER);
    if(modes->has_call)sink->focus.call_active=modes->call_active;
    if(modes->has_speech)sink->focus.siri_active=modes->speech_mode==1 || modes->speech_mode==2;
    if(modes->has_main_audio)sink->focus.remote_audio_entity=modes->main_audio_entity;
    (void)rt_mutex_release(sink->lock);(void)rt_sem_release(sink->wake);
}
bool carplay_audio_sink_duck(bool ducked,float db,uint32_t duration)
{
    audio_sink_t *sink=&s_sink;
    if(!sink->running || !sink->lock || !isfinite(db) || db< -144 || db>0 || duration>10000)return false;
    (void)rt_mutex_take(sink->lock,RT_WAITING_FOREVER);
    sink->ducked=ducked;sink->duck_db=db;sink->duck_ms=duration;
    (void)rt_mutex_release(sink->lock);(void)rt_sem_release(sink->wake);return true;
}
int carplay_audio_sink_local_pcm(const uint8_t *data,size_t len,uint32_t rate,uint8_t channels,bool critical)
{
    audio_sink_t *sink=&s_sink;
    if(!sink->running || !sink->lock)return RTCP_ESTATE;
    if(!data || !len || len>RTCP_AUDIO_PACKET_MAX || channels<1 || channels>2 || rate<8000 || rate>48000 || len%(channels*2U))return RTCP_EINVAL;
    if(rt_mutex_take(sink->lock,0)!=RT_EOK)return RTCP_ETIMEDOUT;
    /* A normal HMI sound cannot demote queued safety audio. A new critical
     * alert replaces pending normal local audio and its decoded generation. */
    if (!critical && sink->local_critical) {
        (void)rt_mutex_release(sink->lock);return RTCP_ESTATE;
    }
    if (critical && !sink->local_critical) {
        rtcp_audio_jitter_flush(sink->queue[RTCP_AUDIO_LOCAL_LANE]);
        ++sink->generation[RTCP_AUDIO_LOCAL_LANE];
    }
    rtcp_audio_format_t f={.sample_rate=rate,.channels=channels,.clock_rate=rate};
    if(sink->format[RTCP_AUDIO_LOCAL_LANE].sample_rate!=rate || sink->format[RTCP_AUDIO_LOCAL_LANE].channels!=channels) {
        rtcp_audio_jitter_flush(sink->queue[RTCP_AUDIO_LOCAL_LANE]);sink->format[RTCP_AUDIO_LOCAL_LANE]=f;++sink->generation[RTCP_AUDIO_LOCAL_LANE];
    }
    int rc=rtcp_audio_jitter_push(sink->queue[RTCP_AUDIO_LOCAL_LANE],data,len,sink->local_timestamp,sink_now_us());
    if(rc==RTCP_OK) { sink->local_timestamp+=(uint32_t)(len/(channels*2));sink->local_critical=critical; }
    (void)rt_mutex_release(sink->lock);if(rc==0)(void)rt_sem_release(sink->wake);return rc;
}

/* CarPlay may leave volume ownership with the vehicle. This also permits
 * checking software gain independently of a phone SET_PARAMETER request. */
static void volume_command(int argc, char **argv)
{
    if (argc == 2) {
        char *end;
        if (!argv[1][0] || strlen(argv[1]) > 3) goto usage;
        long percent = strtol(argv[1], &end, 10);
        if (*end || percent < 0 || percent > 100) goto usage;
        carplay_audio_sink_set_volume_db(percent == 0 ? -144.0f :
                                        -45.0f + (float)percent * 0.45f);
    } else if (argc != 1) goto usage;
    int percent = carplay_audio_sink_volume_percent();
    if (percent < 0) rt_kprintf("[carplay-aud] volume unset (default unity), or sink stopped\n");
    else rt_kprintf("[carplay-aud] output volume %d%%\n", percent);
    return;
usage:
    rt_kprintf("Usage: carplay_volume [0..100]\n");
}
MSH_CMD_EXPORT_ALIAS(volume_command,carplay_volume,CarPlay output volume 0..100);

static void probe_status(void)
{
    static const char *names[]={"idle","capturing","ready","replaying"};
    unsigned state=s_sink.probe_state;
    rt_kprintf("audio probe: state=%s frames=%u rate=%u ignored_live=%u\n",
      names[state],s_sink.probe_frames,s_sink.probe_rate,s_sink.probe_ignored);
}
static void audio_probe_command(int argc,char **argv)
{
    if(!s_sink.running) {rt_kprintf("[carplay-aud] receiver audio not running\n");return;}
    unsigned cmd=0;
    if(argc==2) {
        if(!strcmp(argv[1],"capture"))cmd=PROBE_CMD_CAPTURE;
        else if(!strcmp(argv[1],"replay"))cmd=PROBE_CMD_REPLAY;
        else if(!strcmp(argv[1],"stop"))cmd=PROBE_CMD_STOP;
        else if(!strcmp(argv[1],"clear"))cmd=PROBE_CMD_CLEAR;
        else if(strcmp(argv[1],"status"))goto usage;
    } else if(argc!=1)goto usage;
    if(cmd) {
        atomic_store(&s_sink.probe_command,cmd);
        (void)rt_sem_release(s_sink.wake);
        rt_kprintf("[carplay-aud] probe request queued: %s\n",argv[1]);
    } else probe_status();
    return;
usage:
    rt_kprintf("Usage: carplay_audio_probe [capture|replay|stop|clear|status]\n");
}
MSH_CMD_EXPORT_ALIAS(audio_probe_command,carplay_audio_probe,Capture decoded media and replay from RAM);

void carplay_audio_sink_diagnostics(void)
{
    audio_sink_t *s=&s_sink;
    if(!s->running)return;
    /* SDK console formatting is limited to 128 bytes per call. Keep the
     * newline and all counters even when unsigned values grow to 10 digits. */
    rt_kprintf("audio diag: requested=%s last_output=%s\n",
      s->direct_requested?"direct":"mixer",s->direct_active?"direct":"mixer");
    rt_kprintf("audio output: frames=%u peak=%u device_errors=%u write_max_us=%u\n",
      s->output_frames,s->output_peak,s->device_errors,s->write_max_us);
    if(s->probe_state!=PROBE_IDLE)probe_status();
    for(unsigned i=0;i<RTCP_AUDIO_LANES;++i) {
        lane_diagnostics_t *d=&s->diag[i];
        if(!d->decoded && !d->decode_errors && !d->init_errors && !d->queue_busy)continue;
        rtcp_audio_jitter_stats_t q={0};
        (void)rt_mutex_take(s->lock,RT_WAITING_FOREVER);
        rtcp_audio_jitter_stats(s->queue[i],&q);
        (void)rt_mutex_release(s->lock);
        rt_kprintf("audio lane=%u decoded=%u frames=%u empty=%u\n",i,d->decoded,d->frames,d->empty);
        rt_kprintf("audio lane=%u init_err=%u decode_err=%u pcm_peak=%u decode_max_us=%u\n",
          i,d->init_errors,d->decode_errors,d->peak_pcm,d->decode_max_us);
        rt_kprintf("audio lane=%u negative_ts=%u silence_frames=%u arrival_max_ms=%u\n",
          i,d->negative,d->gap_frames,d->arrival_max_ms);
        rt_kprintf("audio lane=%u resets=%u mix_drop=%u mix_queue=%u\n",i,d->resets,d->mix_drops,d->queued);
        rt_kprintf("audio lane=%u queue_busy=%u\n",i,d->queue_busy);
        rt_kprintf("audio lane=%u jitter_queue=%u late=%u duplicate=%u overflow=%u\n",i,
          (unsigned)q.queued,(unsigned)q.late,(unsigned)q.duplicates,(unsigned)q.overflow);
    }
}
static void audio_path_command(int argc,char **argv)
{
    if(argc==2 && (!strcmp(argv[1],"direct") || !strcmp(argv[1],"mixer"))) {
        if(!s_sink.running) {rt_kprintf("[carplay-aud] receiver audio not running\n");return;}
        atomic_store(&s_sink.direct_requested,!strcmp(argv[1],"direct"));
        (void)rt_sem_release(s_sink.wake);
    } else if(argc!=1) {rt_kprintf("Usage: carplay_audio_path [mixer|direct]\n");return;}
    carplay_audio_sink_diagnostics();
}
MSH_CMD_EXPORT_ALIAS(audio_path_command,carplay_audio_path,Compare mixed and direct media audio);
