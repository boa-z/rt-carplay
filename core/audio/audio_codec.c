/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "include/audio_codec.h"
#include "rtcp/rtcp.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#ifdef RTCP_AUDIO_CODECS
#include "neaacdec.h"
#include "opus.h"
#endif

struct rtcp_audio_decoder {
    rtcp_audio_format_t format;
#ifdef RTCP_AUDIO_CODECS
    NeAACDecHandle aac;
    OpusDecoder *opus;
#endif
};

bool rtcp_audio_format_parse(uint64_t bits, rtcp_audio_format_t *f)
{
    static const uint32_t rates[] = {8000,16000,24000,32000,44100};
    if (!f || !bits || (bits & (bits - 1))) return false;
    memset(f, 0, sizeof(*f));
    for (unsigned i = 0; i < 10; ++i) {
        if (bits == (UINT64_C(4) << i)) {
            f->sample_rate = rates[i / 2]; f->channels = (uint8_t)(1 + i % 2);
            f->clock_rate = f->sample_rate; return true;
        }
    }
    if (bits == 0x4000 || bits == 0x8000) {
        f->sample_rate = f->clock_rate = 48000; f->channels = bits == 0x4000 ? 1 : 2;
        return true;
    }
#ifdef RTCP_AUDIO_CODECS
    if (bits == 0x400000 || bits == 0x800000) {
        f->sample_rate = f->clock_rate = bits == 0x400000 ? 44100 : 48000;
        f->channels = 2; f->codec = RTCP_AUDIO_AAC_LC; return true;
    }
    if (bits == 0x10000000 || bits == 0x20000000 || bits == 0x40000000) {
        f->sample_rate = 48000; f->channels = 1; f->codec = RTCP_AUDIO_OPUS;
        f->clock_rate = bits == 0x10000000 ? 16000 : bits == 0x20000000 ? 24000 : 48000;
        return true;
    }
#endif
    return false;
}
uint64_t rtcp_audio_output_formats(void)
{
    uint64_t bits = 0xcffc;
#ifdef RTCP_AUDIO_CODECS
    bits |= UINT64_C(0x70c00000);
#endif
    return bits;
}
int rtcp_audio_decoder_create(const rtcp_audio_format_t *f, rtcp_audio_decoder_t **out)
{
    if (!out) return RTCP_EINVAL;
    *out = NULL;
    if (!f || (f->channels != 1 && f->channels != 2) || !f->sample_rate) return RTCP_EINVAL;
    rtcp_audio_decoder_t *d = calloc(1, sizeof(*d));
    if (!d) return RTCP_ENOMEM;
    d->format = *f;
    if (f->codec == RTCP_AUDIO_LPCM) { *out = d; return RTCP_OK; }
#ifdef RTCP_AUDIO_CODECS
    if (f->codec == RTCP_AUDIO_AAC_LC && f->channels == 2 &&
        (f->sample_rate == 44100 || f->sample_rate == 48000)) {
        d->aac = NeAACDecOpen();
        if (!d->aac) { free(d); return RTCP_ENOMEM; }
        NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(d->aac);
        cfg->defObjectType = LC;
        cfg->defSampleRate = f->sample_rate;
        cfg->outputFormat = FAAD_FMT_16BIT;
        cfg->dontUpSampleImplicitSBR = 1;
        if (!NeAACDecSetConfiguration(d->aac, cfg)) goto fail;
        unsigned fi = f->sample_rate == 44100 ? 4 : 3;
        unsigned value = (2U << 11) | (fi << 7) | (2U << 3);
        unsigned char asc[2] = {(unsigned char)(value >> 8), (unsigned char)value};
        unsigned long rate; unsigned char channels;
        if (NeAACDecInit2(d->aac, asc, sizeof(asc), &rate, &channels) != 0 ||
            rate != f->sample_rate || channels != f->channels) goto fail;
        *out = d; return RTCP_OK;
    }
    if (f->codec == RTCP_AUDIO_OPUS && f->sample_rate == 48000 && f->channels == 1) {
        int err;
        d->opus = opus_decoder_create(48000, 1, &err);
        if (!d->opus || err != OPUS_OK) goto fail;
        *out = d; return RTCP_OK;
    }
fail:
#endif
    rtcp_audio_decoder_destroy(d); return RTCP_ENOTSUP;
}
int rtcp_audio_decoder_decode(rtcp_audio_decoder_t *d, const uint8_t *data, size_t len,
                             int16_t *pcm, size_t cap, size_t *samples)
{
    if (samples) *samples = 0;
    if (!d || !data || !len || !pcm || !samples || len > 4096) return RTCP_EINVAL;
    if (d->format.codec == RTCP_AUDIO_LPCM) {
        if (len % (2U * d->format.channels) || len / 2 > cap) return RTCP_EINVAL;
        for (size_t i = 0; i < len / 2; ++i)
            pcm[i] = (int16_t)((uint16_t)data[2*i] | (uint16_t)data[2*i+1] << 8);
        *samples = len / 2; return RTCP_OK;
    }
#ifdef RTCP_AUDIO_CODECS
    if (d->aac) {
        NeAACDecFrameInfo info = {0};
        void *decoded = NeAACDecDecode(d->aac, &info, (unsigned char *)data, (unsigned long)len);
        if (info.error || !decoded || info.channels != d->format.channels ||
            info.samplerate != d->format.sample_rate || info.samples > cap ||
            info.samples > 2048 || info.bytesconsumed != len) return RTCP_EIO;
        memcpy(pcm, decoded, info.samples * sizeof(*pcm));
        *samples = info.samples; return RTCP_OK;
    }
    if (d->opus) {
        if (cap > 5760) cap = 5760;
        int n = opus_decode(d->opus, data, (opus_int32)len, pcm, (int)cap, 0);
        if (n < 0) return RTCP_EIO;
        *samples = (size_t)n; return RTCP_OK;
    }
#endif
    return RTCP_ENOTSUP;
}
void rtcp_audio_decoder_destroy(rtcp_audio_decoder_t *d)
{
    if (!d) return;
#ifdef RTCP_AUDIO_CODECS
    if (d->aac) NeAACDecClose(d->aac);
    if (d->opus) opus_decoder_destroy(d->opus);
#endif
    free(d);
}
