#include "rlg_spirv.h"
#include <stdlib.h>
#include <string.h>

static void buf_push(SpvB *b, SpvBuf *s, uint32_t w)
{
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 256;
        uint32_t *nw = realloc(s->w, nc * sizeof(uint32_t));
        if (!nw) {
            b->oom = true;
            return;
        }
        s->w = nw;
        s->cap = nc;
    }
    s->w[s->n++] = w;
}

void spv_init(SpvB *b)
{
    memset(b, 0, sizeof(*b));
    b->next_id = 1;
    b->glsl = spv_id(b);        /* OpExtInstImport is written by spv_finish() */
}

void spv_free(SpvB *b)
{
    SpvBuf *all[] = { &b->caps, &b->header, &b->deco, &b->types, &b->decls, &b->code };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        free(all[i]->w);
    }
    free(b->cache);
    memset(b, 0, sizeof(*b));
}

uint32_t spv_id(SpvB *b) { return b->next_id++; }

void spv_emit(SpvB *b, SpvBuf *s, uint16_t op, const uint32_t *ops, unsigned n)
{
    buf_push(b, s, ((uint32_t)(n + 1) << 16) | op);
    for (unsigned i = 0; i < n; ++i) {
        buf_push(b, s, ops[i]);
    }
}

void spv_emit_str(SpvB *b, SpvBuf *s, uint16_t op, const uint32_t *pre, unsigned npre,
                  const char *str, const uint32_t *post, unsigned npost)
{
    size_t len = strlen(str), sw = len / 4 + 1;         /* nul-terminated, padded */
    buf_push(b, s, ((uint32_t)(1 + npre + sw + npost) << 16) | op);
    for (unsigned i = 0; i < npre; ++i) {
        buf_push(b, s, pre[i]);
    }
    for (size_t i = 0; i < sw; ++i) {
        uint32_t w = 0;
        for (size_t k = 0; k < 4; ++k) {
            size_t at = i * 4 + k;
            if (at < len) {
                w |= (uint32_t)(uint8_t)str[at] << (8 * k);
            }
        }
        buf_push(b, s, w);
    }
    for (unsigned i = 0; i < npost; ++i) {
        buf_push(b, s, post[i]);
    }
}

uint32_t spv_type(SpvB *b, uint16_t op, const uint32_t *ops, unsigned n)
{
    for (size_t i = 0; i < b->ncache; ++i) {
        SpvCacheEntry *e = &b->cache[i];
        if (e->op == op && e->n == n && !memcmp(e->ops, ops, n * sizeof(uint32_t))) {
            return e->id;
        }
    }
    uint32_t id = spv_id(b), words[10];
    words[0] = id;
    memcpy(words + 1, ops, n * sizeof(uint32_t));
    /* Constants and OpTypePointer-like instructions put the result id second. */
    if (op == SpvOpConstant || op == SpvOpConstantComposite || op == SpvOpConstantTrue ||
        op == SpvOpConstantFalse) {
        words[0] = ops[0];
        words[1] = id;
        memcpy(words + 2, ops + 1, (n - 1) * sizeof(uint32_t));
        spv_emit(b, &b->types, op, words, n + 1);
    } else {
        spv_emit(b, &b->types, op, words, n + 1);
    }
    if (n <= 8) {
        if (b->ncache == b->capcache) {
            size_t nc = b->capcache ? b->capcache * 2 : 64;
            SpvCacheEntry *ne = realloc(b->cache, nc * sizeof(*ne));
            if (!ne) {
                b->oom = true;
                return id;
            }
            b->cache = ne;
            b->capcache = nc;
        }
        SpvCacheEntry *e = &b->cache[b->ncache++];
        e->op = op;
        e->n = (uint8_t)n;
        memcpy(e->ops, ops, n * sizeof(uint32_t));
        e->id = id;
    }
    return id;
}

uint32_t spv_t_void(SpvB *b) { return spv_type(b, SpvOpTypeVoid, NULL, 0); }
uint32_t spv_t_bool(SpvB *b) { return spv_type(b, SpvOpTypeBool, NULL, 0); }
uint32_t spv_t_float(SpvB *b) { const uint32_t w = 32; return spv_type(b, SpvOpTypeFloat, &w, 1); }
uint32_t spv_t_uint(SpvB *b) { const uint32_t o[2] = { 32, 0 }; return spv_type(b, SpvOpTypeInt, o, 2); }
uint32_t spv_t_vec(SpvB *b, unsigned n) { const uint32_t o[2] = { spv_t_float(b), n }; return spv_type(b, SpvOpTypeVector, o, 2); }
uint32_t spv_t_ptr(SpvB *b, uint32_t sc, uint32_t t) { const uint32_t o[2] = { sc, t }; return spv_type(b, SpvOpTypePointer, o, 2); }

uint32_t spv_c_float(SpvB *b, float f)
{
    uint32_t o[2];
    o[0] = spv_t_float(b);
    memcpy(&o[1], &f, 4);
    return spv_type(b, SpvOpConstant, o, 2);
}

uint32_t spv_c_uint(SpvB *b, uint32_t u)
{
    const uint32_t o[2] = { spv_t_uint(b), u };
    return spv_type(b, SpvOpConstant, o, 2);
}

uint32_t spv_op1(SpvB *b, uint16_t op, uint32_t type, uint32_t a)
{
    uint32_t id = spv_id(b), o[3] = { type, id, a };
    spv_emit(b, &b->code, op, o, 3);
    return id;
}

uint32_t spv_op2(SpvB *b, uint16_t op, uint32_t type, uint32_t a, uint32_t c)
{
    uint32_t id = spv_id(b), o[4] = { type, id, a, c };
    spv_emit(b, &b->code, op, o, 4);
    return id;
}

uint32_t spv_op3(SpvB *b, uint16_t op, uint32_t type, uint32_t a, uint32_t c, uint32_t e)
{
    uint32_t id = spv_id(b), o[5] = { type, id, a, c, e };
    spv_emit(b, &b->code, op, o, 5);
    return id;
}

static uint32_t glsl(SpvB *b, uint32_t inst, const uint32_t *args, unsigned n)
{
    uint32_t id = spv_id(b), o[7] = { spv_t_float(b), id, b->glsl, inst };
    memcpy(o + 4, args, n * sizeof(uint32_t));
    spv_emit(b, &b->code, SpvOpExtInst, o, 4 + n);
    return id;
}

uint32_t spv_glsl1(SpvB *b, uint32_t inst, uint32_t a) { return glsl(b, inst, &a, 1); }
uint32_t spv_glsl2(SpvB *b, uint32_t inst, uint32_t a, uint32_t c) { const uint32_t v[2] = { a, c }; return glsl(b, inst, v, 2); }
uint32_t spv_glsl3(SpvB *b, uint32_t inst, uint32_t a, uint32_t c, uint32_t e) { const uint32_t v[3] = { a, c, e }; return glsl(b, inst, v, 3); }

uint32_t spv_load(SpvB *b, uint32_t type, uint32_t ptr) { return spv_op1(b, SpvOpLoad, type, ptr); }

void spv_store(SpvB *b, uint32_t ptr, uint32_t val)
{
    const uint32_t o[2] = { ptr, val };
    spv_emit(b, &b->code, SpvOpStore, o, 2);
}

uint32_t spv_extract(SpvB *b, uint32_t type, uint32_t comp, uint32_t index)
{
    return spv_op2(b, SpvOpCompositeExtract, type, comp, index);
}

uint32_t spv_vec4(SpvB *b, const uint32_t c[4])
{
    uint32_t id = spv_id(b), o[6] = { spv_t_vec(b, 4), id, c[0], c[1], c[2], c[3] };
    spv_emit(b, &b->code, SpvOpCompositeConstruct, o, 6);
    return id;
}

uint32_t spv_var(SpvB *b, uint32_t storage, uint32_t pointee)
{
    uint32_t id = spv_id(b), o[3] = { spv_t_ptr(b, storage, pointee), id, storage };
    spv_emit(b, storage == SpvStorageClassFunction ? &b->decls : &b->types, SpvOpVariable, o, 3);
    return id;
}

uint32_t spv_var_init(SpvB *b, uint32_t pointee, uint32_t init)
{
    uint32_t id = spv_id(b), o[4] = { spv_t_ptr(b, SpvStorageClassFunction, pointee), id,
                                      SpvStorageClassFunction, init };
    spv_emit(b, &b->decls, SpvOpVariable, o, 4);
    return id;
}

void spv_decorate(SpvB *b, uint32_t id, uint32_t deco, const uint32_t *args, unsigned n)
{
    uint32_t o[6] = { id, deco };
    memcpy(o + 2, args, n * sizeof(uint32_t));
    spv_emit(b, &b->deco, SpvOpDecorate, o, 2 + n);
}

void spv_label(SpvB *b, uint32_t id) { spv_emit(b, &b->code, SpvOpLabel, &id, 1); }

uint32_t spv_function(SpvB *b)
{
    b->fn_ret = spv_t_void(b);
    b->fn_type = spv_type(b, SpvOpTypeFunction, &b->fn_ret, 1);
    b->fn_id = spv_id(b);
    b->fn_label = spv_id(b);
    return b->fn_id;
}

uint32_t *spv_finish(SpvB *b, size_t *nwords)
{
    const SpvBuf *secs[] = { &b->caps, &b->header, &b->deco, &b->types };
    size_t total = 5 + 5 + 2 + b->decls.n + b->code.n + 1;
    uint32_t *out, *p;

    SpvBuf import = {0};

    /* Capabilities must precede the import, so it is emitted here. */
    spv_emit_str(b, &import, SpvOpExtInstImport, &b->glsl, 1, "GLSL.std.450", NULL, 0);
    total += import.n;
    for (size_t i = 0; i < 4; ++i) {
        total += secs[i]->n;
    }
    if (b->oom || !(out = malloc(total * sizeof(uint32_t)))) {
        free(import.w);
        return NULL;
    }
    p = out;
    *p++ = 0x07230203u;         /* magic */
    *p++ = 0x00010600u;         /* SPIR-V 1.6 (Vulkan 1.3) */
    *p++ = 0;                   /* generator */
    *p++ = b->next_id;          /* bound */
    *p++ = 0;
    for (size_t i = 0; i < 4; ++i) {
        memcpy(p, secs[i]->w, secs[i]->n * sizeof(uint32_t));
        p += secs[i]->n;
        if (i == 0) {
            memcpy(p, import.w, import.n * sizeof(uint32_t));
            p += import.n;
        }
    }
    free(import.w);
    *p++ = (5u << 16) | SpvOpFunction;
    *p++ = b->fn_ret;
    *p++ = b->fn_id;
    *p++ = 0;                   /* FunctionControl None */
    *p++ = b->fn_type;
    *p++ = (2u << 16) | SpvOpLabel;
    *p++ = b->fn_label;
    memcpy(p, b->decls.w, b->decls.n * sizeof(uint32_t));
    p += b->decls.n;
    memcpy(p, b->code.w, b->code.n * sizeof(uint32_t));
    p += b->code.n;
    *p++ = (1u << 16) | SpvOpFunctionEnd;
    *nwords = (size_t)(p - out);
    return out;
}
