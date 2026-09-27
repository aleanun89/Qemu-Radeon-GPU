#include "radeon_r3d.h"
#include "vk_shader.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * Vulkan host backend for the R300/R400 3D engine.
 *
 * Split of work (like a SW-TCL driver): vertex fetch, PVS, clipping, culling,
 * point/line expansion and the RS mapping stay on the CPU (r3d_*.c, verified),
 * while rasterization, the fragment program (translated to SPIR-V), texturing,
 * depth/stencil and blending run on the host GPU.
 *
 * Phase 1 coherency model: every draw uploads the touched region of the colour
 * and depth buffers from emulated VRAM and writes it back afterwards. This is
 * always correct; a resident surface cache is the next optimisation.
 *
 * Vulkan is loaded at run time (vulkan-1.dll / libvulkan.so.1): only the
 * headers are needed to build. Any state this backend cannot reproduce exactly
 * makes r3d_vk_draw() return < 0 and the draw is rasterized in software.
 */

#if defined(RLG_HAVE_VULKAN)

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define VK_INSTANCE_FNS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFeatures) X(vkCreateDevice) X(vkGetDeviceProcAddr)

#define VK_DEVICE_FNS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkCreateFence) \
    X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) \
    X(vkMapMemory) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) \
    X(vkDestroySampler) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCmdPipelineBarrier) \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBeginRendering) \
    X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) \
    X(vkCmdBindVertexBuffers) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) \
    X(vkCmdSetBlendConstants)

#define DECL_FN(n) PFN_##n n;

typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    void *map;
    VkDeviceSize size;
} VkBuf;

typedef struct {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
} VkImg;

/* Everything that selects a VkPipeline. Zero-initialised, compared with memcmp. */
typedef struct {
    uint64_t fs_hash;
    uint32_t fs_words;
    uint32_t nvary;
    VkFormat color_fmt, depth_fmt;
    uint32_t write_mask;
    uint32_t blend, src_c, dst_c, op_c, src_a, dst_a, op_a;
    uint32_t depth_test, depth_write, depth_op;
    uint32_t stencil;
    VkStencilOpState front, back;
    VkFrontFace front_face;
} PipeKey;

typedef struct {
    PipeKey key;
    VkPipeline pipe;
} PipeEntry;

#define PIPE_CACHE_MAX 256

/*
 * Images and samplers are recycled between draws: creating and freeing them
 * (vkAllocateMemory included) cost more than the draw itself. Every draw waits
 * for its fence, so an image is free again as soon as the draw returns.
 */
typedef struct {
    VkFormat fmt;
    uint32_t w, h, levels, base;
    VkImageUsageFlags usage;
    VkImageAspectFlags aspect;
    VkComponentMapping swz;
} ImgKey;

typedef struct {
    ImgKey key;
    VkImg img;
    bool busy;
} ImgEntry;

#define IMG_CACHE_MAX 64

typedef struct {
    VkFilter mag, min;
    VkSamplerMipmapMode mip;
    VkSamplerAddressMode u, v;
    VkBorderColor border;
    float bias, max_lod;
} SampKey;

typedef struct {
    SampKey key;
    VkSampler samp;
} SampEntry;

#define SAMP_CACHE_MAX 64

/*
 * Draw batching. Draws are recorded into one command buffer and submitted
 * together: the colour/depth target stays resident on the GPU for the batch
 * and is written back to the emulated VRAM once, at vk_flush(). A flush is
 * forced whenever something else could observe or modify that VRAM: end of a
 * CP run, a non-3D register write from the CP, a software-rasterized draw, a
 * texture that overlaps the target, a target change, or a full batch.
 */
#define BATCH_MAX_DRAWS 256
#define UBO_SLOT 768                            /* >= sizeof(VKFSUniforms), 256-aligned */

typedef struct {
    bool open, bound, zbound, zdirty;
    unsigned draws;
    VkDeviceSize stg_at, vbo_at;
    /* resident target: rows [0, rows) of the surface */
    uint32_t cvoff, W, rows, cf, zvoff, zfmt;
    uint64_t crow;
    VkFormat cvk, dfmt;
    VkImageAspectFlags daspect;
    VkImg color, depth;
    VkDeviceSize c_stg, z_stg, s_stg;           /* upload area, reused for read-back */
    int y0, y1;                                 /* dirty rows */
    VkImg borrowed[IMG_CACHE_MAX];
    unsigned nborrowed;
} Batch;

struct RLGHost {
    void *lib;
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance inst;
    VkPhysicalDevice phys;
    VkPhysicalDeviceMemoryProperties memprops;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    bool depth_clamp;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkDescriptorPool dpool;
    VkBuf staging, ubo, vbo;
    PipeEntry pipes[PIPE_CACHE_MAX];
    unsigned npipes;
    ImgEntry imgs[IMG_CACHE_MAX];
    unsigned nimgs;
    SampEntry samps[SAMP_CACHE_MAX];
    unsigned nsamps;
    Batch b;
    RLGDevice *d;
    VK_INSTANCE_FNS(DECL_FN)
    VK_DEVICE_FNS(DECL_FN)
};

static int vk_flush(RLGDevice *d);

/* ---- loader / context ------------------------------------------------------------ */

static void *lib_open(void)
{
#ifdef _WIN32
    return (void *)LoadLibraryA("vulkan-1.dll");
#else
    void *h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    return h ? h : dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
#endif
}

static void *lib_sym(void *lib, const char *name)
{
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

static void lib_close(void *lib)
{
#ifdef _WIN32
    FreeLibrary((HMODULE)lib);
#else
    dlclose(lib);
#endif
}

static int find_mem(RLGHost *h, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < h->memprops.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (h->memprops.memoryTypes[i].propertyFlags & want) == want) {
            return (int)i;
        }
    }
    return -1;
}

static void buf_destroy(RLGHost *h, VkBuf *b)
{
    if (b->buf) h->vkDestroyBuffer(h->dev, b->buf, NULL);
    if (b->mem) h->vkFreeMemory(h->dev, b->mem, NULL);
    memset(b, 0, sizeof(*b));
}

/*
 * Host-visible, coherent, persistently mapped; grows on demand. Buffers the CPU
 * reads back (staging) ask for HOST_CACHED: the first coherent type is usually
 * uncached or BAR memory, where CPU reads run at a few hundred MB/s.
 */
static int buf_ensure(RLGHost *h, VkBuf *b, VkDeviceSize size, VkBufferUsageFlags usage, bool cpu_read)
{
    VkMemoryRequirements mr;
    int mt;

    if (b->buf && b->size >= size) {
        return 0;
    }
    buf_destroy(h, b);
    size = size < 65536 ? 65536 : size;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (h->vkCreateBuffer(h->dev, &bi, NULL, &b->buf) != VK_SUCCESS) {
        return -1;
    }
    h->vkGetBufferMemoryRequirements(h->dev, b->buf, &mr);
    mt = -1;
    if (cpu_read) {
        mt = find_mem(h, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    }
    if (mt < 0) {
        mt = find_mem(h, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = (uint32_t)mt };
    if (mt < 0 || h->vkAllocateMemory(h->dev, &ai, NULL, &b->mem) != VK_SUCCESS ||
        h->vkBindBufferMemory(h->dev, b->buf, b->mem, 0) != VK_SUCCESS ||
        h->vkMapMemory(h->dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) != VK_SUCCESS) {
        buf_destroy(h, b);
        return -1;
    }
    b->size = size;
    return 0;
}

static void img_destroy(RLGHost *h, VkImg *i)
{
    if (i->view) h->vkDestroyImageView(h->dev, i->view, NULL);
    if (i->img) h->vkDestroyImage(h->dev, i->img, NULL);
    if (i->mem) h->vkFreeMemory(h->dev, i->mem, NULL);
    memset(i, 0, sizeof(*i));
}

static int img_create(RLGHost *h, VkImg *i, VkFormat fmt, uint32_t w, uint32_t hh, uint32_t levels,
                      VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t base_level,
                      const VkComponentMapping *swz)
{
    VkMemoryRequirements mr;
    int mt;

    memset(i, 0, sizeof(*i));
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = fmt, .extent = { w, hh, 1 }, .mipLevels = levels,
                             .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (h->vkCreateImage(h->dev, &ci, NULL, &i->img) != VK_SUCCESS) {
        return -1;
    }
    h->vkGetImageMemoryRequirements(h->dev, i->img, &mr);
    mt = find_mem(h, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = (uint32_t)mt };
    if (mt < 0 || h->vkAllocateMemory(h->dev, &ai, NULL, &i->mem) != VK_SUCCESS ||
        h->vkBindImageMemory(h->dev, i->img, i->mem, 0) != VK_SUCCESS) {
        img_destroy(h, i);
        return -1;
    }
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = i->img,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                 .subresourceRange = { aspect, base_level, levels - base_level, 0, 1 } };
    if (swz) {
        vi.components = *swz;
    }
    if (h->vkCreateImageView(h->dev, &vi, NULL, &i->view) != VK_SUCCESS) {
        img_destroy(h, i);
        return -1;
    }
    return 0;
}

/* Borrow a cached image matching the description, creating or replacing one if needed. */
static int img_get(RLGHost *h, VkImg *out, VkFormat fmt, uint32_t w, uint32_t hh, uint32_t levels,
                   VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t base_level,
                   const VkComponentMapping *swz)
{
    ImgKey k;
    ImgEntry *e = NULL;

    memset(&k, 0, sizeof(k));
    k.fmt = fmt;
    k.w = w;
    k.h = hh;
    k.levels = levels;
    k.base = base_level;
    k.usage = usage;
    k.aspect = aspect;
    if (swz) {
        k.swz = *swz;
    }
    for (unsigned i = 0; i < h->nimgs; ++i) {
        if (!h->imgs[i].busy && !memcmp(&h->imgs[i].key, &k, sizeof(k))) {
            e = &h->imgs[i];
            goto found;
        }
    }
    if (h->nimgs < IMG_CACHE_MAX) {
        e = &h->imgs[h->nimgs++];
    } else {
        for (unsigned i = 0; i < h->nimgs && !e; ++i) {
            if (!h->imgs[i].busy) {
                e = &h->imgs[i];
            }
        }
        if (!e) {
            return -1;
        }
        img_destroy(h, &e->img);
    }
    if (img_create(h, &e->img, fmt, w, hh, levels, usage, aspect, base_level, swz) != 0) {
        /* leave a dead entry that never matches */
        memset(&e->key, 0xff, sizeof(e->key));
        return -1;
    }
    e->key = k;
found:
    e->busy = true;
    *out = e->img;
    return 0;
}

static void img_put(RLGHost *h, VkImg *i)
{
    for (unsigned n = 0; i->img && n < h->nimgs; ++n) {
        if (h->imgs[n].img.img == i->img) {
            h->imgs[n].busy = false;
            break;
        }
    }
    memset(i, 0, sizeof(*i));
}

static VkSampler samp_get(RLGHost *h, const VkSamplerCreateInfo *sci)
{
    SampKey k;
    VkSampler s;

    memset(&k, 0, sizeof(k));
    k.mag = sci->magFilter;
    k.min = sci->minFilter;
    k.mip = sci->mipmapMode;
    k.u = sci->addressModeU;
    k.v = sci->addressModeV;
    k.border = sci->borderColor;
    k.bias = sci->mipLodBias;
    k.max_lod = sci->maxLod;
    for (unsigned i = 0; i < h->nsamps; ++i) {
        if (!memcmp(&h->samps[i].key, &k, sizeof(k))) {
            return h->samps[i].samp;
        }
    }
    if (h->vkCreateSampler(h->dev, sci, NULL, &s) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    /* r3d_vk_draw keeps room for 16 units, so this never overflows */
    h->samps[h->nsamps].key = k;
    h->samps[h->nsamps++].samp = s;
    return s;
}

void rlg_host_destroy(RLGDevice *d)
{
    RLGHost *h;
    if (!d || !(h = d->host)) {
        return;
    }
    if (h->dev) {
        if (h->d) {
            vk_flush(d);
        }
        h->vkDeviceWaitIdle(h->dev);
        for (unsigned i = 0; i < h->npipes; ++i) {
            h->vkDestroyPipeline(h->dev, h->pipes[i].pipe, NULL);
        }
        for (unsigned i = 0; i < h->nimgs; ++i) {
            img_destroy(h, &h->imgs[i].img);
        }
        for (unsigned i = 0; i < h->nsamps; ++i) {
            h->vkDestroySampler(h->dev, h->samps[i].samp, NULL);
        }
        buf_destroy(h, &h->staging);
        buf_destroy(h, &h->ubo);
        buf_destroy(h, &h->vbo);
        if (h->dpool) h->vkDestroyDescriptorPool(h->dev, h->dpool, NULL);
        if (h->layout) h->vkDestroyPipelineLayout(h->dev, h->layout, NULL);
        if (h->dsl) h->vkDestroyDescriptorSetLayout(h->dev, h->dsl, NULL);
        if (h->fence) h->vkDestroyFence(h->dev, h->fence, NULL);
        if (h->pool) h->vkDestroyCommandPool(h->dev, h->pool, NULL);
        h->vkDestroyDevice(h->dev, NULL);
    }
    if (h->inst) {
        h->vkDestroyInstance(h->inst, NULL);
    }
    if (h->lib) {
        lib_close(h->lib);
    }
    free(h);
    d->host = NULL;
}

int rlg_host_init(RLGDevice *d)
{
    RLGHost *h;
    PFN_vkCreateInstance create_instance;

    if (!d) {
        return -1;
    }
    h = calloc(1, sizeof(*h));
    if (!h) {
        return -2;
    }
    d->host = h;
    if (!(h->lib = lib_open()) ||
        !(h->gipa = (PFN_vkGetInstanceProcAddr)lib_sym(h->lib, "vkGetInstanceProcAddr")) ||
        !(create_instance = (PFN_vkCreateInstance)(void *)h->gipa(NULL, "vkCreateInstance"))) {
        goto fail;
    }
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "radeon-legacy-vgpu",
                              .pEngineName = "r3d", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    if (create_instance(&ici, NULL, &h->inst) != VK_SUCCESS) {
        goto fail;
    }
#define LOAD_I(n) if (!(h->n = (PFN_##n)(void *)h->gipa(h->inst, #n))) goto fail;
    VK_INSTANCE_FNS(LOAD_I)

    /* Prefer a discrete GPU with Vulkan 1.3 and a graphics queue. */
    VkPhysicalDevice pds[16];
    uint32_t npd = 16, best_score = 0;
    if (h->vkEnumeratePhysicalDevices(h->inst, &npd, pds) < 0) {
        goto fail;
    }
    for (uint32_t i = 0; i < npd; ++i) {
        VkPhysicalDeviceProperties p;
        h->vkGetPhysicalDeviceProperties(pds[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) {
            continue;
        }
        uint32_t score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 :
                         p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (score > best_score) {
            best_score = score;
            h->phys = pds[i];
        }
    }
    if (!h->phys) {
        goto fail;
    }
    uint32_t nq = 16;
    VkQueueFamilyProperties qp[16];
    h->vkGetPhysicalDeviceQueueFamilyProperties(h->phys, &nq, qp);
    h->qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; ++i) {
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            h->qfam = i;
            break;
        }
    }
    if (h->qfam == UINT32_MAX) {
        goto fail;
    }
    h->vkGetPhysicalDeviceMemoryProperties(h->phys, &h->memprops);
    VkPhysicalDeviceFeatures have;
    h->vkGetPhysicalDeviceFeatures(h->phys, &have);
    h->depth_clamp = have.depthClamp;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = h->qfam,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                             .dynamicRendering = VK_TRUE,
                                             .shaderDemoteToHelperInvocation = VK_TRUE };
    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                             .pNext = &f13, .samplerMirrorClampToEdge = VK_TRUE };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12 };
    f2.features.depthClamp = have.depthClamp;
    f2.features.textureCompressionBC = have.textureCompressionBC;
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    if (h->vkCreateDevice(h->phys, &dci, NULL, &h->dev) != VK_SUCCESS) {
        goto fail;
    }
#define LOAD_D(n) if (!(h->n = (PFN_##n)(void *)h->vkGetDeviceProcAddr(h->dev, #n))) goto fail;
    VK_DEVICE_FNS(LOAD_D)
    h->vkGetDeviceQueue(h->dev, h->qfam, 0, &h->queue);

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = h->qfam };
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (h->vkCreateCommandPool(h->dev, &pci, NULL, &h->pool) != VK_SUCCESS) goto fail;
    cai.commandPool = h->pool;
    if (h->vkAllocateCommandBuffers(h->dev, &cai, &h->cmd) != VK_SUCCESS) goto fail;
    if (h->vkCreateFence(h->dev, &fci, NULL, &h->fence) != VK_SUCCESS) goto fail;

    /* set 0: binding 0 = UBO (constants), 1..16 = combined samplers per TX unit */
    VkDescriptorSetLayoutBinding bind[17];
    for (uint32_t i = 0; i < 17; ++i) {
        bind[i] = (VkDescriptorSetLayoutBinding){ .binding = i, .descriptorCount = 1,
            .descriptorType = i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
    }
    VkDescriptorSetLayoutCreateInfo dsli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                             .bindingCount = 17, .pBindings = bind };
    if (h->vkCreateDescriptorSetLayout(h->dev, &dsli, NULL, &h->dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1, .pSetLayouts = &h->dsl };
    if (h->vkCreatePipelineLayout(h->dev, &pli, NULL, &h->layout) != VK_SUCCESS) goto fail;
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, BATCH_MAX_DRAWS },
                                   { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 * BATCH_MAX_DRAWS } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = BATCH_MAX_DRAWS,
                                       .poolSizeCount = 2, .pPoolSizes = ps };
    if (h->vkCreateDescriptorPool(h->dev, &dpi, NULL, &h->dpool) != VK_SUCCESS) goto fail;
    _Static_assert(sizeof(VKFSUniforms) <= UBO_SLOT, "UBO slot too small");
    if (buf_ensure(h, &h->ubo, (VkDeviceSize)UBO_SLOT * BATCH_MAX_DRAWS, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, false) != 0) goto fail;
    h->d = d;
    return 0;

fail:
    rlg_host_destroy(d);
    return -3;
}

bool rlg_host_is_active(const RLGDevice *d) { return d && d->host; }

/* ---- state translation --------------------------------------------------------------- */

/* Colour buffer: Vulkan format whose component j stores memory channel map[j] (-1: none). */
static VkFormat color_format(unsigned cf, uint32_t outfmt, int map[4])
{
    static const int ident[4] = { 0, 1, 2, 3 }, argb[4] = { 2, 1, 0, 3 };
    memcpy(map, ident, sizeof(ident));
    switch (cf) {
    case 6: return VK_FORMAT_R8G8B8A8_UNORM;
    case 4: map[3] = -1; return VK_FORMAT_B5G6R5_UNORM_PACK16;
    case 3: memcpy(map, argb, sizeof(argb)); return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case 15: memcpy(map, argb, sizeof(argb)); return VK_FORMAT_A4R4G4B4_UNORM_PACK16;
    case 9: map[0] = 2; map[1] = map[2] = map[3] = -1; return VK_FORMAT_R8_UNORM;
    case 13: map[0] = 2; map[1] = 0; map[2] = map[3] = -1; return VK_FORMAT_R8G8_UNORM;
    case 10: return (outfmt & 0x1fu) == 18 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R16G16B16A16_UNORM;
    case 7: return VK_FORMAT_R32G32B32A32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static unsigned cf_bytes(unsigned cf)
{
    switch (cf) {
    case 9: return 1;
    case 3: case 4: case 13: case 15: return 2;
    case 10: return 8;
    case 7: return 16;
    default: return 4;
    }
}

static bool blend_factor(unsigned f, VkBlendFactor *out, bool *uses_alpha)
{
    static const VkBlendFactor map[15] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
        VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
    };
    if (f < 32 || f > 46) {
        return false;
    }
    *out = map[f - 32];
    if ((f >= 38 && f <= 42) || f >= 45) {
        *uses_alpha = true;
    }
    return true;
}

static VkBlendOp blend_op(unsigned fcn)
{
    switch (fcn & 7u) {
    case 2: case 3: return VK_BLEND_OP_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    case 6: case 7: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp cmp_op(unsigned f)
{
    static const VkCompareOp m[8] = { VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL,
                                      VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL,
                                      VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_ALWAYS };
    return m[f & 7u];
}

/* Texture format -> Vulkan format and where hardware component X/Y/Z/W lands. */
static VkFormat tex_format(const R3DTexInfo *t, VkComponentSwizzle xyzw[4])
{
    static const VkComponentSwizzle rgba[4] = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                                                VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A };
    static const VkComponentSwizzle bgra[4] = { VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G,
                                                VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A };
    bool gamma = (t->fmt1 >> 21) & 1u, dxtc_swz = (t->filter1 >> 21) & 1u;

    if ((t->fmt1 >> 5) & 0xfu) {
        return VK_FORMAT_UNDEFINED;                     /* signed components */
    }
    memcpy(xyzw, rgba, sizeof(rgba));
    switch (t->fmt) {
    case 0x0: return gamma ? VK_FORMAT_UNDEFINED : VK_FORMAT_R8_UNORM;
    case 0x3: return gamma ? VK_FORMAT_UNDEFINED : VK_FORMAT_R8G8_UNORM;
    case 0xc: return gamma ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    case 0x6: return gamma ? VK_FORMAT_UNDEFINED : VK_FORMAT_B5G6R5_UNORM_PACK16;
    case 0xb: memcpy(xyzw, bgra, sizeof(bgra)); return gamma ? VK_FORMAT_UNDEFINED : VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case 0xa: memcpy(xyzw, bgra, sizeof(bgra)); return gamma ? VK_FORMAT_UNDEFINED : VK_FORMAT_A4R4G4B4_UNORM_PACK16;
    case 0x1: return gamma ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16_UNORM;
    case 0x4: return gamma ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16_UNORM;
    case 0xe: return gamma ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16B16A16_UNORM;
    case 0x18: return gamma ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16_SFLOAT;
    case 0x19: return gamma ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16_SFLOAT;
    case 0x1a: return gamma ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x1b: return gamma ? VK_FORMAT_UNDEFINED : VK_FORMAT_R32_SFLOAT;
    case 0x1c: return gamma ? VK_FORMAT_UNDEFINED : VK_FORMAT_R32G32_SFLOAT;
    case 0x1d: return gamma ? VK_FORMAT_UNDEFINED : VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0xf: case 0x10: case 0x11:
        /* BC: red in R. The hardware delivers red in X, or in Z with DXTC_SWIZZLE. */
        if (dxtc_swz) {
            memcpy(xyzw, bgra, sizeof(bgra));
        }
        if (t->fmt == 0xf) return gamma ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        if (t->fmt == 0x10) return gamma ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK;
        return gamma ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

static bool address_mode(unsigned m, uint32_t border, VkSamplerAddressMode *out, VkBorderColor *bc)
{
    switch (m & 7u) {
    case 0: *out = VK_SAMPLER_ADDRESS_MODE_REPEAT; return true;
    case 1: *out = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; return true;
    case 2: case 4: *out = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; return true;
    case 3: case 5: *out = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; return true;
    case 6:
        *out = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        if (border == 0) *bc = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (border == 0xff000000u) *bc = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if (border == 0xffffffffu) *bc = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else return false;
        return true;
    default:
        return false;
    }
}

static uint64_t fnv1a(const uint32_t *w, size_t n)
{
    uint64_t hsh = 1469598103934665603ull;
    const uint8_t *p = (const uint8_t *)w;
    for (size_t i = 0; i < n * 4; ++i) {
        hsh = (hsh ^ p[i]) * 1099511628211ull;
    }
    return hsh;
}

static VkShaderModule make_module(RLGHost *h, const uint32_t *w, size_t n)
{
    VkShaderModule m = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n * 4, .pCode = w };
    h->vkCreateShaderModule(h->dev, &ci, NULL, &m);
    return m;
}

static VkPipeline get_pipeline(RLGHost *h, const PipeKey *k, const uint32_t *fs, size_t fsn)
{
    for (unsigned i = 0; i < h->npipes; ++i) {
        if (!memcmp(&h->pipes[i].key, k, sizeof(*k))) {
            return h->pipes[i].pipe;
        }
    }
    size_t vsn;
    uint32_t *vs = vk_vs_passthrough(k->nvary, &vsn);
    VkShaderModule vm = vs ? make_module(h, vs, vsn) : VK_NULL_HANDLE;
    VkShaderModule fm = make_module(h, fs, fsn);
    VkPipeline pipe = VK_NULL_HANDLE;
    free(vs);
    if (vm && fm) {
        VkPipelineShaderStageCreateInfo st[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" },
        };
        VkVertexInputBindingDescription vb = { 0, (k->nvary + 1) * 16, VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription va[17];
        for (uint32_t i = 0; i <= k->nvary; ++i) {
            va[i] = (VkVertexInputAttributeDescription){ i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16 };
        }
        VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
            .vertexAttributeDescriptionCount = k->nvary + 1, .pVertexAttributeDescriptions = va };
        VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
        VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .scissorCount = 1 };
        VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .depthClampEnable = h->depth_clamp, .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE, .frontFace = k->front_face, .lineWidth = 1.0f };
        VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
        VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = k->depth_test, .depthWriteEnable = k->depth_write,
            .depthCompareOp = (VkCompareOp)k->depth_op, .stencilTestEnable = k->stencil,
            .front = k->front, .back = k->back };
        VkPipelineColorBlendAttachmentState cb = { .blendEnable = k->blend,
            .srcColorBlendFactor = (VkBlendFactor)k->src_c, .dstColorBlendFactor = (VkBlendFactor)k->dst_c,
            .colorBlendOp = (VkBlendOp)k->op_c, .srcAlphaBlendFactor = (VkBlendFactor)k->src_a,
            .dstAlphaBlendFactor = (VkBlendFactor)k->dst_a, .alphaBlendOp = (VkBlendOp)k->op_a,
            .colorWriteMask = k->write_mask };
        VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &cb };
        VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS };
        VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 3, .pDynamicStates = dyn };
        VkFormat stencil_fmt = k->depth_fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ? k->depth_fmt : VK_FORMAT_UNDEFINED;
        VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1, .pColorAttachmentFormats = &k->color_fmt,
            .depthAttachmentFormat = k->depth_fmt, .stencilAttachmentFormat = stencil_fmt };
        VkGraphicsPipelineCreateInfo gp = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
            .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
            .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
            .pDepthStencilState = &ds, .pColorBlendState = &cbs, .pDynamicState = &dy, .layout = h->layout };
        if (h->vkCreateGraphicsPipelines(h->dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe) != VK_SUCCESS) {
            pipe = VK_NULL_HANDLE;
        }
    }
    if (vm) h->vkDestroyShaderModule(h->dev, vm, NULL);
    if (fm) h->vkDestroyShaderModule(h->dev, fm, NULL);
    if (pipe) {
        if (h->npipes == PIPE_CACHE_MAX) {
            if (h->b.open) {
                vk_flush(h->d);             /* the victim may be recorded in the batch */
            }
            h->vkDestroyPipeline(h->dev, h->pipes[0].pipe, NULL);
            memmove(h->pipes, h->pipes + 1, (PIPE_CACHE_MAX - 1) * sizeof(PipeEntry));
            h->npipes--;
        }
        h->pipes[h->npipes].key = *k;
        h->pipes[h->npipes].pipe = pipe;
        h->npipes++;
    }
    return pipe;
}

static void barrier(RLGHost *h, VkImage img, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, 1 } };
    h->vkCmdPipelineBarrier(h->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            0, 0, NULL, 0, NULL, 1, &b);
}

/* ---- draw --------------------------------------------------------------------------- */

typedef struct {
    VkImg img;
    VkSampler sampler;
} TexBind;

#define ALIGN16(x) (((x) + 15u) & ~(VkDeviceSize)15u)

int r3d_vk_draw(RLGDevice *d, const R3DDrawInfo *info, const R3DTri *tris, unsigned count)
{
    RLGHost *h = d->host;
    VKFSInfo fsi;
    VKFSKey key;
    PipeKey pk;
    TexBind tex[16];
    uint32_t *fs = NULL;
    size_t fsn = 0;

    if (!h || !count) {
        return count ? -1 : 0;
    }
    memset(&key, 0, sizeof(key));
    memset(&pk, 0, sizeof(pk));
    memset(tex, 0, sizeof(tex));

    /* ---- colour buffer 0 only (MRT and CBZB fall back) ---- */
    uint32_t cctl = rlg_reg_read32(d, RLG_RB3D_CCTL);
    for (unsigned k = 1; k < 4; ++k) {
        if (rlg_reg_read32(d, RLG_RB3D_COLORPITCH0 + k * 4u) & 0x3ffeu) {
            return -1;
        }
    }
    if ((cctl & (3u << 5)) || (rlg_reg_read32(d, RLG_ZB_BW_CNTL) & RLG_ZB_CB_CLEAR_WRITE_ONLY)) {
        return -1;
    }
    uint32_t cpitchreg = rlg_reg_read32(d, RLG_RB3D_COLORPITCH0);
    uint32_t cpitch = cpitchreg & 0x3ffeu, cf = (cpitchreg >> 21) & 0xfu;
    uint32_t outfmt = rlg_reg_read32(d, RLG_US_OUT_FMT_0);
    uint32_t cbase = rlg_reg_read32(d, RLG_RB3D_COLOROFFSET0) & ~31u;
    int cmap[4];
    VkFormat cvk = color_format(cf, outfmt, cmap);
    unsigned cbytes = cf_bytes(cf), sel[4];
    if (!cpitch || cpitch > 4096 || cvk == VK_FORMAT_UNDEFINED) {
        return -1;
    }
    for (int c = 0; c < 4; ++c) {
        unsigned s = (outfmt >> (8 + 2 * c)) & 3u;
        sel[c] = s == 0 ? 3u : s - 1u;          /* memory channel c <- shader R/G/B/A */
    }

    /* ---- region: rows touched by the triangles, inside the scissor ---- */
    float ymin = 1e30f, ymax = -1e30f;
    for (unsigned i = 0; i < count; ++i) {
        for (int v = 0; v < 3; ++v) {
            ymin = fminf(ymin, tris[i].v[v].pos[1]);
            ymax = fmaxf(ymax, tris[i].v[v].pos[1]);
        }
    }
    int y0 = (int)floorf(ymin), y1 = (int)ceilf(ymax);
    y0 = y0 < info->sc_y0 ? info->sc_y0 : y0;
    y1 = y1 > info->sc_y1 ? info->sc_y1 : y1;
    uint32_t cvoff;
    if (!rlg_mc_to_vram(d, cbase, (uint64_t)cpitch * cbytes, &cvoff)) {
        return -1;                              /* colour buffer outside VRAM (GART) */
    }
    int max_rows = (int)((d->vram_size - cvoff) / ((uint64_t)cpitch * cbytes));
    y1 = y1 >= max_rows ? max_rows - 1 : y1;
    if (y0 > y1) {
        return 0;                               /* nothing visible */
    }
    uint32_t W = cpitch;
    uint64_t crow = (uint64_t)cpitch * cbytes;

    /* ---- scissor + cliprects ---- */
    int sx0 = info->sc_x0, sy0 = info->sc_y0, sx1 = info->sc_x1, sy1 = info->sc_y1;
    uint32_t rule = rlg_reg_read32(d, RLG_SC_CLIP_RULE) & 0xffffu;
    if (rule == 0xaaaau) {
        uint32_t tl = rlg_reg_read32(d, RLG_SC_CLIPRECT_TL_0), br = rlg_reg_read32(d, RLG_SC_CLIPRECT_BR_0);
        int cx0 = (int)(tl & 0x1fffu) - RLG_CLIPRECT_OFFSET, cy0 = (int)((tl >> 13) & 0x1fffu) - RLG_CLIPRECT_OFFSET;
        int cx1 = (int)(br & 0x1fffu) - RLG_CLIPRECT_OFFSET, cy1 = (int)((br >> 13) & 0x1fffu) - RLG_CLIPRECT_OFFSET;
        sx0 = cx0 > sx0 ? cx0 : sx0; sy0 = cy0 > sy0 ? cy0 : sy0;
        sx1 = cx1 < sx1 ? cx1 : sx1; sy1 = cy1 < sy1 ? cy1 : sy1;
    } else if (rule != 0 && rule != 0xffffu) {
        return -1;
    }
    sx0 = sx0 < 0 ? 0 : sx0;
    sy0 = sy0 < y0 ? y0 : sy0;
    sx1 = sx1 >= (int)W ? (int)W - 1 : sx1;
    sy1 = sy1 > y1 ? y1 : sy1;
    if (sx0 > sx1 || sy0 > sy1) {
        return 0;
    }

    /* ---- fragment program ---- */
    vk_fs_analyze(info->fp, &fsi);
    uint64_t inputs;
    {
        float tmp[R3D_MAX_TEMPS][4];
        r3d_rs_eval(d, info, tris[0].v[0].color, tris[0].v[0].tex, tmp, &inputs);
    }
    unsigned nvary = 0;
    for (unsigned t = 0; t < R3D_MAX_TEMPS; ++t) {
        nvary += (inputs >> t) & 1u;
    }
    if (nvary > 16) {
        return -1;
    }
    uint32_t afunc = rlg_reg_read32(d, RLG_FG_ALPHA_FUNC);
    key.inputs = inputs;
    key.tex_units = fsi.tex_units;
    key.out_mask = 1;                           /* only target 0 is bound */
    key.alpha_test = (afunc >> 11) & 1u;
    key.alpha_func = (afunc >> 8) & 7u;
    key.depth_write = fsi.depth_write;
    for (int j = 0; j < 4; ++j) {
        key.out_swz[0][j] = cmap[j] >= 0 ? (uint8_t)sel[cmap[j]] : 4;
    }

    /* ---- blend / write mask ---- */
    uint32_t cblend = rlg_reg_read32(d, RLG_RB3D_CBLEND), ablend = rlg_reg_read32(d, RLG_RB3D_ABLEND);
    uint32_t chmask = rlg_reg_read32(d, RLG_RB3D_COLOR_CHANNEL_MASK) & 0xfu;
    for (int j = 0; j < 4; ++j) {
        if (cmap[j] >= 0 && (chmask & (1u << cmap[j]))) {
            pk.write_mask |= 1u << j;
        }
    }
    float blend_const[4] = {0, 0, 0, 0};
    if (cblend & 1u) {
        VkBlendFactor f[4];
        bool alpha_used = false;
        uint32_t as = (cblend & 2u) ? ablend : cblend;
        if (!blend_factor((cblend >> 16) & 63u, &f[0], &alpha_used) ||
            !blend_factor((cblend >> 24) & 63u, &f[1], &alpha_used) ||
            !blend_factor((as >> 16) & 63u, &f[2], &alpha_used) ||
            !blend_factor((as >> 24) & 63u, &f[3], &alpha_used)) {
            return -1;
        }
        if (alpha_used && !(cmap[3] >= 0 && sel[cmap[3]] == 3)) {
            return -1;                          /* Vulkan alpha must be the shader alpha */
        }
        pk.blend = 1;
        pk.src_c = f[0]; pk.dst_c = f[1]; pk.op_c = blend_op((cblend >> 12) & 7u);
        pk.src_a = f[2]; pk.dst_a = f[3]; pk.op_a = blend_op((as >> 12) & 7u);
        uint32_t bc = rlg_reg_read32(d, RLG_RB3D_BLEND_COLOR);
        float cst[4] = { ((bc >> 16) & 255u) / 255.0f, ((bc >> 8) & 255u) / 255.0f,
                         (bc & 255u) / 255.0f, (bc >> 24) / 255.0f };
        for (int j = 0; j < 4; ++j) {
            blend_const[j] = cmap[j] >= 0 ? cst[sel[cmap[j]]] : 0.0f;
        }
    }

    /* ---- depth / stencil ---- */
    uint32_t zcntl = rlg_reg_read32(d, RLG_ZB_CNTL), zfmt = rlg_reg_read32(d, RLG_ZB_FORMAT) & 0xfu;
    uint32_t zs = rlg_reg_read32(d, RLG_ZB_ZSTENCILCNTL), refmask = rlg_reg_read32(d, RLG_ZB_STENCILREFMASK);
    uint32_t zpitch = rlg_reg_read32(d, RLG_ZB_DEPTHPITCH) & 0x3ffcu;
    uint32_t zbase = rlg_reg_read32(d, RLG_ZB_DEPTHOFFSET) & ~31u, zvoff = 0;
    bool zen = zcntl & 2u, zwr = zcntl & 4u, sten = (zcntl & 1u) && zfmt == 2;
    bool use_depth = (zen || sten) && zpitch;
    unsigned zbytes = zfmt == 2 ? 4 : 2;
    /* rows kept resident: up to the scissor bottom (drivers set it to the surface
     * size), so later draws of the frame fit; without one, the rows in use */
    uint32_t rows = info->sc_y1 < 4095 ? (uint32_t)info->sc_y1 + 1 : ((uint32_t)y1 + 64u) & ~63u;
    rows = rows < (uint32_t)y1 + 1 ? (uint32_t)y1 + 1 : rows;
    rows = rows > (uint32_t)max_rows ? (uint32_t)max_rows : rows;
    if (rows > 4096) {
        return -1;
    }
    if (use_depth) {
        if ((zfmt != 0 && zfmt != 2) || zpitch != cpitch ||
            !rlg_mc_to_vram(d, zbase, (uint64_t)rows * zpitch * zbytes, &zvoff)) {
            return -1;
        }
        pk.depth_fmt = zfmt == 2 ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D16_UNORM;
        pk.depth_test = zen;
        pk.depth_write = zen && zwr;
        pk.depth_op = cmp_op(zs & 7u);
        if (sten) {
            pk.stencil = 1;
            VkStencilOpState f = { (VkStencilOp)((zs >> 6) & 7u), (VkStencilOp)((zs >> 9) & 7u),
                                   (VkStencilOp)((zs >> 12) & 7u), cmp_op((zs >> 3) & 7u),
                                   (refmask >> 8) & 0xffu, (refmask >> 16) & 0xffu, refmask & 0xffu };
            VkStencilOpState bk = f;
            if (zcntl & (1u << 4)) {
                bk.failOp = (VkStencilOp)((zs >> 18) & 7u);
                bk.passOp = (VkStencilOp)((zs >> 21) & 7u);
                bk.depthFailOp = (VkStencilOp)((zs >> 24) & 7u);
                bk.compareOp = cmp_op((zs >> 15) & 7u);
            }
            pk.front = f;
            pk.back = bk;
        }
    }

    /* ---- textures: validate and describe; resources are taken after any flush ---- */
    R3DTexInfo tinfo[16];
    VkFormat tfmt[16];
    VkComponentMapping tcm[16];
    VkSamplerCreateInfo tsci[16];
    uint32_t tspan[16];
    VkDeviceSize tex_total = 0;
    for (unsigned u = 0; u < 16; ++u) {
        if (!(fsi.tex_units & (1u << u))) {
            continue;
        }
        R3DTexInfo *ti = &tinfo[u];
        VkComponentSwizzle xyzw[4];
        r3d_tex_info(d, u, ti);
        if (!(rlg_reg_read32(d, RLG_TX_ENABLE) & (1u << u)) || ti->target != 0) {
            return -1;
        }
        tfmt[u] = tex_format(ti, xyzw);
        if (tfmt[u] == VK_FORMAT_UNDEFINED) {
            return -1;
        }
        static const unsigned shift[4] = { 12, 15, 18, 9 };
        VkComponentSwizzle *dst[4] = { &tcm[u].r, &tcm[u].g, &tcm[u].b, &tcm[u].a };
        for (int c = 0; c < 4; ++c) {
            unsigned sw = (ti->fmt1 >> shift[c]) & 7u;
            if (sw >= 6) return -1;
            *dst[c] = sw < 4 ? xyzw[sw] : (sw == 4 ? VK_COMPONENT_SWIZZLE_ZERO : VK_COMPONENT_SWIZZLE_ONE);
        }
        VkSamplerAddressMode am[2];
        VkBorderColor bcol = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        if (!address_mode(ti->filter0 & 7u, ti->border, &am[0], &bcol) ||
            !address_mode((ti->filter0 >> 3) & 7u, ti->border, &am[1], &bcol)) {
            return -1;
        }
        uint32_t levels = ti->levels + 1;
        unsigned mag = (ti->filter0 >> 9) & 3u, min = (ti->filter0 >> 11) & 3u, mip = (ti->filter0 >> 13) & 3u;
        int bias = (int)((ti->filter1 >> 3) & 0x3ffu);
        bias = bias & 0x200 ? bias - 0x400 : bias;
        tsci[u] = (VkSamplerCreateInfo){ .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = mag == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
            .minFilter = min == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
            .mipmapMode = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = am[0], .addressModeV = am[1], .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .mipLodBias = (float)bias / 32.0f, .minLod = 0.0f,
            .maxLod = mip == 0 ? 0.0f : (float)(levels - 1 - (ti->min_level < levels ? ti->min_level : 0)),
            .borderColor = bcol };
        tspan[u] = 0;
        for (unsigned l = 0; l <= ti->levels; ++l) {
            uint32_t stride, layer, lo = r3d_tex_level(d, u, l, &stride, &layer);
            tex_total += ALIGN16(layer);
            tspan[u] = lo + layer > tspan[u] ? lo + layer : tspan[u];
        }
    }

    /* ---- shader + pipeline (may flush when the pipeline cache evicts) ---- */
    fs = vk_fs_translate(info->fp, &key, &fsn);
    if (!fs) {
        return -1;
    }
    pk.fs_hash = fnv1a(fs, fsn);
    pk.fs_words = (uint32_t)fsn;
    pk.nvary = nvary;
    pk.color_fmt = cvk;
    /* Our "front" is CCW on screen (y down); Vulkan's CCW front uses the same framebuffer sense. */
    pk.front_face = (rlg_reg_read32(d, RLG_SU_CULL_MODE) & RLG_FRONT_FACE_CW) ? VK_FRONT_FACE_CLOCKWISE
                                                                              : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPipeline pipe = get_pipeline(h, &pk, fs, fsn);
    free(fs);
    if (!pipe) {
        return -1;
    }

    /* ---- batch: flush when this draw cannot join the open one ---- */
    Batch *b = &h->b;
    VkImageAspectFlags daspect = pk.depth_fmt == VK_FORMAT_D32_SFLOAT_S8_UINT
                                     ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    VkDeviceSize tgt_size = ALIGN16((VkDeviceSize)rows * crow) +
                            (use_depth ? ALIGN16((VkDeviceSize)rows * W * 4) + ALIGN16((VkDeviceSize)rows * W) : 0);
    VkDeviceSize vsize = ALIGN16((VkDeviceSize)count * 3 * (nvary + 1) * 16);
    if (b->open) {
        bool ok = b->bound && b->cvoff == cvoff && b->W == W && b->cvk == cvk && b->cf == cf && b->rows > (uint32_t)y1 &&
                  (!use_depth || (b->zbound && b->zvoff == zvoff && b->dfmt == pk.depth_fmt)) &&
                  b->draws < BATCH_MAX_DRAWS && b->nborrowed + 16 <= IMG_CACHE_MAX - 2 &&
                  b->stg_at + tex_total <= h->staging.size && b->vbo_at + vsize <= h->vbo.size;
        /* a texture must see what the batch rendered into its memory */
        for (unsigned u = 0; ok && u < 16; ++u) {
            uint32_t toff;
            if (!(fsi.tex_units & (1u << u)) || !rlg_mc_to_vram(d, tinfo[u].base, tspan[u], &toff)) {
                continue;
            }
            uint64_t t0 = toff, t1 = t0 + tspan[u];
            uint64_t c0 = b->cvoff, c1 = c0 + (uint64_t)b->rows * b->crow;
            uint64_t z0 = b->zvoff, z1 = z0 + (uint64_t)b->rows * b->W * (b->zfmt == 2 ? 4 : 2);
            if ((t0 < c1 && c0 < t1) || (b->zbound && t0 < z1 && z0 < t1)) {
                ok = false;
            }
        }
        if (!ok && vk_flush(d) != 0) {
            return -1;
        }
    }
    if (!b->open) {
        if (h->nsamps > SAMP_CACHE_MAX - 16) {
            for (unsigned i = 0; i < h->nsamps; ++i) {
                h->vkDestroySampler(h->dev, h->samps[i].samp, NULL);
            }
            h->nsamps = 0;
        }
        VkDeviceSize want = tex_total + tgt_size;
        if (buf_ensure(h, &h->staging, want > (32u << 20) ? want : (32u << 20),
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true) != 0 ||
            buf_ensure(h, &h->vbo, vsize > (4u << 20) ? vsize : (4u << 20), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false) != 0) {
            return -1;
        }
        VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        h->vkResetDescriptorPool(h->dev, h->dpool, 0);
        h->vkResetCommandBuffer(h->cmd, 0);
        h->vkBeginCommandBuffer(h->cmd, &cbi);
        memset(b, 0, sizeof(*b));
        b->open = true;
    }
    uint8_t *stg = h->staging.map;

    /* ---- bind the target: upload rows [0, rows) once per batch ---- */
    if (!b->bound) {
        if (img_get(h, &b->color, cvk, W, rows, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, NULL) != 0) {
            return -1;
        }
        if (use_depth && img_get(h, &b->depth, pk.depth_fmt, W, rows, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, daspect, 0, NULL) != 0) {
            img_put(h, &b->color);
            return -1;
        }
        b->bound = true;
        b->zbound = use_depth;
        b->cvoff = cvoff; b->W = W; b->rows = rows; b->cf = cf; b->cvk = cvk; b->crow = crow;
        b->zvoff = zvoff; b->zfmt = zfmt; b->dfmt = pk.depth_fmt; b->daspect = daspect;
        b->y0 = (int)rows;
        b->y1 = -1;
        b->c_stg = b->stg_at;
        memcpy(stg + b->c_stg, d->vram + cvoff, (size_t)rows * crow);
        b->stg_at += ALIGN16((VkDeviceSize)rows * crow);
        barrier(h, b->color.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy cc = { .bufferOffset = b->c_stg, .bufferRowLength = W,
                                 .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, rows, 1 } };
        h->vkCmdCopyBufferToImage(h->cmd, h->staging.buf, b->color.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cc);
        barrier(h, b->color.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT);
        if (use_depth) {
            /* Z16 raw; Z24S8 -> float depth + stencil bytes */
            uint64_t n = (uint64_t)W * rows;
            const uint8_t *zsrc = d->vram + zvoff;
            b->z_stg = b->stg_at;
            b->s_stg = ALIGN16(b->z_stg + n * 4);
            b->stg_at = ALIGN16(b->s_stg + n);
            if (zfmt == 2) {
                float *zd = (float *)(stg + b->z_stg);
                uint8_t *sd = stg + b->s_stg;
                for (uint64_t i = 0; i < n; ++i) {
                    uint32_t v;
                    memcpy(&v, zsrc + i * 4, 4);
                    zd[i] = (float)(v >> 8) / 16777215.0f;
                    sd[i] = (uint8_t)(v & 0xffu);
                }
            } else {
                memcpy(stg + b->z_stg, zsrc, n * 2);
            }
            barrier(h, b->depth.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, daspect);
            VkBufferImageCopy dc[2] = {
                { .bufferOffset = b->z_stg, .bufferRowLength = W, .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 }, .imageExtent = { W, rows, 1 } },
                { .bufferOffset = b->s_stg, .bufferRowLength = W, .imageSubresource = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1 }, .imageExtent = { W, rows, 1 } },
            };
            h->vkCmdCopyBufferToImage(h->cmd, h->staging.buf, b->depth.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, zfmt == 2 ? 2 : 1, dc);
            barrier(h, b->depth.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, daspect);
        }
    }

    /* ---- textures: images and samplers, borrowed until the flush ---- */
    for (unsigned u = 0; u < 16; ++u) {
        if (!(fsi.tex_units & (1u << u))) continue;
        uint32_t levels = tinfo[u].levels + 1;
        if (img_get(h, &tex[u].img, tfmt[u], tinfo[u].w0, tinfo[u].h0, levels,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                    tinfo[u].min_level < levels ? tinfo[u].min_level : 0, &tcm[u]) != 0) {
            return -1;
        }
        b->borrowed[b->nborrowed++] = tex[u].img;
        if (!(tex[u].sampler = samp_get(h, &tsci[u]))) {
            return -1;
        }
    }

    /* ---- descriptors (UBO slot per draw) ---- */
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = h->dpool,
                                         .descriptorSetCount = 1, .pSetLayouts = &h->dsl };
    if (h->vkAllocateDescriptorSets(h->dev, &dsai, &set) != VK_SUCCESS) {
        return -1;
    }
    VkDeviceSize ubo_off = (VkDeviceSize)b->draws * UBO_SLOT;
    {
        VKFSUniforms *u = (VKFSUniforms *)((uint8_t *)h->ubo.map + ubo_off);
        memcpy(u->c, info->fp->consts, sizeof(u->c));
        u->alpha_ref[0] = (float)(afunc & 0xffu) / 255.0f;
    }
    {
        VkDescriptorBufferInfo ubi = { h->ubo.buf, ubo_off, sizeof(VKFSUniforms) };
        VkDescriptorImageInfo ii[16];
        VkWriteDescriptorSet wr[17];
        uint32_t nw = 0;
        wr[nw++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
                                           .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                           .pBufferInfo = &ubi };
        for (unsigned u = 0; u < 16; ++u) {
            if (!(fsi.tex_units & (1u << u))) continue;
            ii[u] = (VkDescriptorImageInfo){ tex[u].sampler, tex[u].img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            wr[nw++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1 + u,
                                               .descriptorCount = 1,
                                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                               .pImageInfo = &ii[u] };
        }
        h->vkUpdateDescriptorSets(h->dev, nw, wr, 0, NULL);
    }

    /* ---- vertices: NDC over the resident rows ---- */
    VkDeviceSize voff = b->vbo_at;
    {
        float *vd = (float *)((uint8_t *)h->vbo.map + voff);
        for (unsigned i = 0; i < count; ++i) {
            for (int v = 0; v < 3; ++v) {
                const R3DVertex *vx = &tris[i].v[v];
                float temps[R3D_MAX_TEMPS][4];
                float rhw = vx->rhw, w = (rhw > 0.0f && isfinite(rhw)) ? 1.0f / rhw : 1.0f;
                r3d_rs_eval(d, info, vx->color, vx->tex, temps, NULL);
                vd[0] = ((vx->pos[0] / (float)W) * 2.0f - 1.0f) * w;
                vd[1] = ((vx->pos[1] / (float)rows) * 2.0f - 1.0f) * w;
                vd[2] = vx->pos[2] * w;
                vd[3] = w;
                vd += 4;
                for (unsigned t = 0; t < R3D_MAX_TEMPS; ++t) {
                    if (inputs & (1ull << t)) {
                        memcpy(vd, temps[t], 16);
                        vd += 4;
                    }
                }
            }
        }
    }
    b->vbo_at += vsize;

    /* ---- record: texture uploads from their r300 layout, then the draw ---- */
    for (unsigned u = 0; u < 16; ++u) {
        if (!(fsi.tex_units & (1u << u))) continue;
        const R3DTexInfo *ti = &tinfo[u];
        barrier(h, tex[u].img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        for (unsigned l = 0; l <= ti->levels; ++l) {
            uint32_t stride, layer, lo = r3d_tex_level(d, u, l, &stride, &layer);
            uint32_t lw = ti->w0 >> l ? ti->w0 >> l : 1, lh = ti->h0 >> l ? ti->h0 >> l : 1;
            if (rlg_gpu_read(d, (uint64_t)ti->base + lo, stg + b->stg_at, layer) != 0) {
                memset(stg + b->stg_at, 0, layer);
            }
            VkBufferImageCopy tc = { .bufferOffset = b->stg_at,
                .bufferRowLength = ti->compressed ? stride / ti->bpp * 4 : stride / ti->bpp,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 }, .imageExtent = { lw, lh, 1 } };
            h->vkCmdCopyBufferToImage(h->cmd, h->staging.buf, tex[u].img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &tc);
            b->stg_at += ALIGN16(layer);
        }
        barrier(h, tex[u].img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = b->color.view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingAttachmentInfo da = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = b->depth.view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo rinfo = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { W, rows } },
        .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca,
        .pDepthAttachment = use_depth ? &da : NULL,
        .pStencilAttachment = (use_depth && zfmt == 2) ? &da : NULL };
    h->vkCmdBeginRendering(h->cmd, &rinfo);
    h->vkCmdBindPipeline(h->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    h->vkCmdBindDescriptorSets(h->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, h->layout, 0, 1, &set, 0, NULL);
    h->vkCmdBindVertexBuffers(h->cmd, 0, 1, &h->vbo.buf, &voff);
    VkViewport vp = { 0.0f, 0.0f, (float)W, (float)rows, 0.0f, 1.0f };
    VkRect2D sc = { { sx0, sy0 }, { (uint32_t)(sx1 - sx0 + 1), (uint32_t)(sy1 - sy0 + 1) } };
    h->vkCmdSetViewport(h->cmd, 0, 1, &vp);
    h->vkCmdSetScissor(h->cmd, 0, 1, &sc);
    h->vkCmdSetBlendConstants(h->cmd, blend_const);
    h->vkCmdDraw(h->cmd, count * 3, 1, 0, 0);
    h->vkCmdEndRendering(h->cmd);
    /* the next draw loads what this one stored */
    barrier(h, b->color.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);
    if (use_depth) {
        barrier(h, b->depth.img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, daspect);
    }

    b->y0 = sy0 < b->y0 ? sy0 : b->y0;
    b->y1 = sy1 > b->y1 ? sy1 : b->y1;
    b->zdirty |= use_depth && (pk.depth_write || pk.stencil);
    b->draws++;
    if (b->draws == BATCH_MAX_DRAWS) {
        vk_flush(d);
    }
    return 0;
}

/* Submit the batch, wait, and write the dirty target rows back into VRAM. */
static int vk_flush(RLGDevice *d)
{
    RLGHost *h = d->host;
    Batch *b = &h->b;
    uint8_t *stg = h->staging.map;
    int rc = 0;

    if (!b->open) {
        return 0;
    }
    bool rb = b->bound && b->y0 <= b->y1;
    uint32_t n = rb ? (uint32_t)(b->y1 - b->y0 + 1) : 0;
    if (rb) {
        barrier(h, b->color.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy cc = { .bufferOffset = b->c_stg + (VkDeviceSize)b->y0 * b->crow, .bufferRowLength = b->W,
                                 .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageOffset = { 0, b->y0, 0 }, .imageExtent = { b->W, n, 1 } };
        h->vkCmdCopyImageToBuffer(h->cmd, b->color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, h->staging.buf, 1, &cc);
        if (b->zbound && b->zdirty) {
            barrier(h, b->depth.img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    b->daspect);
            VkDeviceSize zb = b->zfmt == 2 ? 4 : 2;
            VkBufferImageCopy dc[2] = {
                { .bufferOffset = b->z_stg + (VkDeviceSize)b->y0 * b->W * zb, .bufferRowLength = b->W,
                  .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 }, .imageOffset = { 0, b->y0, 0 },
                  .imageExtent = { b->W, n, 1 } },
                { .bufferOffset = b->s_stg + (VkDeviceSize)b->y0 * b->W, .bufferRowLength = b->W,
                  .imageSubresource = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1 }, .imageOffset = { 0, b->y0, 0 },
                  .imageExtent = { b->W, n, 1 } },
            };
            h->vkCmdCopyImageToBuffer(h->cmd, b->depth.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, h->staging.buf,
                                      b->zfmt == 2 ? 2 : 1, dc);
        }
        VkMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
        h->vkCmdPipelineBarrier(h->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    }
    h->vkEndCommandBuffer(h->cmd);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &h->cmd };
    h->vkResetFences(h->dev, 1, &h->fence);
    if (h->vkQueueSubmit(h->queue, 1, &si, h->fence) != VK_SUCCESS ||
        h->vkWaitForFences(h->dev, 1, &h->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        rlg__log(d, "vulkan: batch of %u draws lost (submit failed)", b->draws);
        rb = false;
        rc = -1;
    }
    if (rb) {
        uint64_t off = b->cvoff + (uint64_t)b->y0 * b->crow, sz = (uint64_t)n * b->crow;
        memcpy(d->vram + off, stg + b->c_stg + (uint64_t)b->y0 * b->crow, sz);
        if (d->cfg.dirty) {
            d->cfg.dirty(d->cfg.opaque, (uint32_t)off, (uint32_t)sz);
        }
        if (b->zbound && b->zdirty) {
            uint64_t first = (uint64_t)b->y0 * b->W, cnt = (uint64_t)n * b->W;
            uint8_t *zdst = d->vram + b->zvoff;
            if (b->zfmt == 2) {
                const float *zd = (const float *)(stg + b->z_stg);
                const uint8_t *sd = stg + b->s_stg;
                for (uint64_t i = first; i < first + cnt; ++i) {
                    float z = zd[i] < 0.0f ? 0.0f : (zd[i] > 1.0f ? 1.0f : zd[i]);
                    uint32_t v = ((uint32_t)(z * 16777215.0f + 0.5f) << 8) | sd[i];
                    memcpy(zdst + i * 4, &v, 4);
                }
            } else {
                memcpy(zdst + first * 2, stg + b->z_stg + first * 2, cnt * 2);
            }
            if (d->cfg.dirty) {
                uint64_t zb = b->zfmt == 2 ? 4 : 2;
                d->cfg.dirty(d->cfg.opaque, (uint32_t)(b->zvoff + first * zb), (uint32_t)(cnt * zb));
            }
        }
    }
    for (unsigned i = 0; i < b->nborrowed; ++i) {
        img_put(h, &b->borrowed[i]);
    }
    img_put(h, &b->color);
    img_put(h, &b->depth);
    memset(b, 0, sizeof(*b));
    return rc;
}

void r3d_vk_flush(RLGDevice *d)
{
    if (d && d->host) {
        vk_flush(d);
    }
}

#else /* !RLG_HAVE_VULKAN */

struct RLGHost { int unused; };
int rlg_host_init(RLGDevice *d) { (void)d; return -1; }
void r3d_vk_flush(RLGDevice *d) { (void)d; }
void rlg_host_destroy(RLGDevice *d) { if (d) d->host = NULL; }
bool rlg_host_is_active(const RLGDevice *d) { return d && d->host; }
int r3d_vk_draw(RLGDevice *d, const R3DDrawInfo *info, const R3DTri *tris, unsigned count)
{
    (void)d; (void)info; (void)tris; (void)count;
    return -1;
}

#endif
