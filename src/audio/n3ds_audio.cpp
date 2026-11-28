/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "audio.h"

#include <3ds.h>
#include <math.h>
#include <opus/opus_multistream.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAVEBUF_SIZE 16
bool n3ds_audio_disabled = false;

static OpusMSDecoder *decoder;
static u8 *audioBuffer;
static int samplesPerFrame;
static int sampleRate;
static int channelCount;
static ndspWaveBuf audio_wave_buf[WAVEBUF_SIZE];
static int wave_buf_idx = 0;

// --- Volume boost with a simple peak limiter ---
// The boost is applied in software after decoding. To avoid harsh clipping
// on loud moments, the gain for each audio frame is capped so the frame's
// loudest sample stays at full scale, then eased back up to the requested
// boost once things get quieter.
int n3ds_audio_gain_db = 10;
#define GAIN_RELEASE_DB_PER_SEC 8.0f

static float gain_target = 1.0f;  // Requested boost (linear)
static float gain_current = 1.0f; // Gain applied at the end of the last frame
static float gain_release = 1.0f; // Per-frame multiplier when recovering

static void gain_init(int samples_per_frame, int sample_rate) {
    int db = n3ds_audio_gain_db;
    if (db < 0)
        db = 0;
    if (db > 20)
        db = 20;
    gain_target = powf(10.0f, db / 20.0f);
    gain_current = gain_target;

    float frame_sec = (float)samples_per_frame / (float)sample_rate;
    gain_release = powf(10.0f, GAIN_RELEASE_DB_PER_SEC * frame_sec / 20.0f);
}

static void gain_apply(opus_int16 *samples, int count) {
    if (gain_target <= 1.0f || count <= 0) {
        return; // +0 dB: leave the audio untouched
    }

    int peak = 0;
    for (int i = 0; i < count; i++) {
        int a = abs(samples[i]);
        if (a > peak)
            peak = a;
    }

    // Highest gain that keeps this frame from clipping
    float safe = (peak > 0) ? 32767.0f / (float)peak : gain_target;

    // Recover slowly towards the requested boost, but never above "safe"
    float next = gain_current * gain_release;
    if (next > gain_target)
        next = gain_target;
    if (next > safe)
        next = safe;

    // Ramp smoothly when the gain rises; drop instantly when it must fall so
    // the start of a loud frame can't clip
    float start = (next < gain_current) ? next : gain_current;
    float step = (next - start) / (float)count;
    float g = start;
    for (int i = 0; i < count; i++) {
        float v = samples[i] * g;
        if (v > 32767.0f)
            v = 32767.0f;
        else if (v < -32768.0f)
            v = -32768.0f;
        samples[i] = (opus_int16)v;
        g += step;
    }
    gain_current = next;
}

static int n3ds_renderer_init(int audioConfiguration,
                              POPUS_MULTISTREAM_CONFIGURATION opusConfig,
                              void *context, int arFlags) {
    int rc;
    decoder = opus_multistream_decoder_create(
        opusConfig->sampleRate, opusConfig->channelCount, opusConfig->streams,
        opusConfig->coupledStreams, opusConfig->mapping, &rc);

    sampleRate = opusConfig->sampleRate;
    channelCount = opusConfig->channelCount;
    samplesPerFrame = opusConfig->samplesPerFrame;
    int bytes_per_frame = sizeof(short) * channelCount * samplesPerFrame;
    gain_init(samplesPerFrame, sampleRate);

    if (ndspInit() != 0) {
        fprintf(stderr, "ndspInit() failed\n");
        return -1;
    }

    // Assign the file-level pointer (not a local) so cleanup can release it
    audioBuffer = (u8 *)linearAlloc(bytes_per_frame * WAVEBUF_SIZE);
    if (audioBuffer == NULL)
        return -1;
    memset(audioBuffer, 0, bytes_per_frame * WAVEBUF_SIZE);

    ndspChnWaveBufClear(0);
    ndspChnReset(0);
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, sampleRate);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = mix[1] = 1.0f;
    ndspChnSetMix(0, mix);

    memset(audio_wave_buf, 0, sizeof(audio_wave_buf));
    for (int i = 0; i < WAVEBUF_SIZE; i++) {
        audio_wave_buf[i].data_vaddr = &audioBuffer[i * bytes_per_frame];
        audio_wave_buf[i].status = NDSP_WBUF_DONE;
    }

    ndspChnSetPaused(0, false);

    return 0;
}

static void n3ds_renderer_cleanup() {
    if (decoder != NULL) {
        opus_multistream_decoder_destroy(decoder);
        decoder = NULL;
    }

    ndspChnWaveBufClear(0);
    ndspExit();
    if (audioBuffer != NULL) {
        linearFree(audioBuffer);
        audioBuffer = NULL;
    }
    printf("Audio renderer shutdown successfully\n");
}

static void n3ds_renderer_decode_and_play_sample(char *data, int length) {
    if (n3ds_audio_disabled ||
        (audio_wave_buf[wave_buf_idx].status != NDSP_WBUF_DONE)) {
        return;
    }

    int decodeLen = opus_multistream_decode(
        decoder, (const unsigned char *)data, length,
        (opus_int16 *)audio_wave_buf[wave_buf_idx].data_vaddr, samplesPerFrame,
        0);
    if (decodeLen < 0) {
        fprintf(stderr, "Opus error from decode: %d\n", decodeLen);
        return;
    }
    gain_apply((opus_int16 *)audio_wave_buf[wave_buf_idx].data_vaddr,
               decodeLen * channelCount);
    DSP_FlushDataCache(audio_wave_buf[wave_buf_idx].data_vaddr,
                       decodeLen * channelCount * sizeof(short));
    audio_wave_buf[wave_buf_idx].nsamples = decodeLen;
    ndspChnWaveBufAdd(0, &audio_wave_buf[wave_buf_idx]);

    wave_buf_idx = (wave_buf_idx + 1) % WAVEBUF_SIZE;
}

extern AUDIO_RENDERER_CALLBACKS audio_callbacks_mock = {
    .init = NULL,
    .cleanup = NULL,
    .decodeAndPlaySample = NULL,
    .capabilities = 0,
};

AUDIO_RENDERER_CALLBACKS audio_callbacks_n3ds = {
    .init = n3ds_renderer_init,
    .cleanup = n3ds_renderer_cleanup,
    .decodeAndPlaySample = n3ds_renderer_decode_and_play_sample,
    .capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_SLOW_OPUS_DECODER,
};
