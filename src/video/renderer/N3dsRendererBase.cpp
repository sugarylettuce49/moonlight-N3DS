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
#include "vshader_shbin.h"

#include <3ds.h>
#include <cstdlib>
#include <cstring>
#include <stdbool.h>
#include <stdexcept>
#include <unistd.h>

STEREO_3D_MODE n3ds_stereo_3d_mode = STEREO_3D_POPOUT;

N3dsRendererBase::N3dsRendererBase(gfxScreen_t screen_in, int surface_width_in,
                                   int surface_height_in, int image_width_in,
                                   int image_height_in, int pixel_size,
                                   bool debug_in)
    : screen(screen_in), surface_width(surface_width_in),
      surface_height(surface_height_in), image_width(image_width_in),
      image_height(image_height_in), debug(debug_in), px_size(pixel_size) {
    cmdlist = (u32 *)linearAlloc(CMDLIST_SZ * 4);
    // In 3D, the top screen always renders both eyes (2 x 400px) into this
    // buffer, even for 400px wide streams
    int fb_width = (screen == GFX_TOP) ? GSP_SCREEN_HEIGHT_TOP_2X : surface_width;
    vramFb = vramAlloc(fb_width * surface_height * px_size);
    // Needs to be able to hold an 800x480
    vramTex = vramAlloc(MOON_CTR_VIDEO_TEX_W * MOON_CTR_VIDEO_TEX_H * px_size);
}

N3dsRendererBase::~N3dsRendererBase() {
    linearFree(cmdlist);
    vramFree(vramFb);
    vramFree(vramTex);

    // Clear target screen
    u8 *framebuf = gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL);
    memset(framebuf, 0, surface_width * surface_height * px_size);
    gfxScreenSwapBuffers(screen, true);

    // Leave 3D mode, otherwise the menus would keep showing the last stream
    // frame in the right eye
    if (screen == GFX_TOP && gfxIs3D()) {
        gfxSet3D(false);
    }

    // Return to the default display width before exiting
    if (surface_width == GSP_SCREEN_HEIGHT_TOP_2X) {
        gfxSetWide(false);
    }
}

void N3dsRendererBase::ensure_3d_enabled() {
    if (!gfxIs3D()) {
        gfxSetWide(false);
        gfxSet3D(true);
    }
}

void N3dsRendererBase::ensure_3d_disabled() {
    if (gfxIs3D()) {
        gfxSet3D(false);
    }
    if (surface_width == GSP_SCREEN_HEIGHT_TOP_2X) {
        gfxSetWide(true);
    }
}

inline void N3dsRendererBase::write24(u8 *p, u32 val) {
    p[0] = val;
    p[1] = val >> 8;
    p[2] = val >> 16;
}

inline void N3dsRendererBase::draw_perf_counters() {
    u8 *dest = gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL);

    // Use a line going across the first scanline (left) for the perf counters.
    // Clear to black
    memset(dest, 0, GSP_SCREEN_WIDTH * 3);

    // Display frame target in the middle of the screen.
    double perf_tick_divisor =
        ((double)GSP_SCREEN_WIDTH) / ((double)(perf_frame_target_ticks * 2));
    u32 perf_px = 0;
    u32 perf_tmp_height = 0;

#define PERF_DRAW(ticks, r, g, b)                                              \
    perf_tmp_height = perf_tick_divisor * ((double)(ticks));                   \
    do {                                                                       \
        if (perf_px > GSP_SCREEN_WIDTH)                                        \
            break;                                                             \
        const u32 color = (r << 16) | (g << 8) | b;                            \
        memcpy(dest + (perf_px * 3), &color, 3);                               \
        perf_px++;                                                             \
    } while (perf_tmp_height-- > 0);

    PERF_DRAW(perf_decode_ticks, 255, 0, 0);
    PERF_DRAW(perf_fbcopy_ticks, 0, 0, 255);

    // Draw two green pixels at the center
    perf_px = (GSP_SCREEN_WIDTH / 2) - 1;
    PERF_DRAW(0, 0, 255, 0);
    PERF_DRAW(0, 0, 255, 0);
}

void N3dsRendererBase::write_px_to_framebuffer_gpu(uint8_t *__restrict source,
                                                   StereoRender stereo,
                                                   float depth) {
    // Do nothing when GPU right is lost, otherwise we hang when going to
    // the home menu.
    if (!gspHasGpuRight()) {
        return;
    }

    if (screen != GFX_TOP) {
        stereo = StereoRender::MONO;
    }
    bool is_3d = stereo != StereoRender::MONO;

    // In 3D both eyes are rendered into one 800px tall (rotated) buffer:
    // the first 400px become the left eye, the last 400px the right eye.
    int render_width = is_3d ? GSP_SCREEN_HEIGHT_TOP_2X : surface_width;

    u64 start_ticks = svcGetSystemTick();

    // NOTE: At 800x480, we can display the _width_ natively, but the height
    // needs to be downsampled. MVD is incapable of downsampling, so we have to
    // do it on the GPU.

    // TODO: If we can use rotation from the decoder, we can do a 2x downscale
    // using display transfer and skip P3D. Not necessary because PICA is
    // significantly faster than the decoder.

    // Tile the source image into the scratch buffer.
    GX_DisplayTransfer(
        (u32 *)source,
        GX_BUFFER_DIM(MOON_CTR_VIDEO_TEX_W, MOON_CTR_VIDEO_TEX_H),
        (u32 *)vramTex,
        GX_BUFFER_DIM(MOON_CTR_VIDEO_TEX_W, MOON_CTR_VIDEO_TEX_H),
        GX_TRANSFER_FLIP_VERT(1) | GX_TRANSFER_OUT_TILED(1) |
            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
            GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565));

    // Pop-out mode leaves black borders around each eye's image, so clear the
    // render buffer first (the draw below only touches the image area)
    if (stereo == StereoRender::POPOUT) {
        u32 fb_bytes = render_width * surface_height * px_size;
        GX_MemoryFill((u32 *)vramFb, 0, (u32 *)((u8 *)vramFb + fb_bytes),
                      GX_FILL_TRIGGER | GX_FILL_16BIT_DEPTH, NULL, 0, NULL, 0);
    }

    // While the transfer is running, create a temporary command list to rotate
    // the framebuffer into source
    GPUCMD_SetBuffer(cmdlist, CMDLIST_SZ, 0);

    // TODO: Verify this mitigates rounding errors due to f24 precision issues.

#define C GPUCMD_AddWrite

    C(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
    C(GPUREG_COLORBUFFER_LOC, osConvertVirtToPhys(vramFb) >> 3);
    C(GPUREG_DEPTHBUFFER_LOC, 0);
    C(GPUREG_RENDERBUF_DIM,
      (1 << 24) | ((render_width - 1) << 12) | surface_height);
    C(GPUREG_FRAMEBUFFER_DIM,
      (1 << 24) | ((render_width - 1) << 12) | surface_height);
    C(GPUREG_FRAMEBUFFER_BLOCK32, 0);

    C(GPUREG_DEPTH_COLOR_MASK, 0xF << 8); // Write RGBA, no depth
    C(GPUREG_EARLYDEPTH_TEST1, 0);
    C(GPUREG_EARLYDEPTH_TEST2, 0);
    C(GPUREG_COLORBUFFER_FORMAT, GPU_RGB565 << 16);
    C(GPUREG_COLORBUFFER_READ,
      0x0); // Buffer is uninitialized and should not be read.
    C(GPUREG_COLORBUFFER_WRITE, 0xF);
    C(GPUREG_DEPTHBUFFER_READ, 0); // No depth buffer
    C(GPUREG_DEPTHBUFFER_WRITE, 0);

    C(GPUREG_VIEWPORT_XY, 0);

    C(GPUREG_VIEWPORT_WIDTH, f32tof24(surface_height / 2));
    C(GPUREG_VIEWPORT_INVW, f32tof31(2.0 / ((double)surface_height)) << 1);
    C(GPUREG_VIEWPORT_HEIGHT, f32tof24(render_width / 2));
    C(GPUREG_VIEWPORT_INVH, f32tof31(2.0 / ((double)render_width)) << 1);

    C(GPUREG_SCISSORTEST_MODE, 0);
    C(GPUREG_SCISSORTEST_POS, 0);
    C(GPUREG_SCISSORTEST_DIM, 0);

    C(GPUREG_DEPTHMAP_ENABLE, 1);
    C(GPUREG_DEPTHMAP_SCALE, f32tof24(-1.0));
    C(GPUREG_DEPTHMAP_OFFSET, 0);
    C(GPUREG_STENCIL_TEST, 0);
    C(GPUREG_FRAGOP_ALPHA_TEST, 0);
    C(GPUREG_LOGIC_OP, 3);
    C(GPUREG_COLOR_OPERATION, 0x00E40000);

    // Texturing
    C(GPUREG_TEXUNIT0_TYPE, GPU_RGB565);
    C(GPUREG_TEXUNIT0_DIM, MOON_CTR_VIDEO_TEX_H | (MOON_CTR_VIDEO_TEX_W << 16));
    C(GPUREG_TEXUNIT0_ADDR1, osConvertVirtToPhys(vramTex) >> 3);
    // Pop-out shrinks the image ~2x horizontally, so also filter when
    // minifying there to avoid shimmering
    C(GPUREG_TEXUNIT0_PARAM,
      GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) |
          GPU_TEXTURE_MIN_FILTER(stereo == StereoRender::POPOUT ? GPU_LINEAR
                                                                : GPU_NEAREST));

    // Shading
    // GPUCMD_AddMaskedWrite(GPUREG_SH_OUTATTR_CLOCK, 0x2, 1 << 8); // No Z, Yes
    // texcoord0
    C(GPUREG_TEXUNIT_CONFIG,
      1 | (1 << 12) | (1 << 16)); // Activate texture 0, clear texture cache

    C(GPUREG_TEXENV0_SOURCE, 0x003003); // Texture 0
    C(GPUREG_TEXENV0_OPERAND, 0);       // Source Color
    C(GPUREG_TEXENV0_COMBINER, 0);      // Replace
    C(GPUREG_TEXENV0_SCALE, 0);         // No Scale

    C(GPUREG_TEXENV1_SOURCE, 0x003003); // Texture 0
    C(GPUREG_TEXENV1_OPERAND, 0);       // Source Color
    C(GPUREG_TEXENV1_COMBINER, 0);      // Replace
    C(GPUREG_TEXENV1_SCALE, 0);         // No Scale

    C(GPUREG_TEXENV2_SOURCE, 0x003003); // Texture 0
    C(GPUREG_TEXENV2_OPERAND, 0);       // Source Color
    C(GPUREG_TEXENV2_COMBINER, 0);      // Replace
    C(GPUREG_TEXENV2_SCALE, 0);         // No Scale

    C(GPUREG_TEXENV3_SOURCE, 0x003003); // Texture 0
    C(GPUREG_TEXENV3_OPERAND, 0);       // Source Color
    C(GPUREG_TEXENV3_COMBINER, 0);      // Replace
    C(GPUREG_TEXENV3_SCALE, 0);         // No Scale

    C(GPUREG_TEXENV4_SOURCE, 0x003003); // Texture 0
    C(GPUREG_TEXENV4_OPERAND, 0);       // Source Color
    C(GPUREG_TEXENV4_COMBINER, 0);      // Replace
    C(GPUREG_TEXENV4_SCALE, 0);         // No Scale

    C(GPUREG_TEXENV5_SOURCE, 0x003003); // Texture 0
    C(GPUREG_TEXENV5_OPERAND, 0);       // Source Color
    C(GPUREG_TEXENV5_COMBINER, 0);      // Replace
    C(GPUREG_TEXENV5_SCALE, 0);         // No Scale

    // Attribute buffers
    C(GPUREG_ATTRIBBUFFERS_LOC, 0);
    C(GPUREG_ATTRIBBUFFERS_FORMAT_LOW, 0);
    C(GPUREG_ATTRIBBUFFERS_FORMAT_HIGH,
      (0xFFF << 16) | (1 << 28)); // Two fixed vertex attributes

    // Vertex Shader
    static DVLB_s *vshader_dvlb = NULL;
    static shaderProgram_s program;

    if (!vshader_dvlb) {
        vshader_dvlb = DVLB_ParseFile((u32 *)vshader_shbin, vshader_shbin_size);
        shaderProgramInit(&program);
        shaderProgramSetVsh(&program, &vshader_dvlb->DVLE[0]);
    }

    shaderProgramUse(&program);

    C(GPUREG_VSH_NUM_ATTR, 1); // 2 attributes
    GPUCMD_AddMaskedWrite(GPUREG_VSH_INPUTBUFFER_CONFIG, 0xB,
                          1 | (0xA0 << 24)); // 2 attributes, no geometry shader
    C(GPUREG_VSH_ATTRIBUTES_PERMUTATION_LOW, 0x00000010);
    C(GPUREG_VSH_ATTRIBUTES_PERMUTATION_HIGH, 0);

    // Geometry Pipeline
    C(GPUREG_FACECULLING_CONFIG, 0);
    C(GPUREG_GEOSTAGE_CONFIG, 0);
    GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 2,
                          (1 << 8) |
                              1); // 2 outmap registers, drawing triangle strip
    C(GPUREG_INDEXBUFFER_CONFIG, 0x80000000);
    C(GPUREG_RESTART_PRIMITIVE, 1);

    // Vertex Data
    GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 1, 1);
    GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 0);
    C(GPUREG_FIXEDATTRIB_INDEX, 0xF);

    union {
        u32 packed[3];
        struct {
            u8 x[3], y[3], z[3], w[3];
        };
    } param;

#define ATTR(X, Y, Z, W)                                                       \
    {                                                                          \
        write24(param.x, f32tof24(X));                                         \
        write24(param.y, f32tof24(Y));                                         \
        write24(param.z, f32tof24(Z));                                         \
        write24(param.w, f32tof24(W));                                         \
                                                                               \
        u32 p = param.packed[0];                                               \
        param.packed[0] = param.packed[2];                                     \
        param.packed[2] = p;                                                   \
        GPUCMD_AddIncrementalWrites(GPUREG_FIXEDATTRIB_DATA0, param.packed,    \
                                    3);                                        \
    }

    float sw = image_width / ((float)MOON_CTR_VIDEO_TEX_W);
    float sh = image_height / ((float)MOON_CTR_VIDEO_TEX_H);

    // float hw = 2.0f / surface_height;
    float hh = 2.0f / surface_width;

    // The buffer is rotated: clip X runs along the 240px screen height and
    // clip Y along the screen width, with Y = +1 at the left edge of the
    // screen. In 3D, Y in [0, 1] is the left eye and Y in [-1, 0] the right.
    //
    // Draws the whole image as a quad: image left edge at clip Y = y_left,
    // right edge at y_right, vertically spanning clip X in [-k, k].
#define IMAGE_QUAD(y_left, y_right, k)                                         \
    {                                                                          \
        ATTR((k), (y_right), 0.0, 0.0); /* TR */                               \
        ATTR(sw, -hh, 0.0, 0.0);                                               \
        ATTR(-(k), (y_right), 0.0, 0.0); /* TL */                              \
        ATTR(sw, sh, 0.0, 0.0);                                                \
        ATTR((k), (y_left), 0.0, 0.0); /* BR */                                \
        ATTR(0.0, -hh, 0.0, 0.0);                                              \
        ATTR(-(k), (y_left), 0.0, 0.0); /* BL */                               \
        ATTR(0.0, sh, 0.0, 0.0);                                               \
    }

    if (stereo == StereoRender::POPOUT) {
        // Each eye gets the full image, shrunk to leave room for the offset
        // so nothing is ever cropped. The left eye's copy is moved right and
        // the right eye's copy moved left (crossed disparity), which makes
        // the flat image appear in front of the screen.
        const float eye_w = GSP_SCREEN_HEIGHT_TOP; // 400px per eye
        const float max_d = MOON_CTR_POPOUT_MAX_PARALLAX;
        float d = depth * max_d;
        if (d < 0.0f)
            d = 0.0f;
        if (d > max_d)
            d = max_d;
        float img_w = eye_w - max_d;
        float k = img_w / eye_w;

        // Left edge of the image within each eye, in screen pixels
        float left_x = (max_d + d) / 2.0f;
        float right_x = (max_d - d) / 2.0f;

        // Left eye: screen x -> clip Y = 1 - x / 400
        IMAGE_QUAD(1.0f - left_x / eye_w, 1.0f - (left_x + img_w) / eye_w,
                   k);
        C(GPUREG_RESTART_PRIMITIVE, 1);
        // Right eye: screen x -> clip Y = -x / 400
        IMAGE_QUAD(-right_x / eye_w, -(right_x + img_w) / eye_w, k);
    } else {
        // Full screen. In side-by-side mode this naturally puts the left
        // half of the image in the left eye and the right half in the right.
        IMAGE_QUAD(1.0f, -1.0f, 1.0f);
    }

#undef IMAGE_QUAD

    // End Geometry Pipeline
    GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 1);
    GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 1, 0);
    C(GPUREG_VTX_FUNC, 1);

    // Stop Command List
    GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 0x8, 0x00000000);
    C(GPUREG_FRAMEBUFFER_FLUSH, 1);
    C(GPUREG_FRAMEBUFFER_INVALIDATE, 1);

#undef C

    gspWaitForEvent(GSPGPU_EVENT_PPF, 0);
    if (stereo == StereoRender::POPOUT) {
        gspWaitForEvent(GSPGPU_EVENT_PSC0, 0);
    }

    u32 *unused;
    u32 cmdlist_len;
    GPUCMD_Split(&unused, &cmdlist_len);
    GSPGPU_FlushDataCache(cmdlist, cmdlist_len);

    extern u32 __ctru_linear_heap;
    extern u32 __ctru_linear_heap_size;
    GX_FlushCacheRegions(cmdlist, cmdlist_len * 4, (u32 *)__ctru_linear_heap,
                         __ctru_linear_heap_size, NULL, 0);

    GX_ProcessCommandList(cmdlist, cmdlist_len * 4, 2);

    gspWaitForEvent(GSPGPU_EVENT_P3D, 0);

    // Copy into framebuffer, untiled
    if (is_3d && gfxIs3D()) {
        // Left
        u32 *dest_left =
            (u32 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
        auto surface_width_3d = GSP_SCREEN_HEIGHT_TOP;
        GX_DisplayTransfer(
            (u32 *)vramFb, GX_BUFFER_DIM(surface_height, surface_width_3d),
            dest_left, GX_BUFFER_DIM(surface_height, surface_width_3d),
            GX_TRANSFER_OUT_TILED(0) |
                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
                GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));

        // Right
        u32 *dest_right =
            (u32 *)gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL);
        auto surface_offset_3d =
            surface_height * surface_width_3d * px_size / sizeof(u32);
        GX_DisplayTransfer((u32 *)vramFb + surface_offset_3d,
                           GX_BUFFER_DIM(surface_height, surface_width_3d),
                           dest_right,
                           GX_BUFFER_DIM(surface_height, surface_width_3d),
                           GX_TRANSFER_OUT_TILED(0) |
                               GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
                               GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
                               GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    } else {
        u32 *dest = (u32 *)gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL);
        GX_DisplayTransfer((u32 *)vramFb,
                           GX_BUFFER_DIM(surface_height, surface_width), dest,
                           GX_BUFFER_DIM(surface_height, surface_width),
                           GX_TRANSFER_OUT_TILED(0) |
                               GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
                               GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
                               GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    }
    gspWaitForEvent(GSPGPU_EVENT_PPF, 0);

    perf_fbcopy_ticks = svcGetSystemTick() - start_ticks;
    if (debug) {
        draw_perf_counters();
    }

    gfxScreenSwapBuffers(screen, true);
}
