#pragma once
/*
 * Minimal SPIR-V module builder (no external dependencies).
 * Opcode and enum values checked against Khronos SPIRV-Headers
 * (include/spirv/unified1/spirv.h and GLSL.std.450.h).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    SpvOpExtension = 10, SpvOpExtInstImport = 11, SpvOpExtInst = 12, SpvOpMemoryModel = 14,
    SpvOpEntryPoint = 15, SpvOpExecutionMode = 16, SpvOpCapability = 17,
    SpvOpTypeVoid = 19, SpvOpTypeBool = 20, SpvOpTypeInt = 21, SpvOpTypeFloat = 22,
    SpvOpTypeVector = 23, SpvOpTypeImage = 25, SpvOpTypeSampledImage = 27, SpvOpTypeArray = 28,
    SpvOpTypeStruct = 30, SpvOpTypePointer = 32, SpvOpTypeFunction = 33,
    SpvOpConstantTrue = 41, SpvOpConstantFalse = 42, SpvOpConstant = 43, SpvOpConstantComposite = 44,
    SpvOpFunction = 54, SpvOpFunctionEnd = 56, SpvOpVariable = 59, SpvOpLoad = 61, SpvOpStore = 62,
    SpvOpAccessChain = 65, SpvOpDecorate = 71, SpvOpMemberDecorate = 72,
    SpvOpCompositeConstruct = 80, SpvOpCompositeExtract = 81,
    SpvOpImageSampleImplicitLod = 87, SpvOpImageSampleProjImplicitLod = 91,
    SpvOpFNegate = 127, SpvOpFAdd = 129, SpvOpFSub = 131, SpvOpFMul = 133, SpvOpFDiv = 136,
    SpvOpLogicalOr = 166, SpvOpLogicalAnd = 167, SpvOpLogicalNot = 168, SpvOpSelect = 169,
    SpvOpFOrdEqual = 180, SpvOpFOrdNotEqual = 182, SpvOpFOrdLessThan = 184,
    SpvOpFOrdGreaterThan = 186, SpvOpFOrdLessThanEqual = 188, SpvOpFOrdGreaterThanEqual = 190,
    SpvOpSelectionMerge = 247, SpvOpLabel = 248, SpvOpBranch = 249, SpvOpBranchConditional = 250,
    SpvOpReturn = 253, SpvOpDemoteToHelperInvocation = 5380,
};
enum { SpvDecorationBlock = 2, SpvDecorationArrayStride = 6, SpvDecorationBuiltIn = 11,
       SpvDecorationLocation = 30, SpvDecorationBinding = 33, SpvDecorationDescriptorSet = 34,
       SpvDecorationOffset = 35 };
enum { SpvBuiltInPosition = 0, SpvBuiltInFragCoord = 15, SpvBuiltInFragDepth = 22 };
enum { SpvStorageClassUniformConstant = 0, SpvStorageClassInput = 1, SpvStorageClassUniform = 2,
       SpvStorageClassOutput = 3, SpvStorageClassFunction = 7 };
enum { SpvCapabilityShader = 1, SpvCapabilityDemoteToHelperInvocation = 5379 };
enum { SpvExecutionModelVertex = 0, SpvExecutionModelFragment = 4 };
enum { SpvExecutionModeOriginUpperLeft = 7, SpvExecutionModeDepthReplacing = 12 };
enum { SpvDim2D = 1, SpvDim3D = 2, SpvDimCube = 3 };
enum { GLSLstd450FAbs = 4, GLSLstd450Floor = 8, GLSLstd450Fract = 10, GLSLstd450Pow = 26,
       GLSLstd450Exp2 = 29, GLSLstd450Log2 = 30, GLSLstd450Sqrt = 31, GLSLstd450InverseSqrt = 32,
       GLSLstd450FMin = 37, GLSLstd450FMax = 40, GLSLstd450FClamp = 43 };

typedef struct {
    uint32_t *w;
    size_t n, cap;
} SpvBuf;

typedef struct {
    uint16_t op;
    uint8_t n;
    uint32_t ops[8];
    uint32_t id;
} SpvCacheEntry;

typedef struct {
    /* Module sections in SPIR-V logical layout order. */
    SpvBuf caps;                    /* capabilities, extensions, ext-inst imports */
    SpvBuf header;                  /* memory model, entry points, execution modes */
    SpvBuf deco, types;             /* annotations; types, constants, global variables */
    SpvBuf decls, code;             /* single function: Function variables, then body */
    uint32_t next_id;
    uint32_t glsl;                  /* GLSL.std.450 import id */
    uint32_t fn_id, fn_ret, fn_type, fn_label;
    SpvCacheEntry *cache;
    size_t ncache, capcache;
    bool oom;
} SpvB;

void spv_init(SpvB *b);
void spv_free(SpvB *b);
uint32_t spv_id(SpvB *b);
/* Append one instruction (opcode + operand words) to a section. */
void spv_emit(SpvB *b, SpvBuf *s, uint16_t op, const uint32_t *ops, unsigned n);
void spv_emit_str(SpvB *b, SpvBuf *s, uint16_t op, const uint32_t *pre, unsigned npre,
                  const char *str, const uint32_t *post, unsigned npost);
/* Deduplicated type/constant: `ops` excludes the result id. */
uint32_t spv_type(SpvB *b, uint16_t op, const uint32_t *ops, unsigned n);

uint32_t spv_t_void(SpvB *b);
uint32_t spv_t_bool(SpvB *b);
uint32_t spv_t_float(SpvB *b);
uint32_t spv_t_uint(SpvB *b);
uint32_t spv_t_vec(SpvB *b, unsigned n);
uint32_t spv_t_ptr(SpvB *b, uint32_t storage, uint32_t type);
uint32_t spv_c_float(SpvB *b, float f);
uint32_t spv_c_uint(SpvB *b, uint32_t u);

/* Function-body helpers (emit into `code`); return the result id. */
uint32_t spv_op1(SpvB *b, uint16_t op, uint32_t type, uint32_t a);
uint32_t spv_op2(SpvB *b, uint16_t op, uint32_t type, uint32_t a, uint32_t c);
uint32_t spv_op3(SpvB *b, uint16_t op, uint32_t type, uint32_t a, uint32_t c, uint32_t e);
uint32_t spv_glsl1(SpvB *b, uint32_t inst, uint32_t a);
uint32_t spv_glsl2(SpvB *b, uint32_t inst, uint32_t a, uint32_t c);
uint32_t spv_glsl3(SpvB *b, uint32_t inst, uint32_t a, uint32_t c, uint32_t e);
uint32_t spv_load(SpvB *b, uint32_t type, uint32_t ptr);
void spv_store(SpvB *b, uint32_t ptr, uint32_t val);
uint32_t spv_extract(SpvB *b, uint32_t type, uint32_t composite, uint32_t index);
uint32_t spv_vec4(SpvB *b, const uint32_t c[4]);
/* Global variable (in `decls` for Function storage, otherwise in `types`). */
uint32_t spv_var(SpvB *b, uint32_t storage, uint32_t pointee);
/* Function-storage variable with a constant initializer. */
uint32_t spv_var_init(SpvB *b, uint32_t pointee, uint32_t init);
void spv_decorate(SpvB *b, uint32_t id, uint32_t deco, const uint32_t *args, unsigned n);
void spv_label(SpvB *b, uint32_t id);

/* Declare the module's only function (void main()); its first label is implicit. */
uint32_t spv_function(SpvB *b);

/* Assemble the final module (SPIR-V 1.6). Caller frees the result. */
uint32_t *spv_finish(SpvB *b, size_t *nwords);
