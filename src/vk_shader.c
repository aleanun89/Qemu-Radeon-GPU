#include "vk_shader.h"
#include "rlg_spirv.h"
#include <stdlib.h>
#include <string.h>

/*
 * US program -> SPIR-V. Every construct mirrors the software interpreter in
 * r3d_fs.c (the verified reference): scalar code per component, temporaries
 * as Function variables, constants from a UBO, TEX through combined samplers
 * (implicit LOD = hardware quad derivatives), KIL/alpha test via demote.
 */

void vk_fs_analyze(const R3DFragProg *p, VKFSInfo *info)
{
    memset(info, 0, sizeof(*info));
    for (unsigned n = 0; n < p->nnodes; ++n) {
        for (unsigned k = 0; k < p->node[n].tex_count; ++k) {
            const TEXInst *t = &p->tex[(p->node[n].tex_start + k) % R3D_US_TEX_MAX];
            if (t->op == TEX_KIL) {
                info->uses_kill = true;
            } else if (t->op == TEX_LD || t->op == TEX_TXP || t->op == TEX_TXB) {
                info->tex_units |= (uint16_t)(1u << t->unit);
            }
        }
        for (unsigned k = 0; k < p->node[n].alu_count; ++k) {
            const ALUInst *a = &p->alu[(p->node[n].alu_start + k) % R3D_US_ALU_MAX];
            if (a->rgb_omask) {
                info->targets |= (uint8_t)(1u << a->rgb_target);
            }
            if (a->a_out) {
                info->targets |= (uint8_t)(1u << a->a_target);
            }
            if (a->a_depth) {
                info->depth_write = true;
            }
        }
    }
}

typedef struct {
    SpvB b;
    const R3DFragProg *p;
    uint32_t tf, tv2, tv3, tv4, tbool;
    uint32_t temp[R3D_MAX_TEMPS][4];     /* Function variables, created on demand */
    uint32_t out[4][4];
    uint32_t depth;
    uint32_t ubo, samplers[16], simg_type;
    uint32_t zero, one, half;
} FS;

static uint32_t temp_var(FS *s, unsigned t, unsigned c)
{
    t &= 63u;
    if (!s->temp[t][c]) {
        s->temp[t][c] = spv_var_init(&s->b, s->tf, s->zero);
    }
    return s->temp[t][c];
}

static uint32_t load_src(FS *s, uint8_t src, unsigned c)
{
    if (src & R3D_SRC_CONST) {
        uint32_t idx[3] = { spv_c_uint(&s->b, 0), spv_c_uint(&s->b, src & 31u), spv_c_uint(&s->b, c) };
        uint32_t id = spv_id(&s->b);
        uint32_t o[6] = { spv_t_ptr(&s->b, SpvStorageClassUniform, s->tf), id, s->ubo, idx[0], idx[1], idx[2] };
        spv_emit(&s->b, &s->b.code, SpvOpAccessChain, o, 6);
        return spv_load(&s->b, s->tf, id);
    }
    return spv_load(&s->b, s->tf, temp_var(s, src, c));
}

static uint32_t fneg(FS *s, uint32_t v) { return spv_op1(&s->b, SpvOpFNegate, s->tf, v); }
static uint32_t fadd(FS *s, uint32_t a, uint32_t c) { return spv_op2(&s->b, SpvOpFAdd, s->tf, a, c); }
static uint32_t fsub(FS *s, uint32_t a, uint32_t c) { return spv_op2(&s->b, SpvOpFSub, s->tf, a, c); }
static uint32_t fmul(FS *s, uint32_t a, uint32_t c) { return spv_op2(&s->b, SpvOpFMul, s->tf, a, c); }
static uint32_t fmad(FS *s, uint32_t a, uint32_t c, uint32_t e) { return fadd(s, fmul(s, a, c), e); }
static uint32_t sel(FS *s, uint16_t cmp, uint32_t x, uint32_t k, uint32_t a, uint32_t c)
{
    uint32_t cond = spv_op2(&s->b, cmp, s->tbool, x, k);
    return spv_op3(&s->b, SpvOpSelect, s->tf, cond, a, c);
}

static void presub(FS *s, unsigned mode, const uint32_t s0[4], const uint32_t s1[4], uint32_t o[4])
{
    for (int c = 0; c < 4; ++c) {
        switch (mode) {
        case 0: o[c] = fsub(s, s->one, fmul(s, spv_c_float(&s->b, 2.0f), s0[c])); break;
        case 1: o[c] = fsub(s, s1[c], s0[c]); break;
        case 2: o[c] = fadd(s, s1[c], s0[c]); break;
        default: o[c] = fsub(s, s->one, s0[c]); break;
        }
    }
}

static uint32_t modify(FS *s, uint32_t v, uint8_t arg)
{
    if (arg & 0x40u) {
        v = spv_glsl1(&s->b, GLSLstd450FAbs, v);
    }
    return (arg & 0x20u) ? fneg(s, v) : v;
}

/* Same selection table as rgb_arg() in r3d_fs.c. */
static void rgb_arg(FS *s, uint8_t arg, uint32_t rs[3][4], uint32_t as[3][4], const uint32_t sp[4],
                    uint32_t o[3])
{
    unsigned sel = arg & 31u, n;
    switch (sel) {
    case 0: case 4: case 8: n = sel / 4; o[0] = rs[n][0]; o[1] = rs[n][1]; o[2] = rs[n][2]; break;
    case 1: case 5: case 9: n = sel / 4; o[0] = o[1] = o[2] = rs[n][0]; break;
    case 2: case 6: case 10: n = sel / 4; o[0] = o[1] = o[2] = rs[n][1]; break;
    case 3: case 7: case 11: n = sel / 4; o[0] = o[1] = o[2] = rs[n][2]; break;
    case 12: case 13: case 14: o[0] = o[1] = o[2] = as[sel - 12][3]; break;
    case 15: o[0] = sp[0]; o[1] = sp[1]; o[2] = sp[2]; break;
    case 16: case 17: case 18: case 19: o[0] = o[1] = o[2] = sp[sel - 16]; break;
    case 20: o[0] = o[1] = o[2] = s->zero; break;
    case 21: o[0] = o[1] = o[2] = s->one; break;
    case 22: o[0] = o[1] = o[2] = s->half; break;
    case 23: case 24: case 25: n = sel - 23; o[0] = rs[n][1]; o[1] = rs[n][2]; o[2] = rs[n][0]; break;
    case 26: case 27: case 28: n = sel - 26; o[0] = rs[n][2]; o[1] = rs[n][0]; o[2] = rs[n][1]; break;
    default: n = sel - 29; o[0] = as[n][3]; o[1] = rs[n][2]; o[2] = rs[n][1]; break;
    }
    for (int c = 0; c < 3; ++c) {
        o[c] = modify(s, o[c], arg);
    }
}

static uint32_t a_arg(FS *s, uint8_t arg, uint32_t rs[3][4], uint32_t as[3][4], const uint32_t sp[4])
{
    unsigned sel = arg & 31u;
    uint32_t v;
    if (sel < 9) v = rs[sel / 3][sel % 3];
    else if (sel < 12) v = as[sel - 9][3];
    else if (sel < 16) v = sp[sel - 12];
    else if (sel == 16) v = s->zero;
    else if (sel == 17) v = s->one;
    else v = s->half;
    return modify(s, v, arg);
}

static uint32_t omod(FS *s, uint32_t v, unsigned m, bool clamp)
{
    static const float k[8] = { 1.0f, 2.0f, 4.0f, 8.0f, 0.5f, 0.25f, 0.125f, 1.0f };
    if (k[m & 7u] != 1.0f) {
        v = fmul(s, v, spv_c_float(&s->b, k[m & 7u]));
    }
    return clamp ? spv_glsl3(&s->b, GLSLstd450FClamp, v, s->zero, s->one) : v;
}

static void emit_alu(FS *s, const ALUInst *a)
{
    uint32_t rs[3][4], as[3][4], spr[4], spa[4], ra[3][3], aa[3], rr[3], ar;

    for (int j = 0; j < 3; ++j) {
        for (unsigned c = 0; c < 4; ++c) {
            rs[j][c] = load_src(s, a->rgb_src[j], c);
            as[j][c] = load_src(s, a->a_src[j], c);
        }
    }
    presub(s, a->rgb_presub, rs[0], rs[1], spr);
    presub(s, a->a_presub, as[0], as[1], spa);
    for (int j = 0; j < 3; ++j) {
        rgb_arg(s, a->rgb_arg[j], rs, as, spr, ra[j]);
        aa[j] = a_arg(s, a->a_arg[j], rs, as, spa);
    }
    uint32_t dot3 = fadd(s, fadd(s, fmul(s, ra[0][0], ra[1][0]), fmul(s, ra[0][1], ra[1][1])),
                         fmul(s, ra[0][2], ra[1][2]));
    uint32_t dot4 = fmad(s, aa[0], aa[1], dot3);

    switch (a->a_op) {
    case A_DP4: ar = a->rgb_op == RGB_DP3 ? dot3 : dot4; break;
    case A_MIN: ar = spv_glsl2(&s->b, GLSLstd450FMin, aa[0], aa[1]); break;
    case A_MAX: ar = spv_glsl2(&s->b, GLSLstd450FMax, aa[0], aa[1]); break;
    case A_CND: ar = sel(s, SpvOpFOrdGreaterThan, aa[2], s->half, aa[0], aa[1]); break;
    case A_CMP: ar = sel(s, SpvOpFOrdGreaterThanEqual, aa[2], s->zero, aa[0], aa[1]); break;
    case A_FRC: ar = spv_glsl1(&s->b, GLSLstd450Fract, aa[0]); break;
    case A_EX2: ar = spv_glsl1(&s->b, GLSLstd450Exp2, aa[0]); break;
    case A_LG2: ar = spv_glsl1(&s->b, GLSLstd450Log2, aa[0]); break;
    case A_RCP: ar = spv_op2(&s->b, SpvOpFDiv, s->tf, s->one, aa[0]); break;
    case A_RSQ: ar = spv_glsl1(&s->b, GLSLstd450InverseSqrt, spv_glsl1(&s->b, GLSLstd450FAbs, aa[0])); break;
    default: ar = fmad(s, aa[0], aa[1], aa[2]); break;
    }
    ar = omod(s, ar, a->a_omod, a->a_clamp);

    for (int c = 0; c < 3; ++c) {
        switch (a->rgb_op) {
        case RGB_DP3: rr[c] = dot3; break;
        case RGB_DP4: rr[c] = dot4; break;
        case RGB_D2A: rr[c] = fadd(s, fadd(s, fmul(s, ra[0][0], ra[1][0]), fmul(s, ra[0][1], ra[1][1])), ra[2][0]); break;
        case RGB_MIN: rr[c] = spv_glsl2(&s->b, GLSLstd450FMin, ra[0][c], ra[1][c]); break;
        case RGB_MAX: rr[c] = spv_glsl2(&s->b, GLSLstd450FMax, ra[0][c], ra[1][c]); break;
        case RGB_CND: rr[c] = sel(s, SpvOpFOrdGreaterThan, ra[2][c], s->half, ra[0][c], ra[1][c]); break;
        case RGB_CMP: rr[c] = sel(s, SpvOpFOrdGreaterThanEqual, ra[2][c], s->zero, ra[0][c], ra[1][c]); break;
        case RGB_FRC: rr[c] = spv_glsl1(&s->b, GLSLstd450Fract, ra[0][c]); break;
        case RGB_REPL_ALPHA: rr[c] = ar; break;
        default: rr[c] = fmad(s, ra[0][c], ra[1][c], ra[2][c]); break;
        }
        if (a->rgb_op != RGB_REPL_ALPHA) {
            rr[c] = omod(s, rr[c], a->rgb_omod, a->rgb_clamp);
        }
    }
    for (int c = 0; c < 3; ++c) {
        if (a->rgb_wmask & (1u << c)) {
            spv_store(&s->b, temp_var(s, a->rgb_dst, (unsigned)c), rr[c]);
        }
        if (a->rgb_omask & (1u << c)) {
            spv_store(&s->b, s->out[a->rgb_target][c], rr[c]);
        }
    }
    if (a->a_wreg) {
        spv_store(&s->b, temp_var(s, a->a_dst, 3), ar);
    }
    if (a->a_out) {
        spv_store(&s->b, s->out[a->a_target][3], ar);
    }
    if (a->a_depth) {
        spv_store(&s->b, s->depth, ar);
    }
}

/* if (cond) demote; */
static void demote_if(FS *s, uint32_t cond)
{
    uint32_t kill = spv_id(&s->b), merge = spv_id(&s->b);
    const uint32_t sm[2] = { merge, 0 }, bc[3] = { cond, kill, merge };
    spv_emit(&s->b, &s->b.code, SpvOpSelectionMerge, sm, 2);
    spv_emit(&s->b, &s->b.code, SpvOpBranchConditional, bc, 3);
    spv_label(&s->b, kill);
    spv_emit(&s->b, &s->b.code, SpvOpDemoteToHelperInvocation, NULL, 0);
    spv_emit(&s->b, &s->b.code, SpvOpBranch, &merge, 1);
    spv_label(&s->b, merge);
}

static uint32_t construct(FS *s, uint32_t type, const uint32_t *c, unsigned n)
{
    uint32_t id = spv_id(&s->b), o[6] = { type, id };
    memcpy(o + 2, c, n * sizeof(uint32_t));
    spv_emit(&s->b, &s->b.code, SpvOpCompositeConstruct, o, 2 + n);
    return id;
}

static void emit_tex(FS *s, const TEXInst *t)
{
    uint32_t c[4], res, img;

    for (unsigned k = 0; k < 4; ++k) {
        c[k] = spv_load(&s->b, s->tf, temp_var(s, t->src, k));
    }
    if (t->op == TEX_KIL) {
        uint32_t any = 0;
        for (unsigned k = 0; k < 4; ++k) {
            uint32_t lt = spv_op2(&s->b, SpvOpFOrdLessThan, s->tbool, c[k], s->zero);
            any = any ? spv_op2(&s->b, SpvOpLogicalOr, s->tbool, any, lt) : lt;
        }
        demote_if(s, any);
        return;
    }
    if (t->op != TEX_LD && t->op != TEX_TXP && t->op != TEX_TXB) {
        return;
    }
    img = spv_load(&s->b, s->simg_type, s->samplers[t->unit & 15u]);
    if (t->op == TEX_TXP) {
        const uint32_t xyw[3] = { c[0], c[1], c[3] };
        uint32_t coord = construct(s, s->tv3, xyw, 3);
        res = spv_op2(&s->b, SpvOpImageSampleProjImplicitLod, s->tv4, img, coord);
    } else {
        uint32_t coord = construct(s, s->tv2, c, 2), id = spv_id(&s->b);
        if (t->op == TEX_TXB) {
            uint32_t o[7] = { s->tv4, id, img, coord, 0x1u /* Bias */, c[3] };
            spv_emit(&s->b, &s->b.code, SpvOpImageSampleImplicitLod, o, 6);
        } else {
            uint32_t o[4] = { s->tv4, id, img, coord };
            spv_emit(&s->b, &s->b.code, SpvOpImageSampleImplicitLod, o, 4);
        }
        res = id;
    }
    for (unsigned k = 0; k < 4; ++k) {
        spv_store(&s->b, temp_var(s, t->dst, k), spv_extract(&s->b, s->tf, res, k));
    }
}

static uint32_t compare(FS *s, unsigned func, uint32_t a, uint32_t b)
{
    static const uint16_t ops[8] = { 0, SpvOpFOrdLessThan, SpvOpFOrdLessThanEqual, SpvOpFOrdEqual,
                                     SpvOpFOrdGreaterThanEqual, SpvOpFOrdGreaterThan, SpvOpFOrdNotEqual, 0 };
    return spv_op2(&s->b, ops[func & 7u], s->tbool, a, b);
}

uint32_t *vk_fs_translate(const R3DFragProg *p, const VKFSKey *key, size_t *nwords)
{
    FS *s = calloc(1, sizeof(*s));
    uint32_t iface[64], niface = 0, fn, frag_coord = 0, frag_depth = 0, outs[4] = {0};
    uint32_t *words;

    if (!s) {
        return NULL;
    }
    s->p = p;
    spv_init(&s->b);
    SpvB *b = &s->b;
    const uint32_t cap_shader = SpvCapabilityShader, cap_demote = SpvCapabilityDemoteToHelperInvocation;
    spv_emit(b, &b->caps, SpvOpCapability, &cap_shader, 1);
    spv_emit(b, &b->caps, SpvOpCapability, &cap_demote, 1);
    s->tf = spv_t_float(b);
    s->tv2 = spv_t_vec(b, 2);
    s->tv3 = spv_t_vec(b, 3);
    s->tv4 = spv_t_vec(b, 4);
    s->tbool = spv_t_bool(b);
    s->zero = spv_c_float(b, 0.0f);
    s->one = spv_c_float(b, 1.0f);
    s->half = spv_c_float(b, 0.5f);

    /* UBO { vec4 c[32]; vec4 alpha_ref; } at set 0, binding 0 */
    {
        uint32_t arr_ops[2] = { s->tv4, spv_c_uint(b, 32) };
        uint32_t arr = spv_type(b, SpvOpTypeArray, arr_ops, 2);
        uint32_t st_ops[2] = { arr, s->tv4 };
        uint32_t st = spv_id(b), stw[3] = { st, arr, s->tv4 };
        uint32_t stride = 16, zero = 0, set = 0, bind = 0;
        (void)st_ops;
        spv_emit(b, &b->types, SpvOpTypeStruct, stw, 3);
        spv_decorate(b, arr, SpvDecorationArrayStride, &stride, 1);
        uint32_t m0[4] = { st, 0, SpvDecorationOffset, 0 }, m1[4] = { st, 1, SpvDecorationOffset, 512 };
        spv_emit(b, &b->deco, SpvOpMemberDecorate, m0, 4);
        spv_emit(b, &b->deco, SpvOpMemberDecorate, m1, 4);
        spv_decorate(b, st, SpvDecorationBlock, NULL, 0);
        s->ubo = spv_var(b, SpvStorageClassUniform, st);
        spv_decorate(b, s->ubo, SpvDecorationDescriptorSet, &set, 1);
        spv_decorate(b, s->ubo, SpvDecorationBinding, &bind, 1);
        iface[niface++] = s->ubo;
        (void)zero;
    }
    /* Combined image samplers: binding 1 + unit */
    {
        uint32_t img_ops[7] = { s->tf, SpvDim2D, 0, 0, 0, 1, 0 };
        uint32_t img = spv_type(b, SpvOpTypeImage, img_ops, 7);
        s->simg_type = spv_type(b, SpvOpTypeSampledImage, &img, 1);
        for (unsigned u = 0; u < 16; ++u) {
            if (key->tex_units & (1u << u)) {
                uint32_t set = 0, bind = 1 + u;
                s->samplers[u] = spv_var(b, SpvStorageClassUniformConstant, s->simg_type);
                spv_decorate(b, s->samplers[u], SpvDecorationDescriptorSet, &set, 1);
                spv_decorate(b, s->samplers[u], SpvDecorationBinding, &bind, 1);
                iface[niface++] = s->samplers[u];
            }
        }
    }
    /* Outputs */
    for (unsigned t = 0; t < 4; ++t) {
        if (key->out_mask & (1u << t)) {
            uint32_t loc = t;
            outs[t] = spv_var(b, SpvStorageClassOutput, s->tv4);
            spv_decorate(b, outs[t], SpvDecorationLocation, &loc, 1);
            iface[niface++] = outs[t];
        }
    }
    if (key->depth_write) {
        uint32_t bi = SpvBuiltInFragDepth, bc = SpvBuiltInFragCoord;
        frag_depth = spv_var(b, SpvStorageClassOutput, s->tf);
        spv_decorate(b, frag_depth, SpvDecorationBuiltIn, &bi, 1);
        frag_coord = spv_var(b, SpvStorageClassInput, s->tv4);
        spv_decorate(b, frag_coord, SpvDecorationBuiltIn, &bc, 1);
        iface[niface++] = frag_depth;
        iface[niface++] = frag_coord;
    }

    fn = spv_function(b);
    for (unsigned t = 0; t < 4; ++t) {
        for (unsigned c = 0; c < 4; ++c) {
            s->out[t][c] = spv_var_init(b, s->tf, s->zero);
        }
    }
    s->depth = spv_var_init(b, s->tf, s->zero);

    /* RS inputs -> temporaries (location order = increasing temp index). */
    {
        uint32_t loc = 0;
        for (unsigned t = 0; t < R3D_MAX_TEMPS; ++t) {
            if (!(key->inputs & (1ull << t))) {
                continue;
            }
            uint32_t in = spv_var(b, SpvStorageClassInput, s->tv4);
            spv_decorate(b, in, SpvDecorationLocation, &loc, 1);
            iface[niface++] = in;
            uint32_t v = spv_load(b, s->tv4, in);
            for (unsigned c = 0; c < 4; ++c) {
                spv_store(b, temp_var(s, t, c), spv_extract(b, s->tf, v, c));
            }
            loc++;
        }
    }
    if (key->depth_write) {
        uint32_t fc = spv_load(b, s->tv4, frag_coord);
        spv_store(b, s->depth, spv_extract(b, s->tf, fc, 2));
    }

    for (unsigned n = 0; n < p->nnodes; ++n) {
        for (unsigned k = 0; k < p->node[n].tex_count; ++k) {
            unsigned i = p->node[n].tex_start + k;
            if (i < R3D_US_TEX_MAX) {
                emit_tex(s, &p->tex[i]);
            }
        }
        for (unsigned k = 0; k < p->node[n].alu_count; ++k) {
            unsigned i = p->node[n].alu_start + k;
            if (i < R3D_US_ALU_MAX) {
                emit_alu(s, &p->alu[i]);
            }
        }
    }

    if (key->alpha_test) {
        unsigned f = key->alpha_func & 7u;
        if (f != 7) {
            uint32_t a = spv_glsl3(b, GLSLstd450FClamp, spv_load(b, s->tf, s->out[0][3]), s->zero, s->one);
            uint32_t idx[2] = { spv_c_uint(b, 1), spv_c_uint(b, 0) }, ptr = spv_id(b);
            uint32_t o[5] = { spv_t_ptr(b, SpvStorageClassUniform, s->tf), ptr, s->ubo, idx[0], idx[1] };
            spv_emit(b, &b->code, SpvOpAccessChain, o, 5);
            uint32_t ref = spv_load(b, s->tf, ptr);
            uint32_t fail = f == 0 ? spv_type(b, SpvOpConstantTrue, &s->tbool, 1)
                                   : spv_op1(b, SpvOpLogicalNot, s->tbool, compare(s, f, a, ref));
            demote_if(s, fail);
        }
    }
    for (unsigned t = 0; t < 4; ++t) {
        if (!(key->out_mask & (1u << t))) {
            continue;
        }
        uint32_t comp[4];
        for (unsigned j = 0; j < 4; ++j) {
            unsigned src = key->out_swz[t][j];
            comp[j] = src < 4 ? spv_load(b, s->tf, s->out[t][src]) : s->zero;
        }
        spv_store(b, outs[t], spv_vec4(b, comp));
    }
    if (key->depth_write) {
        spv_store(b, frag_depth, spv_load(b, s->tf, s->depth));
    }
    spv_emit(b, &b->code, SpvOpReturn, NULL, 0);

    /* Header: memory model, entry point (with the whole interface), modes. */
    {
        const uint32_t mm[2] = { 0 /* Logical */, 1 /* GLSL450 */ };
        const uint32_t pre[2] = { SpvExecutionModelFragment, fn };
        uint32_t m1[2] = { fn, SpvExecutionModeOriginUpperLeft }, m2[2] = { fn, SpvExecutionModeDepthReplacing };
        spv_emit(b, &b->header, SpvOpMemoryModel, mm, 2);
        spv_emit_str(b, &b->header, SpvOpEntryPoint, pre, 2, "main", iface, niface);
        spv_emit(b, &b->header, SpvOpExecutionMode, m1, 2);
        if (key->depth_write) {
            spv_emit(b, &b->header, SpvOpExecutionMode, m2, 2);
        }
    }
    words = spv_finish(b, nwords);
    spv_free(b);
    free(s);
    return words;
}

uint32_t *vk_vs_passthrough(unsigned nvary, size_t *nwords)
{
    SpvB bb, *b = &bb;
    uint32_t iface[40], niface = 0, fn, tv4, in[17], out[17], pos;
    uint32_t *words;

    if (nvary > 16) {
        return NULL;
    }
    spv_init(b);
    const uint32_t cap = SpvCapabilityShader;
    spv_emit(b, &b->caps, SpvOpCapability, &cap, 1);
    tv4 = spv_t_vec(b, 4);
    for (uint32_t i = 0; i <= nvary; ++i) {
        in[i] = spv_var(b, SpvStorageClassInput, tv4);
        spv_decorate(b, in[i], SpvDecorationLocation, &i, 1);
        iface[niface++] = in[i];
    }
    {
        uint32_t bi = SpvBuiltInPosition;
        pos = spv_var(b, SpvStorageClassOutput, tv4);
        spv_decorate(b, pos, SpvDecorationBuiltIn, &bi, 1);
        iface[niface++] = pos;
    }
    for (uint32_t i = 0; i < nvary; ++i) {
        out[i] = spv_var(b, SpvStorageClassOutput, tv4);
        spv_decorate(b, out[i], SpvDecorationLocation, &i, 1);
        iface[niface++] = out[i];
    }
    fn = spv_function(b);
    spv_store(b, pos, spv_load(b, tv4, in[0]));
    for (unsigned i = 0; i < nvary; ++i) {
        spv_store(b, out[i], spv_load(b, tv4, in[i + 1]));
    }
    spv_emit(b, &b->code, SpvOpReturn, NULL, 0);
    {
        const uint32_t mm[2] = { 0, 1 }, pre[2] = { SpvExecutionModelVertex, fn };
        spv_emit(b, &b->header, SpvOpMemoryModel, mm, 2);
        spv_emit_str(b, &b->header, SpvOpEntryPoint, pre, 2, "main", iface, niface);
    }
    words = spv_finish(b, nwords);
    spv_free(b);
    return words;
}
