/*
 * 3D throughput: full-screen textured quads at 640x480 (ARGB8888, 256x256
 * texture, bilinear), drawn through the CP exactly like the tests do.
 * Usage: bench-3d [software|vulkan] [draws]
 */
#include "radeon_r3d.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RING 0x00100000u
#define CBUF 0x00200000u
#define TEX  0x00400000u
#define W 640
#define H 480

static RLGDevice *dev;
static uint32_t wptr;

static void w(uint32_t a, uint32_t v) { rlg_mmio_write(dev, a, v, 4); }
static uint32_t fbits(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
static void ring(uint32_t v) { memcpy(dev->vram + RING + (wptr++ & 1023) * 4, &v, 4); }

static double now(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    bool vk = argc > 1 && !strcmp(argv[1], "vulkan");
    int draws = argc > 2 ? atoi(argv[2]) : 50;
    RLGConfig c = { .profile = RLG_PROFILE_X700PRO, .vram_size = 32u << 20,
                    .backend = vk ? RLG_BACKEND_VULKAN : RLG_BACKEND_SOFTWARE };
    dev = rlg_create(&c);
    if (vk && !rlg_host_is_active(dev)) {
        puts("Vulkan unavailable");
        return 77;
    }
    w(RLG_CP_RB_CNTL, 9 | RLG_RB_NO_UPDATE);
    w(RLG_CP_RB_BASE, RING);
    w(RLG_CP_CSQ_CNTL, 4u << RLG_CSQ_MODE_SHIFT);
    w(RLG_RB3D_COLOROFFSET0, CBUF);
    w(RLG_RB3D_COLORPITCH0, W | (6u << 21));
    w(RLG_US_OUT_FMT_0, (3u << 8) | (2u << 10) | (1u << 12));
    w(RLG_SC_CLIP_RULE, 0xffff);
    w(RLG_VAP_CNTL_STATUS, RLG_VAP_TCL_BYPASS);
    w(RLG_VAP_VTE_CNTL, RLG_VTX_XY_FMT | RLG_VTX_Z_FMT);
    w(RLG_VAP_OUTPUT_VTX_FMT_0, 1u);
    w(RLG_VAP_OUTPUT_VTX_FMT_1, 4u);
    w(RLG_VAP_PROG_STREAM_CNTL_0, 3u | ((3u | (6u << 8) | (1u << 13)) << 16));
    w(RLG_VAP_PROG_STREAM_CNTL_EXT_0, 0xf688u | (0xf688u << 16));
    w(RLG_VAP_VTX_SIZE, 8);
    w(RLG_RS_COUNT, 4u);
    w(RLG_RS_INST_COUNT, 0);
    w(RLG_RS_IP_0, (1u << 16) | (2u << 19) | (3u << 22));
    w(RLG_RS_INST_0, (1u << 3));
    /* FS: t1 = TEX(t0); out = t1 * t1 (one ALU MAD) */
    w(RLG_US_CONFIG, 1u << 3);
    w(RLG_US_CODE_ADDR_0 + 12, 0);
    w(RLG_US_TEX_INST_0, (1u << 6) | (1u << 15));
    w(RLG_US_ALU_RGB_ADDR_0, 1 | (1 << 6) | (1 << 12) | (7u << 26));
    w(RLG_US_ALU_ALPHA_ADDR_0, 1 | (1 << 6) | (1 << 12) | (1u << 24));
    w(RLG_US_ALU_RGB_INST_0, 0 | (4u << 7) | (20u << 14));
    w(RLG_US_ALU_ALPHA_INST_0, 9 | (10u << 7) | (16u << 14));
    for (int i = 0; i < 256 * 256; ++i) {
        uint32_t t = 0xff000000u | (uint32_t)(i * 2654435761u >> 8);
        memcpy(dev->vram + TEX + i * 4, &t, 4);
    }
    w(RLG_TX_ENABLE, 1);
    w(RLG_TX_FILTER0_0, (2u << 9) | (2u << 11));
    w(RLG_TX_FORMAT0_0, 255u | (255u << 11));
    w(RLG_TX_FORMAT1_0, 0xcu | (3u << 9) | (2u << 12) | (1u << 15));
    w(RLG_TX_OFFSET_0, TEX);
    const float v[] = { 0, 0, 0, 1, 0, 0, 0, 1,  W, 0, 0, 1, 1, 0, 0, 1,
                        W, H, 0, 1, 1, 1, 0, 1,  0, H, 0, 1, 0, 1, 0, 1 };

    double t0 = now();
    for (int n = 0; n < draws; ++n) {
        ring(RLG_PM4_PACKET3(RLG_PM4_3D_DRAW_IMMD_2, 33));
        ring((3u << 4) | (4u << 16) | 13u);
        for (int i = 0; i < 32; ++i) ring(fbits(v[i]));
        w(RLG_CP_RB_WPTR, wptr & 1023);
    }
    double dt = now() - t0;
    printf("%s: %d draws of %dx%d in %.3f s -> %.2f ms/draw, %.1f Mpixel/s (gpu %llu, sw %llu)\n",
           vk ? "vulkan" : "software", draws, W, H, dt, dt * 1000.0 / draws,
           (double)W * H * draws / dt / 1e6, (unsigned long long)dev->r3d->stats.vk_draws,
           (unsigned long long)dev->r3d->stats.sw_fallbacks);
    rlg_destroy(dev);
    return 0;
}
