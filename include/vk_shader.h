#pragma once
/* R300/R400 fragment program (US) -> SPIR-V, and the pass-through vertex shader. */
#include "radeon_r3d.h"

typedef struct {
    uint64_t inputs;            /* temps written by the RS, in location order (bit = temp) */
    uint16_t tex_units;         /* units sampled by TEX/TXP/TXB */
    uint8_t out_mask;           /* render targets written */
    uint8_t out_swz[4][4];      /* per target: Vulkan component j <- shader R(0)/G/B/A(3), 4 = 0 */
    bool alpha_test;
    uint8_t alpha_func;         /* FG_ALPHA_FUNC 10:8 */
    bool depth_write;
} VKFSKey;

typedef struct {
    uint16_t tex_units;
    uint8_t targets;
    bool depth_write;
    bool uses_kill;
} VKFSInfo;

/* Static analysis of the decoded program (what the translator will need). */
void vk_fs_analyze(const R3DFragProg *p, VKFSInfo *info);
/* Returns malloc'ed SPIR-V words or NULL. */
uint32_t *vk_fs_translate(const R3DFragProg *p, const VKFSKey *key, size_t *nwords);
/* Vertex shader: location 0 = gl_Position, locations 1..n -> outputs 0..n-1. */
uint32_t *vk_vs_passthrough(unsigned nvary, size_t *nwords);

/* UBO layout shared with the backend (std140-compatible). */
typedef struct {
    float c[32][4];             /* US constants (decoded float24) */
    float alpha_ref[4];         /* x = FG_ALPHA_FUNC reference in [0, 1] */
} VKFSUniforms;
