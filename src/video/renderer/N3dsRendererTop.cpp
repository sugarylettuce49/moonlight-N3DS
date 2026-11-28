/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015 Iwan Timmer
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

#include "N3dsRenderer.hpp"

#include <3ds.h>
#include <cstdlib>
#include <cstring>
#include <stdbool.h>
#include <stdexcept>
#include <unistd.h>

N3dsRendererTop::N3dsRendererTop(int dest_width, int dest_height, int src_width,
                                 int src_height, int px_size, bool debug_in)
    : N3dsRendererBase(GFX_TOP, dest_width, dest_height, src_width, src_height,
                       px_size, debug_in) {}

void N3dsRendererTop::write_px_to_framebuffer(uint8_t *source) {
    float slider = osGet3DSliderState();
    StereoRender stereo = StereoRender::MONO;
    if (slider > 0.0f) {
        switch (n3ds_stereo_3d_mode) {
        case STEREO_3D_POPOUT:
            stereo = StereoRender::POPOUT;
            break;
        case STEREO_3D_SIDE_BY_SIDE:
            stereo = StereoRender::SIDE_BY_SIDE;
            break;
        default:
            break;
        }
    }

    if (stereo == StereoRender::MONO) {
        ensure_3d_disabled();
    } else {
        ensure_3d_enabled();
    }
    write_px_to_framebuffer_gpu(source, stereo, slider);
}

void N3dsRendererTop::set_perf_decode_ticks(u64 ticks) {
    perf_decode_ticks = ticks;
}