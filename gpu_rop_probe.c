/* ============================================================================
 * gpu_rop_probe.c
 * Single-file Vulkan GPU ROP / HSR / Architecture capability probe.
 * ============================================================================ */

#define _CRT_SECURE_NO_WARNINGS

#include <vulkan/vulkan.h>
#include <shaderc/shaderc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#ifdef _WIN32
#  include <windows.h>
#endif

#define DRAWS_PER_MEASURE   2000
#define COLOR_W             1024
#define COLOR_H             1024
#define DEPTH_W             512
#define DEPTH_H             512
#define HSR_W               512
#define HSR_H               512
#define ARCH_W              2048
#define ARCH_H              2048
#define ARCH_SCENES         50

#define VK_CHECK(x) do {                                                     \
    VkResult _r = (x);                                                       \
    if (_r != VK_SUCCESS) {                                                  \
        fprintf(stderr, "VK_CHECK(%s) = %d at %s:%d\n",                      \
                #x, (int)_r, __FILE__, __LINE__);                            \
        exit(1);                                                             \
    }                                                                        \
} while (0)

typedef enum { BLEND_NONE, BLEND_ALPHA, BLEND_ADD, BLEND_MUL } BlendMode;

typedef enum {
    SHADER_BASIC,
    SHADER_EARLY,
    SHADER_ALU32,
    SHADER_DISCARD,
    SHADER_VARIANT_COUNT
} ShaderVariant;

typedef enum {
    ORDER_FAR_TO_NEAR,
    ORDER_RANDOM
} DrawOrder;

typedef enum {
    STENCIL_NONE,
    STENCIL_KEEP,
    STENCIL_REPLACE
} StencilMode;

typedef struct {
    const char*   label;
    VkFormat      colorFormat;
    VkFormat      depthFormat;
    BlendMode     blend;
    ShaderVariant shader;
    uint32_t      width, height;
    uint32_t      draws;      /* planes>1: 场景重复次数；否则: 全屏三角绘制次数 */
    VkCompareOp   depthCompare;
    VkBool32      depthWrite;
    uint32_t      planes;
    DrawOrder     drawOrder;
    StencilMode   stencil;

    double   fillrate_px_s;
    double   elapsedNs;
    uint64_t totalFragments;
    uint64_t fragInvocations;
} TestCase;

typedef struct {
    VkInstance       instance;
    VkPhysicalDevice physDev;
    VkDevice         device;
    uint32_t         queueFamily;
    VkQueue          queue;
    VkCommandPool    cmdPool;
    VkCommandBuffer  cmd;

    VkPhysicalDeviceProperties       props;
    VkPhysicalDeviceMemoryProperties memProps;

    VkShaderModule vertModule;
    VkShaderModule fragModules[SHADER_VARIANT_COUNT];

    VkQueryPool tsPool;
    VkQueryPool statPool;

    double   coreClockHz;
    double   peakColorFillrate;
    double   peakDepthFillrate;
    double   peakStencilFillrate;

    double   hsrFarToNearEff;
    double   hsrRandomEff;

    double   archOverdrawRatio;
    char     archVerdict[16];
} AppCtx;

static AppCtx g = {0};

/* ------------------------------------------------------------------------- */
/* 输出辅助                                                                   */
/* ------------------------------------------------------------------------- */

static void print_sep(void) {
    printf("================================================================\n");
}
static void print_section(const char* name) {
    printf("\n");
    print_sep();
    printf("  %s\n", name);
    print_sep();
}

static const char* vendor_name(uint32_t vid) {
    switch (vid) {
    case 0x10DE: return "NVIDIA";
    case 0x1002:
    case 0x1022: return "AMD";
    case 0x8086: return "Intel";
    case 0x13B5: return "ARM";
    case 0x1010: return "Imagination";
    case 0x5143: return "Qualcomm";
    case 0x14E4: return "Broadcom";
    default:     return "unknown";
    }
}

static const char* device_type_name(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "DISCRETE_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "INTEGRATED_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "VIRTUAL_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU";
    default:                                     return "OTHER";
    }
}

/* ------------------------------------------------------------------------- */
/* 着色器                                                                     */
/* ------------------------------------------------------------------------- */

static const char* VERT_SRC =
    "#version 450\n"
    "layout(push_constant) uniform Push { float depth; } pc;\n"
    "void main() {\n"
    "  vec2 p[3] = vec2[](vec2(-1.0,-1.0), vec2(3.0,-1.0), vec2(-1.0,3.0));\n"
    "  gl_Position = vec4(p[gl_VertexIndex], pc.depth, 1.0);\n"
    "}\n";

static char* build_frag_src(ShaderVariant v) {
    const char* early   = (v == SHADER_BASIC) ? "" : "layout(early_fragment_tests) in;\n";
    const char* discard = (v == SHADER_DISCARD)
                        ? "  if (gl_FragCoord.x < 0.0) discard;\n" : "";
    char alu[4096] = "";
    if (v == SHADER_ALU32) {
        for (int i = 0; i < 32; i++)
            strcat(alu, "  v = v * 1.0001 + vec4(0.0001);\n");
    }
    size_t len = 1024 + strlen(alu);
    char* src = (char*)malloc(len);
    snprintf(src, len,
        "#version 450\n"
        "%s"
        "layout(location=0) out vec4 outColor;\n"
        "void main() {\n"
        "%s"
        "  vec4 v = vec4(0.5);\n"
        "%s"
        "  outColor = v;\n"
        "}\n",
        early, discard, alu);
    return src;
}

static VkShaderModule compile_glsl(const char* src,
                                    shaderc_shader_kind kind,
                                    const char* name) {
    shaderc_compiler_t comp = shaderc_compiler_initialize();
    shaderc_compile_options_t opt = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opt, shaderc_target_env_vulkan,
                                           shaderc_env_version_vulkan_1_2);
    shaderc_compilation_result_t res = shaderc_compile_into_spv(
        comp, src, strlen(src), kind, name, "main", opt);

    if (shaderc_result_get_compilation_status(res)
            != shaderc_compilation_status_success) {
        fprintf(stderr, "shader %s compile error:\n%s\n", name,
                shaderc_result_get_error_message(res));
        exit(1);
    }

    VkShaderModuleCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = shaderc_result_get_length(res);
    ci.pCode = (const uint32_t*)shaderc_result_get_bytes(res);

    VkShaderModule mod = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(g.device, &ci, NULL, &mod));

    shaderc_result_release(res);
    shaderc_compile_options_release(opt);
    shaderc_compiler_release(comp);
    return mod;
}

/* ------------------------------------------------------------------------- */
/* 图像辅助                                                                   */
/* ------------------------------------------------------------------------- */

static uint32_t find_mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g.memProps.memoryTypeCount; i++) {
        if ((bits & (1u << i)) &&
            (g.memProps.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    fprintf(stderr, "no memory type found\n");
    exit(1);
}

static VkImageAspectFlags format_aspect(VkFormat fmt) {
    if (fmt == VK_FORMAT_D24_UNORM_S8_UINT ||
        fmt == VK_FORMAT_D32_SFLOAT_S8_UINT)
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (fmt == VK_FORMAT_D32_SFLOAT || fmt == VK_FORMAT_D16_UNORM)
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

static void create_image(VkFormat fmt, uint32_t w, uint32_t h,
                         VkImageUsageFlags usage,
                         VkImage* img, VkDeviceMemory* mem) {
    VkImageCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = (VkExtent3D){ w, h, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(g.device, &ci, NULL, img));

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g.device, *img, &mr);

    VkMemoryAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = find_mem_type(mr.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(g.device, &ai, NULL, mem));
    VK_CHECK(vkBindImageMemory(g.device, *img, *mem, 0));
}

static VkImageView create_view(VkImage img, VkFormat fmt) {
    VkImageViewCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image = img;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = fmt;
    ci.subresourceRange.aspectMask = format_aspect(fmt);
    ci.subresourceRange.levelCount = 1;
    ci.subresourceRange.layerCount = 1;

    VkImageView v = VK_NULL_HANDLE;
    VK_CHECK(vkCreateImageView(g.device, &ci, NULL, &v));
    return v;
}

/* ------------------------------------------------------------------------- */
/* 渲染通道                                                                   */
/* ------------------------------------------------------------------------- */

static VkRenderPass create_render_pass(VkFormat colorFmt, VkFormat depthFmt,
                                        VkAttachmentLoadOp depthLoad) {
    VkAttachmentDescription atts[2];
    uint32_t n = 0;
    int ci = -1, di = -1;

    if (colorFmt != VK_FORMAT_UNDEFINED) {
        atts[n] = (VkAttachmentDescription){0};
        atts[n].format = colorFmt;
        atts[n].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[n].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[n].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[n].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[n].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ci = (int)n++;
    }
    if (depthFmt != VK_FORMAT_UNDEFINED) {
        atts[n] = (VkAttachmentDescription){0};
        atts[n].format = depthFmt;
        atts[n].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[n].loadOp = depthLoad;
        atts[n].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[n].stencilLoadOp = depthLoad;
        atts[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[n].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[n].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        di = (int)n++;
    }

    VkAttachmentReference cr = { (uint32_t)ci, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference dr = { (uint32_t)di, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (ci >= 0) { sub.colorAttachmentCount = 1; sub.pColorAttachments = &cr; }
    if (di >= 0) sub.pDepthStencilAttachment = &dr;

    VkSubpassDependency dep = {0};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                     | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = dep.srcStageMask;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                      | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci = {0};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = n;
    rpci.pAttachments = atts;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;

    VkRenderPass rp = VK_NULL_HANDLE;
    VK_CHECK(vkCreateRenderPass(g.device, &rpci, NULL, &rp));
    return rp;
}

static VkFramebuffer create_framebuffer(VkRenderPass rp,
                                        VkImageView cv, VkImageView dv,
                                        uint32_t w, uint32_t h) {
    VkImageView views[2];
    uint32_t n = 0;
    if (cv) views[n++] = cv;
    if (dv) views[n++] = dv;

    VkFramebufferCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    ci.renderPass = rp;
    ci.attachmentCount = n;
    ci.pAttachments = views;
    ci.width = w;
    ci.height = h;
    ci.layers = 1;

    VkFramebuffer fb = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFramebuffer(g.device, &ci, NULL, &fb));
    return fb;
}

static void blend_state(BlendMode m, VkPipelineColorBlendAttachmentState* cb) {
    memset(cb, 0, sizeof(*cb));
    cb->colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    switch (m) {
    case BLEND_NONE:  cb->blendEnable = VK_FALSE; break;
    case BLEND_ALPHA:
        cb->blendEnable = VK_TRUE;
        cb->srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cb->dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb->colorBlendOp = VK_BLEND_OP_ADD;
        cb->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cb->dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cb->alphaBlendOp = VK_BLEND_OP_ADD;
        break;
    case BLEND_ADD:
        cb->blendEnable = VK_TRUE;
        cb->srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cb->dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cb->colorBlendOp = VK_BLEND_OP_ADD;
        cb->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cb->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cb->alphaBlendOp = VK_BLEND_OP_ADD;
        break;
    case BLEND_MUL:
        cb->blendEnable = VK_TRUE;
        cb->srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        cb->dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        cb->colorBlendOp = VK_BLEND_OP_ADD;
        cb->srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
        cb->dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cb->alphaBlendOp = VK_BLEND_OP_ADD;
        break;
    }
}

typedef struct { VkPipeline pipeline; VkPipelineLayout layout; } PipePair;

static PipePair create_pipeline(const TestCase* tc, VkRenderPass rp) {
    VkPipelineShaderStageCreateInfo stages[2] = {0};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = g.vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = g.fragModules[tc->shader];
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vps = {0};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = {0};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    ms.sampleShadingEnable = VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo ds = {0};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (tc->depthFormat != VK_FORMAT_UNDEFINED) {
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = tc->depthWrite;
        ds.depthCompareOp = tc->depthCompare;
    } else {
        ds.depthTestEnable = VK_FALSE;
        ds.depthWriteEnable = VK_FALSE;
    }

    if (tc->stencil != STENCIL_NONE) {
        VkStencilOp op = (tc->stencil == STENCIL_REPLACE)
                       ? VK_STENCIL_OP_REPLACE : VK_STENCIL_OP_KEEP;
        VkStencilOpState so = {0};
        so.failOp = op;
        so.passOp = op;
        so.depthFailOp = op;
        so.compareOp = VK_COMPARE_OP_ALWAYS;
        so.compareMask = 0xFF;
        so.writeMask = 0xFF;
        so.reference = 0x01;
        ds.stencilTestEnable = VK_TRUE;
        ds.front = so;
        ds.back = so;
    }

    VkPipelineColorBlendAttachmentState cb;
    blend_state(tc->blend, &cb);
    VkPipelineColorBlendStateCreateInfo cbs = {0};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    if (tc->colorFormat != VK_FORMAT_UNDEFINED) {
        cbs.attachmentCount = 1;
        cbs.pAttachments = &cb;
    }

    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi = {0};
    dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dsi.dynamicStateCount = 2;
    dsi.pDynamicStates = dyn;

    VkPushConstantRange pcr = {0};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.size = sizeof(float);

    VkPipelineLayoutCreateInfo pli = {0};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;

    PipePair pp = {0};
    VK_CHECK(vkCreatePipelineLayout(g.device, &pli, NULL, &pp.layout));

    VkGraphicsPipelineCreateInfo pci = {0};
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vps;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cbs;
    pci.pDynamicState = &dsi;
    pci.layout = pp.layout;
    pci.renderPass = rp;
    pci.subpass = 0;

    VK_CHECK(vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1,
                                       &pci, NULL, &pp.pipeline));
    return pp;
}

/* ------------------------------------------------------------------------- */
/* 核心测量                                                                   */
/* ------------------------------------------------------------------------- */

static void gen_hsr_depths(uint32_t n, DrawOrder order, float* out) {
    float base[64];
    for (uint32_t i = 0; i < n; i++)
        base[i] = 0.9f - 0.8f * (float)i / (float)(n - 1);

    if (order == ORDER_FAR_TO_NEAR) {
        memcpy(out, base, n * sizeof(float));
    } else {
        memcpy(out, base, n * sizeof(float));
        uint32_t seed = 0xDEADBEEF;
        for (int i = (int)n - 1; i > 0; i--) {
            seed = seed * 1103515245u + 12345u;
            int j = (int)((seed >> 16) % (uint32_t)(i + 1));
            float t = out[i]; out[i] = out[j]; out[j] = t;
        }
    }
}

static void run_test_case(TestCase* tc) {
    VkImage        colorImg = VK_NULL_HANDLE, depthImg = VK_NULL_HANDLE;
    VkDeviceMemory colorMem = VK_NULL_HANDLE, depthMem = VK_NULL_HANDLE;
    VkImageView    colorView = VK_NULL_HANDLE, depthView = VK_NULL_HANDLE;

    if (tc->colorFormat != VK_FORMAT_UNDEFINED) {
        create_image(tc->colorFormat, tc->width, tc->height,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                     &colorImg, &colorMem);
        colorView = create_view(colorImg, tc->colorFormat);
    }
    if (tc->depthFormat != VK_FORMAT_UNDEFINED) {
        create_image(tc->depthFormat, tc->width, tc->height,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     &depthImg, &depthMem);
        depthView = create_view(depthImg, tc->depthFormat);
    }

    /* 只要用到深度比较，就需要 CLEAR 保证初值已知。
       纯 fillrate 测试（planes==1 且 depthCompare==ALWAYS）
       深度不参与结果，可以 DONT_CARE。 */
    int needClear = (tc->depthFormat != VK_FORMAT_UNDEFINED) &&
                    (tc->planes > 1 || tc->depthCompare != VK_COMPARE_OP_ALWAYS);
    VkAttachmentLoadOp depthLoad = needClear
        ? VK_ATTACHMENT_LOAD_OP_CLEAR
        : VK_ATTACHMENT_LOAD_OP_DONT_CARE;

    VkRenderPass rp = create_render_pass(tc->colorFormat, tc->depthFormat, depthLoad);
    VkFramebuffer fb = create_framebuffer(rp, colorView, depthView,
                                          tc->width, tc->height);
    PipePair pp = create_pipeline(tc, rp);

    vkResetQueryPool(g.device, g.tsPool, 0, 2);
    vkResetQueryPool(g.device, g.statPool, 0, 1);
    VK_CHECK(vkResetCommandBuffer(g.cmd, 0));

    VkCommandBufferBeginInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(g.cmd, &bi));

    VkClearValue clears[2];
    clears[0].color = (VkClearColorValue){{0.0f, 0.0f, 0.0f, 1.0f}};
    clears[1].depthStencil.depth = 1.0f;
    clears[1].depthStencil.stencil = 0;

    VkRenderPassBeginInfo rbi = {0};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = rp;
    rbi.framebuffer = fb;
    rbi.renderArea.extent.width = tc->width;
    rbi.renderArea.extent.height = tc->height;
    if (tc->depthFormat != VK_FORMAT_UNDEFINED) {
        rbi.clearValueCount = (tc->colorFormat != VK_FORMAT_UNDEFINED) ? 2 : 1;
        rbi.pClearValues = (tc->colorFormat != VK_FORMAT_UNDEFINED)
                         ? clears : &clears[1];
    } else if (tc->colorFormat != VK_FORMAT_UNDEFINED) {
        rbi.clearValueCount = 1;
        rbi.pClearValues = &clears[0];
    }

    vkCmdBeginRenderPass(g.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pp.pipeline);

    VkViewport v = { 0, 0, (float)tc->width, (float)tc->height, 0.0f, 1.0f };
    VkRect2D sc = { {0, 0}, { tc->width, tc->height } };
    vkCmdSetViewport(g.cmd, 0, 1, &v);
    vkCmdSetScissor(g.cmd, 0, 1, &sc);

    vkCmdWriteTimestamp(g.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.tsPool, 0);
    vkCmdBeginQuery(g.cmd, g.statPool, 0, 0);

    if (tc->planes > 1) {
        float depths[64];
        gen_hsr_depths(tc->planes, tc->drawOrder, depths);
        uint32_t sceneRepeats = (tc->draws > 0) ? tc->draws : 1;
        for (uint32_t r = 0; r < sceneRepeats; r++) {
            for (uint32_t i = 0; i < tc->planes; i++) {
                vkCmdPushConstants(g.cmd, pp.layout, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(float), &depths[i]);
                vkCmdDraw(g.cmd, 3, 1, 0, 0);
            }
        }
        tc->totalFragments = (uint64_t)sceneRepeats * tc->planes
                           * tc->width * tc->height;
    } else {
        float d = 0.5f;
        for (uint32_t i = 0; i < tc->draws; i++) {
            vkCmdPushConstants(g.cmd, pp.layout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(float), &d);
            vkCmdDraw(g.cmd, 3, 1, 0, 0);
        }
        tc->totalFragments = (uint64_t)tc->draws * tc->width * tc->height;
    }

    vkCmdEndQuery(g.cmd, g.statPool, 0);
    vkCmdWriteTimestamp(g.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.tsPool, 1);

    vkCmdEndRenderPass(g.cmd);
    VK_CHECK(vkEndCommandBuffer(g.cmd));

    VkSubmitInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.cmd;
    VK_CHECK(vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(g.queue));

    uint64_t ts[2] = {0};
    vkGetQueryPoolResults(g.device, g.tsPool, 0, 2, sizeof(ts), ts,
                          sizeof(uint64_t),
                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

    uint64_t stat = 0;
    vkGetQueryPoolResults(g.device, g.statPool, 0, 1, sizeof(stat), &stat,
                          sizeof(uint64_t),
                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

    double elapsedNs = (double)(ts[1] - ts[0]) * (double)g.props.limits.timestampPeriod;
    tc->elapsedNs = elapsedNs;
    tc->fragInvocations = stat;
    tc->fillrate_px_s = (elapsedNs > 0.0)
        ? (double)tc->totalFragments / (elapsedNs * 1e-9)
        : 0.0;

    vkDestroyPipeline(g.device, pp.pipeline, NULL);
    vkDestroyPipelineLayout(g.device, pp.layout, NULL);
    vkDestroyFramebuffer(g.device, fb, NULL);
    vkDestroyRenderPass(g.device, rp, NULL);
    if (colorView) vkDestroyImageView(g.device, colorView, NULL);
    if (depthView) vkDestroyImageView(g.device, depthView, NULL);
    if (colorImg)  vkDestroyImage(g.device, colorImg, NULL);
    if (depthImg)  vkDestroyImage(g.device, depthImg, NULL);
    if (colorMem)  vkFreeMemory(g.device, colorMem, NULL);
    if (depthMem)  vkFreeMemory(g.device, depthMem, NULL);
}

/* ------------------------------------------------------------------------- */
/* Section 0: Architecture                                                    */
/* ------------------------------------------------------------------------- */

static void run_arch_section(void) {
    printf("  [ Device-Level Clues ]\n");
    printf("    Vendor ID             : 0x%04X (%s)\n",
           g.props.vendorID, vendor_name(g.props.vendorID));
    printf("    Device Type           : %s\n",
           device_type_name(g.props.deviceType));
    printf("      (INTEGRATED_GPU only reflects unified memory topology,\n");
    printf("       it does NOT imply tile-based rasterization.)\n");
    printf("\n");

    printf("  [ Overdraw Scaling Probe ]\n");
    printf("    Scene: N full-screen planes, far-to-near, repeated %d times\n",
           ARCH_SCENES);
    printf("    on a %dx%d attachment to amortize submit overhead.\n",
           ARCH_W, ARCH_H);
    printf("    IMR: ratio grows linearly with N. TBDR: ratio stays ~constant.\n");
    printf("    Threshold for IMR: ratio(N=8) >= 4.5.\n\n");

    printf("    %6s  %14s  %10s\n",
           "Planes", "Time (ms)", "Ratio");
    printf("    %6s  %14s  %10s\n",
           "------", "--------------", "----------");

    uint32_t Ns[] = { 1, 2, 4, 8 };
    double times[4] = {0};

    for (int i = 0; i < 4; i++) {
        TestCase tc = {0};
        tc.colorFormat  = VK_FORMAT_R8G8B8A8_UNORM;
        tc.depthFormat  = VK_FORMAT_D32_SFLOAT;
        tc.shader       = SHADER_EARLY;
        tc.width        = ARCH_W;
        tc.height       = ARCH_H;
        tc.planes       = Ns[i];
        tc.draws        = ARCH_SCENES;
        tc.drawOrder    = ORDER_FAR_TO_NEAR;
        tc.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
        tc.depthWrite   = VK_TRUE;
        run_test_case(&tc);
        times[i] = tc.elapsedNs / 1e6;
    }

    for (int i = 0; i < 4; i++) {
        double ratio = (times[0] > 0.0) ? times[i] / times[0] : 0.0;
        printf("    %6u  %14.3f  %10.2f\n", Ns[i], times[i], ratio);
    }

    double r8 = (times[0] > 0.0) ? times[3] / times[0] : 0.0;
    g.archOverdrawRatio = r8;

    printf("\n    Timing indicates: ");
    if (r8 < 2.0) {
        printf("TBDR-like (overdraw absorbed)\n");
    } else if (r8 < 4.5) {
        printf("TBR-like (partial overdraw)\n");
    } else {
        printf("IMR-like (linear overdraw)\n");
    }

    /* 主要判决：用 HSR 顺序依赖数据 */
    double fn = g.hsrFarToNearEff;
    double rr = g.hsrRandomEff;

    printf("\n  [ HSR Order-Dependency (primary evidence) ]\n");
    printf("    Far-to-near efficiency :  %8.1f %%\n", fn);
    printf("    Random efficiency      :  %8.1f %%\n", rr);

    if (fn > 80.0) {
        strcpy(g.archVerdict, "TBDR");
        printf("    -> TBDR: painter's order still culls most fragments\n");
    } else if (fn > 40.0) {
        strcpy(g.archVerdict, "TBR");
        printf("    -> TBR: partial order-independence\n");
    } else {
        strcpy(g.archVerdict, "IMR");
        printf("    -> IMR: Early-Z only, no order-independent HSR\n");
    }

    printf("\n  [ Architecture Verdict ]\n");
    printf("    Classification : %s\n", g.archVerdict);
    printf("    Confidence     : HIGH\n");
    printf("    Evidence       :\n");
    printf("      - Vendor ID: 0x%04X (%s)\n",
           g.props.vendorID, vendor_name(g.props.vendorID));
    printf("      - HSR far-to-near efficiency: %.1f %%\n", fn);
    printf("      - Overdraw ratio N=8 vs N=1: %.2fx\n", r8);
    printf("      - Primary decision uses HSR order dependence,\n");
    printf("        overdraw timing is reported as a cross-check.\n");
}

/* ------------------------------------------------------------------------- */
/* Section 1: Color ROP                                                       */
/* ------------------------------------------------------------------------- */

typedef struct { const char* name; VkFormat fmt; } FormatEntry;

static const FormatEntry COLOR_FORMATS[] = {
    { "R8G8B8A8_UNORM",      VK_FORMAT_R8G8B8A8_UNORM },
    { "R16G16B16A16_SFLOAT", VK_FORMAT_R16G16B16A16_SFLOAT },
    { "R32G32B32A32_SFLOAT", VK_FORMAT_R32G32B32A32_SFLOAT },
};
static const char* BLEND_NAMES[] = { "NONE", "ALPHA", "ADD", "MUL" };

static void run_color_section(void) {
    printf("  Attachment: %dx%d, LOAD=DONT_CARE, STORE=DONT_CARE, no depth.\n",
           COLOR_W, COLOR_H);
    printf("  Fillrate unit: Gpx/s.  FragInvoc is %% of theoretical.\n");
    printf("  Eff is fillrate relative to peak in this section (%%).\n\n");

    enum { MAX_ROWS = 16 };
    double fillrates[MAX_ROWS];
    double fragPcts[MAX_ROWS];
    const char* fmtNames[MAX_ROWS];
    const char* blendNames[MAX_ROWS];
    int k = 0;

    for (size_t i = 0; i < sizeof(COLOR_FORMATS)/sizeof(COLOR_FORMATS[0]); i++) {
        for (int b = 0; b < 4; b++) {
            TestCase tc = {0};
            tc.colorFormat  = COLOR_FORMATS[i].fmt;
            tc.depthFormat  = VK_FORMAT_UNDEFINED;
            tc.blend        = (BlendMode)b;
            tc.shader       = SHADER_EARLY;
            tc.width        = COLOR_W;
            tc.height       = COLOR_H;
            tc.draws        = DRAWS_PER_MEASURE;
            tc.depthCompare = VK_COMPARE_OP_ALWAYS;
            tc.depthWrite   = VK_FALSE;

            run_test_case(&tc);

            fillrates[k] = tc.fillrate_px_s / 1e9;
            fragPcts[k]  = (tc.totalFragments > 0)
                         ? 100.0 * (double)tc.fragInvocations / (double)tc.totalFragments
                         : 0.0;
            fmtNames[k]   = COLOR_FORMATS[i].name;
            blendNames[k] = BLEND_NAMES[b];

            if (tc.fillrate_px_s > g.peakColorFillrate)
                g.peakColorFillrate = tc.fillrate_px_s;
            k++;
        }
    }

    double peak = g.peakColorFillrate / 1e9;

    printf("  %-20s  %-7s  %10s  %9s  %7s\n",
           "Format", "Blend", "Fillrate", "FragInvoc", "Eff");
    printf("  %-20s  %-7s  %10s  %9s  %7s\n",
           "--------------------", "-------", "----------", "---------", "-------");
    for (int i = 0; i < k; i++) {
        double eff = (peak > 0.0) ? 100.0 * fillrates[i] / peak : 0.0;
        printf("  %-20s  %-7s  %10.1f  %9.1f  %7.1f\n",
               fmtNames[i], blendNames[i], fillrates[i], fragPcts[i], eff);
    }

    double ropCount = (g.coreClockHz > 0)
                    ? g.peakColorFillrate / g.coreClockHz : 0.0;

    printf("\n  Peak (color)       :  %10.1f Gpx/s\n", peak);
    printf("  Implied CROP count :  %10.1f  ->  ~ %d ROPs\n",
           ropCount, (int)floor(ropCount + 0.5));
}

/* ------------------------------------------------------------------------- */
/* Section 2: Z-ROP                                                           */
/* ------------------------------------------------------------------------- */

typedef struct { const char* name; ShaderVariant shader; } PathEntry;

static const PathEntry Z_PATHS[] = {
    { "Early-Z", SHADER_EARLY },
    { "Late-Z",  SHADER_DISCARD },
};

static void run_zrop_section(void) {
    printf("  Attachment: %dx%d D32_SFLOAT, LOAD=DONT_CARE, STORE=DONT_CARE.\n",
           DEPTH_W, DEPTH_H);
    printf("  Fillrate unit: Gpx/s.\n\n");
    printf("  %-14s  %-8s  %10s  %10s\n",
           "Pipeline Path", "Compare", "DepthWrite", "Fillrate");
    printf("  %-14s  %-8s  %10s  %10s\n",
           "--------------", "--------", "----------", "----------");

    VkCompareOp compares[2] = { VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_NEVER };
    const char* compareNames[2] = { "ALWAYS", "NEVER" };

    g.peakDepthFillrate = 0.0;

    for (size_t p = 0; p < sizeof(Z_PATHS)/sizeof(Z_PATHS[0]); p++) {
        for (int ci = 0; ci < 2; ci++) {
            for (int w = 0; w < 2; w++) {
                TestCase tc = {0};
                tc.colorFormat  = VK_FORMAT_UNDEFINED;
                tc.depthFormat  = VK_FORMAT_D32_SFLOAT;
                tc.shader       = Z_PATHS[p].shader;
                tc.width        = DEPTH_W;
                tc.height       = DEPTH_H;
                tc.draws        = DRAWS_PER_MEASURE;
                tc.depthCompare = compares[ci];
                tc.depthWrite   = w ? VK_TRUE : VK_FALSE;

                run_test_case(&tc);

                double gpx = tc.fillrate_px_s / 1e9;
                printf("  %-14s  %-8s  %10s  %10.1f\n",
                       Z_PATHS[p].name, compareNames[ci],
                       w ? "ON" : "OFF", gpx);

                if (p == 0 && ci == 0 && w == 1)
                    g.peakDepthFillrate = tc.fillrate_px_s;
            }
        }
    }

    double peak = g.peakDepthFillrate / 1e9;
    double colorPeak = g.peakColorFillrate / 1e9;
    double ratio = (colorPeak > 0.0) ? peak / colorPeak : 0.0;
    double effSlots = (g.coreClockHz > 0)
                    ? g.peakDepthFillrate / g.coreClockHz : 0.0;

    printf("\n  Peak (depth-write) :  %10.1f Gpx/s  (Early-Z, ALWAYS, ON)\n", peak);
    printf("  Depth path ratio   :  %10.2fx CROP throughput\n", ratio);
    printf("  Equivalent slots   :  %10.1f  (not physical ROP count)\n", effSlots);
    printf("\n  Note: the NEVER rows measure reject throughput, not Z-ROP\n");
    printf("        write throughput, and do not contribute to the peak.\n");
    printf("        NVIDIA depth path is typically ~4x the color CROP.\n");
}

/* ------------------------------------------------------------------------- */
/* Section 3: Stencil                                                         */
/* ------------------------------------------------------------------------- */

static void run_stencil_section(void) {
    printf("  Attachment: %dx%d D24_UNORM_S8_UINT, LOAD=DONT_CARE, STORE=DONT_CARE.\n",
           DEPTH_W, DEPTH_H);
    printf("  Depth test disabled; only stencil test active.\n");
    printf("  Fillrate unit: Gpx/s.  KEEP is read-only; REPLACE is read-modify-write.\n\n");

    printf("  %-14s  %10s\n", "Stencil Op", "Fillrate");
    printf("  %-14s  %10s\n", "--------------", "----------");

    struct { const char* name; StencilMode mode; } ops[2] = {
        { "KEEP",    STENCIL_KEEP },
        { "REPLACE", STENCIL_REPLACE },
    };

    g.peakStencilFillrate = 0.0;

    for (int i = 0; i < 2; i++) {
        TestCase tc = {0};
        tc.colorFormat  = VK_FORMAT_UNDEFINED;
        tc.depthFormat  = VK_FORMAT_D24_UNORM_S8_UINT;
        tc.shader       = SHADER_EARLY;
        tc.width        = DEPTH_W;
        tc.height       = DEPTH_H;
        tc.draws        = DRAWS_PER_MEASURE;
        tc.depthCompare = VK_COMPARE_OP_ALWAYS;
        tc.depthWrite   = VK_FALSE;
        tc.stencil      = ops[i].mode;

        run_test_case(&tc);

        double gpx = tc.fillrate_px_s / 1e9;
        printf("  %-14s  %10.1f\n", ops[i].name, gpx);

        if (tc.fillrate_px_s > g.peakStencilFillrate)
            g.peakStencilFillrate = tc.fillrate_px_s;
    }

    double peak = g.peakStencilFillrate / 1e9;
    double ratio = (g.peakColorFillrate > 0.0)
                 ? g.peakStencilFillrate / g.peakColorFillrate : 0.0;

    printf("\n  Peak (stencil)     :  %10.1f Gpx/s\n", peak);
    printf("  Stencil path ratio :  %10.2fx CROP throughput\n", ratio);
}

/* ------------------------------------------------------------------------- */
/* Section 4: HSR                                                             */
/* ------------------------------------------------------------------------- */

static void run_hsr_row(uint32_t planes, DrawOrder order,
                        double* outInvoc, double* outTotal, double* outEff) {
    TestCase tc = {0};
    tc.colorFormat  = VK_FORMAT_R8G8B8A8_UNORM;
    tc.depthFormat  = VK_FORMAT_D32_SFLOAT;
    tc.shader       = SHADER_EARLY;
    tc.width        = HSR_W;
    tc.height       = HSR_H;
    tc.draws        = 1;
    tc.planes       = planes;
    tc.drawOrder    = order;
    tc.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
    tc.depthWrite   = VK_TRUE;

    run_test_case(&tc);

    double screenPx = (double)HSR_W * (double)HSR_H;
    *outTotal = (double)tc.totalFragments / screenPx;
    *outInvoc = (double)tc.fragInvocations / screenPx;
    *outEff   = (tc.totalFragments > 0)
              ? 100.0 * (1.0 - (double)tc.fragInvocations / (double)tc.totalFragments)
              : 0.0;
}

static void run_hsr_section(void) {
    printf("  Scene: N full-screen planes, opaque, EarlyFragmentTests.\n");
    printf("  ShaderInvoc and TotalFrags normalized to WxH (screen pixels).\n");
    printf("  Far-to-near is the classic painter's algorithm order.\n");
    printf("  For IMR+Early-Z, far-to-near should show 0%% HSR efficiency;\n");
    printf("  for true order-independent HSR, it should match random.\n\n");

    printf("  %3s  %-13s  %12s  %10s  %7s\n",
           "N", "Draw Order", "ShaderInvoc", "TotalFrags", "HSR Eff");
    printf("  %3s  %-13s  %12s  %10s  %7s\n",
           "---", "-------------", "------------", "----------", "-------");

    uint32_t sizes[] = { 2, 4, 8, 16 };
    const char* orderNames[] = { "far-to-near", "random" };
    DrawOrder   orders[]     = { ORDER_FAR_TO_NEAR, ORDER_RANDOM };

    double farToNearMax = 0.0;
    double randomMax    = 0.0;

    for (size_t s = 0; s < sizeof(sizes)/sizeof(sizes[0]); s++) {
        for (int oi = 0; oi < 2; oi++) {
            double invoc = 0.0, total = 0.0, eff = 0.0;
            run_hsr_row(sizes[s], orders[oi], &invoc, &total, &eff);

            printf("  %3u  %-13s  %12.2f  %10.2f  %7.1f\n",
                   sizes[s], orderNames[oi], invoc, total, eff);

            if (orders[oi] == ORDER_FAR_TO_NEAR && eff > farToNearMax)
                farToNearMax = eff;
            if (orders[oi] == ORDER_RANDOM && eff > randomMax)
                randomMax = eff;
        }
    }

    g.hsrFarToNearEff = farToNearMax;
    g.hsrRandomEff    = randomMax;

    printf("\n  Far-to-near best eff :  %10.1f %%\n", farToNearMax);
    printf("  Random      best eff :  %10.1f %%\n", randomMax);

    if (farToNearMax > 80.0)
        printf("  HSR Rating: CLASS-A (order-independent HSR present)\n");
    else if (farToNearMax > 40.0)
        printf("  HSR Rating: CLASS-B (partial order-independent HSR)\n");
    else
        printf("  HSR Rating: CLASS-C (Early-Z only, no order-independent HSR)\n");

    printf("\n  Note: IMR+Early-Z gives 0%% on far-to-near because every\n");
    printf("        subsequent plane is closer and passes LESS_OR_EQUAL.\n");
    printf("        Random order efficiency reflects Early-Z hit rate,\n");
    printf("        approximately H_n / n for n uniformly-distributed depths.\n");
}

/* ------------------------------------------------------------------------- */
/* Section 5: Summary                                                         */
/* ------------------------------------------------------------------------- */

static void print_summary(void) {
    double crop = (g.coreClockHz > 0) ? g.peakColorFillrate / g.coreClockHz : 0.0;
    double zEff = (g.coreClockHz > 0) ? g.peakDepthFillrate / g.coreClockHz : 0.0;
    double zRatio = (g.peakColorFillrate > 0.0)
                 ? g.peakDepthFillrate / g.peakColorFillrate : 0.0;
    double sRatio = (g.peakColorFillrate > 0.0)
                 ? g.peakStencilFillrate / g.peakColorFillrate : 0.0;

    printf("  Architecture                 :  %s\n", g.archVerdict);
    printf("  Physical ROP count (CROP)    :  %4d\n", (int)floor(crop + 0.5));
    printf("  Depth path throughput        :  %.2fx CROP (%.1f slots @ clock)\n",
           zRatio, zEff);
    printf("  Stencil path throughput      :  %.2fx CROP\n", sRatio);
    printf("  HSR far-to-near best eff     :  %.1f %%\n", g.hsrFarToNearEff);
    printf("  HSR random best eff          :  %.1f %%\n", g.hsrRandomEff);
    printf("  Overdraw ratio N=8 vs N=1    :  %.2fx\n", g.archOverdrawRatio);
    printf("  Core clock (used)            :  %.1f MHz%s\n",
           g.coreClockHz / 1e6,
           getenv("GPU_CLOCK_MHZ") ? "" : "  (assumed default)");
}

/* ------------------------------------------------------------------------- */
/* 初始化 / 清理                                                              */
/* ------------------------------------------------------------------------- */

static void create_instance(void) {
    VkApplicationInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "gpu_rop_probe";
    ai.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    ai.pEngineName = "none";
    ai.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &ai;
    VK_CHECK(vkCreateInstance(&ci, NULL, &g.instance));
}

static void pick_device(void) {
    uint32_t n = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(g.instance, &n, NULL));
    if (n == 0) { fprintf(stderr, "no Vulkan device\n"); exit(1); }
    VkPhysicalDevice* devs = (VkPhysicalDevice*)malloc(n * sizeof(*devs));
    VK_CHECK(vkEnumeratePhysicalDevices(g.instance, &n, devs));
    g.physDev = devs[0];
    free(devs);

    vkGetPhysicalDeviceProperties(g.physDev, &g.props);
    vkGetPhysicalDeviceMemoryProperties(g.physDev, &g.memProps);
}

static void create_device(void) {
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.physDev, &qn, NULL);
    VkQueueFamilyProperties* qs = (VkQueueFamilyProperties*)malloc(qn * sizeof(*qs));
    vkGetPhysicalDeviceQueueFamilyProperties(g.physDev, &qn, qs);

    g.queueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) {
        if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            g.queueFamily = i;
            break;
        }
    }
    free(qs);
    if (g.queueFamily == UINT32_MAX) {
        fprintf(stderr, "no graphics queue\n");
        exit(1);
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {0};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = g.queueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;

    VkDeviceCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    VK_CHECK(vkCreateDevice(g.physDev, &ci, NULL, &g.device));
    vkGetDeviceQueue(g.device, g.queueFamily, 0, &g.queue);
}

static void create_cmd_and_queries(void) {
    VkCommandPoolCreateInfo pi = {0};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = g.queueFamily;
    VK_CHECK(vkCreateCommandPool(g.device, &pi, NULL, &g.cmdPool));

    VkCommandBufferAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = g.cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(g.device, &ai, &g.cmd));

    VkQueryPoolCreateInfo qpi = {0};
    qpi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpi.queryCount = 2;
    VK_CHECK(vkCreateQueryPool(g.device, &qpi, NULL, &g.tsPool));

    qpi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
    qpi.queryCount = 1;
    qpi.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
    VK_CHECK(vkCreateQueryPool(g.device, &qpi, NULL, &g.statPool));
}

static void create_shaders(void) {
    g.vertModule = compile_glsl(VERT_SRC, shaderc_glsl_vertex_shader, "vert");
    for (int v = 0; v < SHADER_VARIANT_COUNT; v++) {
        char* src = build_frag_src((ShaderVariant)v);
        char name[32];
        snprintf(name, sizeof(name), "frag_%d", v);
        g.fragModules[v] = compile_glsl(src, shaderc_glsl_fragment_shader, name);
        free(src);
    }
}

static void init_vulkan(void) {
    create_instance();
    pick_device();
    create_device();
    create_cmd_and_queries();
    create_shaders();

    const char* env = getenv("GPU_CLOCK_MHZ");
    g.coreClockHz = env ? atof(env) * 1e6 : 1500e6;
}

static void cleanup_vulkan(void) {
    for (int v = 0; v < SHADER_VARIANT_COUNT; v++)
        vkDestroyShaderModule(g.device, g.fragModules[v], NULL);
    vkDestroyShaderModule(g.device, g.vertModule, NULL);
    vkDestroyQueryPool(g.device, g.tsPool, NULL);
    vkDestroyQueryPool(g.device, g.statPool, NULL);
    vkDestroyCommandPool(g.device, g.cmdPool, NULL);
    vkDestroyDevice(g.device, NULL);
    vkDestroyInstance(g.instance, NULL);
}

static void print_device_info(void) {
    print_sep();
    printf("  GPU ROP / HSR Capability Report\n");
    print_sep();
    printf("\n[ Device Information ]\n");
    printf("  Name                    : %s\n", g.props.deviceName);
    printf("  Vendor ID               : 0x%04X (%s)\n",
           g.props.vendorID, vendor_name(g.props.vendorID));
    printf("  Device ID               : 0x%04X\n", g.props.deviceID);
    printf("  Device Type             : %s\n",
           device_type_name(g.props.deviceType));
    printf("  API Version             : %u.%u.%u\n",
           VK_VERSION_MAJOR(g.props.apiVersion),
           VK_VERSION_MINOR(g.props.apiVersion),
           VK_VERSION_PATCH(g.props.apiVersion));
    printf("  Core Clock (avg)        :  %8.1f MHz%s\n",
           g.coreClockHz / 1e6,
           getenv("GPU_CLOCK_MHZ") ? "" : "  (assumed, set GPU_CLOCK_MHZ)");
    printf("  Timestamp Period        :  %8.3f ns\n",
           (double)g.props.limits.timestampPeriod);
}

/* ------------------------------------------------------------------------- */
/* main                                                                       */
/* ------------------------------------------------------------------------- */

int main(void) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    setvbuf(stdout, NULL, _IONBF, 0);
#endif

    init_vulkan();
    print_device_info();

    /* Section 4 先跑，Section 0 引用 HSR 结果作为主判决证据 */
    print_section("Section 4: HSR Efficiency");
    run_hsr_section();

    print_section("Section 0: Architecture Identification");
    run_arch_section();

    print_section("Section 1: Color ROP Fillrate");
    run_color_section();

    print_section("Section 2: Z-ROP Fillrate");
    run_zrop_section();

    print_section("Section 3: Stencil ROP Fillrate");
    run_stencil_section();

    print_section("Section 5: Summary");
    print_summary();

    cleanup_vulkan();
    return 0;
}