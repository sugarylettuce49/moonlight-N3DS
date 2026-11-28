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

#pragma once

#include <Limelight.h>

#include <stdbool.h>

#define MAX_INPUTS 6

enum VIDEO_DECODER_TYPE {
    HARDWARE_VIDEO_DECODER = 0,
    SOFTWARE_VIDEO_DECODER,
    DISABLE_VIDEO_DECODER
};

// What the top screen does when the 3D slider is raised
enum STEREO_3D_MODE {
    // Show the full image to both eyes with a small horizontal offset, so the
    // flat stream floats in front of the screen. Works with any content.
    STEREO_3D_POPOUT = 0,
    // The host sends side-by-side 3D (left half = left eye, right half =
    // right eye), e.g. a game's SBS mode or a ReShade 3D shader.
    STEREO_3D_SIDE_BY_SIDE,
    // Ignore the 3D slider and always show 2D.
    STEREO_3D_OFF,
};

// Active 3D mode used by the top screen renderer (set when a stream starts)
extern STEREO_3D_MODE n3ds_stereo_3d_mode;

// Extra audio volume applied by the 3DS audio renderer, in dB
#define AUDIO_GAIN_DB_MIN 0
#define AUDIO_GAIN_DB_MAX 20
#define AUDIO_GAIN_DB_DEFAULT 10

typedef struct _CONFIGURATION {
    STREAM_CONFIGURATION stream;
    char *app;
    char *action;
    char *address;
    char *mapping;
    char *platform;
    char *audio_device;
    char *config_file;
    char key_dir[4096];
    bool sops;
    bool localaudio;
    bool unsupported;
    bool quitappafter;
    bool viewonly;
    bool mouse_emulation;
    char *inputs[MAX_INPUTS];
    int inputsCount;
    bool hdr;
    unsigned short port;
    VIDEO_DECODER_TYPE video_decoder;
    bool motion_controls;
    bool swap_face_buttons;
    bool swap_triggers_and_shoulders;
    bool use_triggers_for_mouse;
    STEREO_3D_MODE stereo_3d_mode;
    int audio_gain_db;
} CONFIGURATION, *PCONFIGURATION;

bool config_file_parse(char *filename, PCONFIGURATION config);
void config_parse(int argc, char *argv[], PCONFIGURATION config);
void parse_argument(int c, char *value, PCONFIGURATION config);
void config_save(char *filename, PCONFIGURATION config);
