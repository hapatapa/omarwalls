// shader-host: pure Vulkan + wayland layer-shell background renderer for
// Shadertoy-style GLSL wallpapers. No Qt involved (Vulkan-based, per directive).
//
// Usage: shader-host <wallpaper.json> <state-file>
//   - reads wallpaper.json for "entry" (relative shader path)
//   - optional "renderScale" in wallpaper.json (0.1..1.0) renders the shader at
//     a lower resolution and upscales it with wp_viewporter; a global override
//     exists in ~/.local/state/omarchy-webwall/render-scale
//   - creates a layer-shell BACKGROUND surface on the compositor
//   - renders a fullscreen triangle with the compiled shader via Vulkan
//   - watches the shader file + Omarchy colors.toml for live reload
//   - uniforms exposed to GLSL: iResolution, iTime, iFrame, iMouse, iDate, u_theme[]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <map>
#include <cmath>
#include <algorithm>

#include <wayland-client.h>
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#include "viewporter-client-protocol.h"
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>
#include <shaderc/shaderc.h>

#define VKOK(e) do { VkResult _r = (e); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "SHADERHOST: vk error %d at %s:%d\n", (int)_r, __FILE__, __LINE__); exit(1); } } while (0)

static void die(const char *m) { fprintf(stderr, "SHADERHOST: %s\n", m); exit(1); }

// ---------------------------------------------------------------------------
// UBO layout (must match fragment shader block)
//   layout(set=0, binding=0) uniform Uniforms {
//     vec3 iResolution; float _pad0; float iTime; float iFrame;
//     vec4 iMouse; vec4 iDate; vec4 u_theme[24];
//   };
// ---------------------------------------------------------------------------
struct UBO {                       // std140 layout
    float iResolution[3];          // vec3,   offset 0
    float _pad0;                   //         offset 12 (helps alignment)
    float iTime;                   //         offset 16
    float iFrame;                  //         offset 20
    float _pad1[2];                // align iMouse vec4 to 32 (24->32)
    float iMouse[4];               // vec4,   offset 32
    float iDate[4];                // vec4,   offset 48
    float u_theme[24][4];          // vec4 x24, offset 64 .. 448
};

// ---------------------------------------------------------------------------
// globals
// ---------------------------------------------------------------------------
static std::string g_statePath, g_manifestPath, g_entryPath, g_mouseFile;
static double g_mouseX = 0, g_mouseY = 0;

// wayland
static struct wl_display *g_display;
static struct wl_registry *g_registry;
static struct wl_compositor *g_compositor;
static struct zwlr_layer_shell_v1 *g_layerShell;
static struct wp_viewporter *g_viewporter;
static struct wp_viewport *g_viewport;
static struct wl_surface *g_wlSurface;
static struct zwlr_layer_surface_v1 *g_layerSurface;
static int g_w = 0, g_h = 0;                 // layer (surface) size
static int g_rw = 0, g_rh = 0;               // render (buffer) size, <= g_w/g_h
static double g_renderScale = 1.0;           // 0.1 .. 1.0
static uint32_t g_serial = 0;
static int g_resizePend = 0;

// vulkan
static VkInstance g_inst;
static VkPhysicalDevice g_pdev;
static VkDevice g_dev;
static uint32_t g_qfam = VK_QUEUE_FAMILY_IGNORED;
static VkQueue g_q;
static VkSurfaceKHR g_surface;
static VkSwapchainKHR g_swapchain;
static VkFormat g_fmt = VK_FORMAT_UNDEFINED;
static VkRenderPass g_pass;
static VkPipelineLayout g_plLayout, g_plLayoutNew;
static VkPipeline g_pipeline, g_pipelineNew;
static VkDescriptorPool g_dpool;
static VkDescriptorSetLayout g_setLayout;
static VkDescriptorSet g_descSet;
static VkBuffer g_ubo;
static VkDeviceMemory g_uboMem;
static std::vector<VkImage> g_imgs;
static std::vector<VkImageView> g_views;
static std::vector<VkFramebuffer> g_fbs;
static std::vector<VkCommandBuffer> g_cmds;
static VkCommandPool g_cpool;
static VkSemaphore g_imgAvail, g_renderDone;

// ---------------------------------------------------------------------------
// wayland listeners
// ---------------------------------------------------------------------------
static void layer_configure(void*, struct zwlr_layer_surface_v1*, uint32_t serial, uint32_t w, uint32_t h) {
    g_serial = serial;
    g_w = (int)w; g_h = (int)h;
    if (g_w > 0 && g_h > 0) g_resizePend = 1;
}
static void layer_closed(void*, struct zwlr_layer_surface_v1*) { exit(0); }
static const struct zwlr_layer_surface_v1_listener layerListener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void reg_global(void*, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver) {
    if (!strcmp(iface, wl_compositor_interface.name))
        g_compositor = (struct wl_compositor*)wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
        g_layerShell = (struct zwlr_layer_shell_v1*)wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 3);
    else if (!strcmp(iface, wp_viewporter_interface.name))
        g_viewporter = (struct wp_viewporter*)wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
}
static void reg_remove(void*, struct wl_registry*, uint32_t) {}
static const struct wl_registry_listener registryListener = {
    .global = reg_global,
    .global_remove = reg_remove,
};

// ---------------------------------------------------------------------------
// shaderc helpers
// ---------------------------------------------------------------------------
static std::vector<uint32_t> compileGlsl(const std::string &src, shaderc_shader_kind kind, std::string *err) {
    shaderc_compiler_t comp = shaderc_compiler_initialize();
    shaderc_compile_options_t opts = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opts, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
    shaderc_compilation_result_t res = shaderc_compile_into_spv(comp, src.c_str(), src.size(), kind, "main", "main", opts);
    std::vector<uint32_t> spv;
    shaderc_compilation_status st = shaderc_result_get_compilation_status(res);
    if (st != shaderc_compilation_status_success) {
        if (err) *err = shaderc_result_get_error_message(res) ? shaderc_result_get_error_message(res) : "";
    } else {
        size_t n = shaderc_result_get_length(res) / 4;
        const uint32_t *bytes = (const uint32_t*)shaderc_result_get_bytes(res);
        spv.assign(bytes, bytes + n);
    }
    shaderc_result_release(res);
    shaderc_compile_options_release(opts);
    shaderc_compiler_release(comp);
    return spv;
}

static VkShaderModule makeModule(const std::vector<uint32_t> &spv) {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = spv.size() * 4; mi.pCode = spv.data();
    VkShaderModule m; VKOK(vkCreateShaderModule(g_dev, &mi, nullptr, &m)); return m;
}

// Fragment shader: shadertoy-style shaders define mainImage(out vec4 fragColor, in vec2 fragCoord).
// Self-contained shaders define their own main() writing fragColor. Support both.
static VkPipeline buildPipeline(const std::string &fragSrc, std::string *err) {
    static const char *kPrefix =
        "#version 450\n"
        "precision highp float;\n"
        "layout(location = 0) out vec4 fragColor;\n"
        "#define gl_FragColor fragColor\n"
        "layout(set=0, binding=0) uniform Uniforms {\n"
        "  vec3  iResolution;\n"
        "  float _pad0;\n"
        "  float iTime;\n"
        "  float iFrame;\n"
        "  vec4  iMouse;\n"
        "  vec4  iDate;\n"
        "  vec4  u_theme[24];\n"
        "} u;\n"
        "#define iResolution u.iResolution\n"
        "#define iTime       u.iTime\n"
        "#define iFrame      u.iFrame\n"
        "#define iMouse      u.iMouse\n"
        "#define iDate       u.iDate\n"
        "#define u_theme     u.u_theme\n"
        // Unprefixed semantic aliases in slots 16..21: the color0..15 palette
        // override never touches these, so they stay the real UI colors.
        "#define u_background u_theme[16]\n"
        "#define u_foreground u_theme[17]\n"
        "#define u_accent     u_theme[18]\n"
        "#define u_muted      u_theme[19]\n"
        "#define u_cursor     u_theme[20]\n"
        "#define u_selection  u_theme[21]\n";

    static const char *kVtx = R"GLSL(
#version 450
layout(location=0) out vec2 vUV;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

    bool hasMainImage = fragSrc.find("mainImage") != std::string::npos;
    std::string frag = std::string(kPrefix) + "\n" + fragSrc + "\n";
    if (hasMainImage) {
        frag += "void main(){ vec4 c = vec4(0.0,0.0,0.0,1.0); "
                "mainImage(c, gl_FragCoord.xy); fragColor = c; }\n";
    }

    auto fspv = compileGlsl(frag, shaderc_fragment_shader, err);
    if (fspv.empty()) return VK_NULL_HANDLE;
    auto vspv = compileGlsl(kVtx, shaderc_vertex_shader, nullptr);
    VkShaderModule vsmod = makeModule(vspv), fsmod = makeModule(fspv);

    VkPipelineShaderStageCreateInfo stages[2] = {
    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
     VK_SHADER_STAGE_VERTEX_BIT, vsmod, "main", nullptr},
    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
     VK_SHADER_STAGE_FRAGMENT_BIT, fsmod, "main", nullptr},
};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0.0f, 0.0f, (float)g_w, (float)g_h, 0.0f, 1.0f};
    VkRect2D scissor{{0,0},{(uint32_t)g_w,(uint32_t)g_h}};
    VkPipelineViewportStateCreateInfo vpState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vpState.viewportCount=1; vpState.pViewports=&vp; vpState.scissorCount=1; vpState.pScissors=&scissor;
    // The render size changes with the render scale and on layer resize, so
    // viewport/scissor are dynamic and set per frame in recordFrame().
    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynState.dynamicStateCount=2; dynState.pDynamicStates=dynStates;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_NONE; rs.lineWidth=1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask=0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount=1; cb.pAttachments=&ba;

    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount=2; gpi.pStages=stages;
    gpi.pVertexInputState=&vi; gpi.pInputAssemblyState=&ia;
    gpi.pViewportState=&vpState; gpi.pRasterizationState=&rs;
    gpi.pMultisampleState=&ms; gpi.pColorBlendState=&cb;
    gpi.pDynamicState=&dynState;
    gpi.layout=g_plLayout; gpi.renderPass=g_pass;
    VkPipeline p;
    VkResult r = vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &p);
    vkDestroyShaderModule(g_dev, vsmod, nullptr);
    vkDestroyShaderModule(g_dev, fsmod, nullptr);
    if (r != VK_SUCCESS) { fprintf(stderr, "SHADERHOST: pipeline create failed rc=%d %s\n", (int)r, err?err->c_str():""); return VK_NULL_HANDLE; }
    return p;
}

// ---------------------------------------------------------------------------
// vulkan setup
// ---------------------------------------------------------------------------
static void initVulkan() {
    const char *exts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME };
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_0; ai.pApplicationName = "webwall-shader";
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo=&ai; ci.enabledExtensionCount=2; ci.ppEnabledExtensionNames=exts;
    VKOK(vkCreateInstance(&ci, nullptr, &g_inst));

    uint32_t n=0; vkEnumeratePhysicalDevices(g_inst, &n, nullptr);
    if (!n) die("no vulkan physical device");
    std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(g_inst, &n, pds.data());
    g_pdev = pds[0];

    VkWaylandSurfaceCreateInfoKHR ws{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
    ws.display=g_display; ws.surface=g_wlSurface;
    VKOK(vkCreateWaylandSurfaceKHR(g_inst, &ws, nullptr, &g_surface));

    // pick family with both graphics+present
    uint32_t nq=0; vkGetPhysicalDeviceQueueFamilyProperties(g_pdev, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq); vkGetPhysicalDeviceQueueFamilyProperties(g_pdev, &nq, qf.data());
    for (uint32_t i=0;i<nq;i++) {
        VkBool32 present=VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_pdev, i, g_surface, &present);
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { g_qfam=i; break; }
    }
    if (g_qfam==VK_QUEUE_FAMILY_IGNORED) die("no graphics+present queue family");

    float prio=1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex=g_qfam; qci.queueCount=1; qci.pQueuePriorities=&prio;
    const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci;
    dci.enabledExtensionCount=1; dci.ppEnabledExtensionNames=dext;
    VKOK(vkCreateDevice(g_pdev, &dci, nullptr, &g_dev));
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_q);

    // formats
    uint32_t nf=0; vkGetPhysicalDeviceSurfaceFormatsKHR(g_pdev, g_surface, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(nf); vkGetPhysicalDeviceSurfaceFormatsKHR(g_pdev, g_surface, &nf, fmts.data());
    for (auto &f : fmts) if (f.format==VK_FORMAT_B8G8R8A8_UNORM) g_fmt=f.format;
    if (g_fmt==VK_FORMAT_UNDEFINED && !fmts.empty()) g_fmt = fmts[0].format;

    // render pass
    VkAttachmentDescription ad{};
    ad.format=g_fmt; ad.samples=VK_SAMPLE_COUNT_1_BIT;
    ad.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; ad.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    ad.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; ad.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    ad.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; ad.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{}; sp.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount=1; sp.pColorAttachments=&ref;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount=1; rpci.pAttachments=&ad; rpci.subpassCount=1; rpci.pSubpasses=&sp;
    VKOK(vkCreateRenderPass(g_dev, &rpci, nullptr, &g_pass));

    // descriptor set
    VkDescriptorSetLayoutBinding dlb{};
    dlb.binding=0; dlb.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    dlb.descriptorCount=1; dlb.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount=1; dli.pBindings=&dlb;
    VKOK(vkCreateDescriptorSetLayout(g_dev, &dli, nullptr, &g_setLayout));
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount=1; pli.pSetLayouts=&g_setLayout;
    VKOK(vkCreatePipelineLayout(g_dev, &pli, nullptr, &g_plLayout));

    VkDescriptorPoolSize dps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets=1; dpci.poolSizeCount=1; dpci.pPoolSizes=&dps;
    VKOK(vkCreateDescriptorPool(g_dev, &dpci, nullptr, &g_dpool));
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool=g_dpool; dai.descriptorSetCount=1; dai.pSetLayouts=&g_setLayout;
    VKOK(vkAllocateDescriptorSets(g_dev, &dai, &g_descSet));

    // UBO
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size=sizeof(UBO); bci.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VKOK(vkCreateBuffer(g_dev, &bci, nullptr, &g_ubo));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev, g_ubo, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_pdev, &mp);
    uint32_t mtype=VK_MAX_MEMORY_TYPES;
    for (uint32_t i=0;i<mp.memoryTypeCount;i++)
        if ((mr.memoryTypeBits & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { mtype=i; break; }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize=mr.size; mai.memoryTypeIndex=mtype;
    VKOK(vkAllocateMemory(g_dev, &mai, nullptr, &g_uboMem));
    VKOK(vkBindBufferMemory(g_dev, g_ubo, g_uboMem, 0));
    VkDescriptorBufferInfo dbi{g_ubo, 0, sizeof(UBO)};
    VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wds.dstSet=g_descSet; wds.dstBinding=0; wds.descriptorCount=1;
    wds.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wds.pBufferInfo=&dbi;
    vkUpdateDescriptorSets(g_dev, 1, &wds, 0, nullptr);

    // semaphores
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VKOK(vkCreateSemaphore(g_dev, &sci, nullptr, &g_imgAvail));
    VKOK(vkCreateSemaphore(g_dev, &sci, nullptr, &g_renderDone));
}

static void createSwapchain() {
    if (g_swapchain) { vkDestroySwapchainKHR(g_dev, g_swapchain, nullptr); g_swapchain=VK_NULL_HANDLE; }
    // The layer surface is always g_w x g_h; the swapchain may be smaller and
    // is stretched to the surface by wp_viewport_set_destination(). On a Wayland
    // layer surface WSI reports currentExtent as "undefined", so the size comes
    // from the layer configure, not from surface capabilities.
    uint32_t w=(uint32_t)std::max(1, g_rw), h=(uint32_t)std::max(1, g_rh);
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface=g_surface; sci.minImageCount=2;
    sci.imageFormat=g_fmt; sci.imageColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent={w,h}; sci.imageArrayLayers=1;
    sci.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; sci.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    uint32_t npm=0; vkGetPhysicalDeviceSurfacePresentModesKHR(g_pdev, g_surface, &npm, nullptr);
    std::vector<VkPresentModeKHR> pms(npm); vkGetPhysicalDeviceSurfacePresentModesKHR(g_pdev, g_surface, &npm, pms.data());
    sci.presentMode=VK_PRESENT_MODE_FIFO_KHR;
    for (auto m : pms) if (m==VK_PRESENT_MODE_MAILBOX_KHR) { sci.presentMode=m; break; }
    VkResult scr = vkCreateSwapchainKHR(g_dev, &sci, nullptr, &g_swapchain);
    if (scr != VK_SUCCESS && g_renderScale < 1.0) {
        // Some WSI implementations insist on the configured surface extent.
        fprintf(stderr, "SHADERHOST: scaled swapchain failed (rc=%d); falling back to 1.0\n", (int)scr);
        g_renderScale = 1.0; g_rw = g_w; g_rh = g_h;
        sci.imageExtent = {(uint32_t)std::max(1, g_rw), (uint32_t)std::max(1, g_rh)};
        scr = vkCreateSwapchainKHR(g_dev, &sci, nullptr, &g_swapchain);
    }
    if (scr != VK_SUCCESS) die("vkCreateSwapchainKHR failed");

    for (auto v:g_views) vkDestroyImageView(g_dev, v, nullptr); g_views.clear();
    for (auto f:g_fbs) vkDestroyFramebuffer(g_dev, f, nullptr); g_fbs.clear();
    if (g_cpool) { vkDestroyCommandPool(g_dev, g_cpool, nullptr); g_cpool=VK_NULL_HANDLE; }
    g_cmds.clear();

    uint32_t n=0; vkGetSwapchainImagesKHR(g_dev, g_swapchain, &n, nullptr);
    g_imgs.resize(n); vkGetSwapchainImagesKHR(g_dev, g_swapchain, &n, g_imgs.data());
    for (auto img : g_imgs) {
        VkImageViewCreateInfo ivi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivi.image=img; ivi.viewType=VK_IMAGE_VIEW_TYPE_2D; ivi.format=g_fmt;
        ivi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkImageView iv; VKOK(vkCreateImageView(g_dev, &ivi, nullptr, &iv)); g_views.push_back(iv);
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass=g_pass; fci.attachmentCount=1; fci.pAttachments=&iv;
        fci.width=w; fci.height=h; fci.layers=1;
        VkFramebuffer fb; VKOK(vkCreateFramebuffer(g_dev, &fci, nullptr, &fb)); g_fbs.push_back(fb);
    }
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpci.queueFamilyIndex=g_qfam;
    VKOK(vkCreateCommandPool(g_dev, &cpci, nullptr, &g_cpool));
    g_cmds.resize(n);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool=g_cpool; cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount=n;
    VKOK(vkAllocateCommandBuffers(g_dev, &cai, g_cmds.data()));
}

static void recordFrame(VkCommandBuffer cb, uint32_t imgIdx) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VKOK(vkBeginCommandBuffer(cb, &bi));
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass=g_pass; rbi.framebuffer=g_fbs[imgIdx];
    rbi.renderArea={{0,0},{(uint32_t)g_rw,(uint32_t)g_rh}};
    VkClearValue cv; cv.color={{0.05f,0.06f,0.07f,1.0f}};
    rbi.clearValueCount=1; rbi.pClearValues=&cv;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline);
    VkViewport vp{0.0f, 0.0f, (float)g_rw, (float)g_rh, 0.0f, 1.0f};
    VkRect2D scissor{{0,0},{(uint32_t)g_rw,(uint32_t)g_rh}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_plLayout, 0, 1, &g_descSet, 0, nullptr);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    VKOK(vkEndCommandBuffer(cb));
}

// ---------------------------------------------------------------------------
// theme colors from Omarchy's native colors.toml (the same source Color.qml
// reads — no hooks): flat `key = "#rrggbb"` lines. Mapping to u_theme[]:
//   background → [0], foreground → [1], accent → [2], muted → [3],
//   cursor → [4], selection → [5]; color0..color15 override [0..15] so the
//   full terminal palette is exposed to shaders.
//   [16]..[21] keep the unprefixed semantic colors (background, foreground,
//   accent, muted, cursor, selection), which the palette override never
//   touches — use these (u_accent, u_background, …) for UI-colored visuals.
// ---------------------------------------------------------------------------
static int hexVal(char c) {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    if (c>='A'&&c<='F') return c-'A'+10;
    return 0;
}
static void themeColor(const std::string &s, float out[4]) {
    std::string v=s;
    size_t h=v.find_last_of('#');
    if (h==std::string::npos) { out[0]=out[1]=out[2]=1.0f; out[3]=1.0f; return; }
    size_t len=v.size()-h-1;
    if (len>=6) {
        out[0] = (hexVal(v[h+1])*16+hexVal(v[h+2]))/255.0f;
        out[1] = (hexVal(v[h+3])*16+hexVal(v[h+4]))/255.0f;
        out[2] = (hexVal(v[h+5])*16+hexVal(v[h+6]))/255.0f;
    } else if (len>=3) {
        out[0]=(hexVal(v[h+1])*17)/255.0f;
        out[1]=(hexVal(v[h+2])*17)/255.0f;
        out[2]=(hexVal(v[h+3])*17)/255.0f;
    } else { out[0]=out[1]=out[2]=1.0f; }
    out[3]=1.0f;
}

// Path to the Omarchy native theme colors.toml.
static std::string themeColorsPath() {
    const char *xdg = getenv("XDG_STATE_HOME");
    std::string base = (xdg && *xdg) ? xdg : (std::string(getenv("HOME") ? getenv("HOME") : "") + "/.local/state");
    return base + "/omarchy/current/theme/colors.toml";
}

static void loadTheme(UBO &ubo) {
    // default dark
    const char *def[] = { "#1e1e2e", "#cdd6f4", "#89b4fa", "#6c7086", "#cba6f7", "#f38ba8" };
    for (int i=0;i<6;i++) themeColor(def[i], ubo.u_theme[i]);

    std::string tp = themeColorsPath();
    std::ifstream f(tp); if (!f) return;
    std::stringstream ss; ss<<f.rdbuf(); std::string js=ss.str();
    std::map<std::string,std::string> kv;
    std::string cur;
    size_t i=0;
    while (i<js.size()) {
        size_t nl=js.find('\n', i);
        if (nl==std::string::npos) break;
        std::string line=js.substr(i, nl-i); i=nl+1;
        size_t hash=line.find('#');
        size_t sz=line.find('=');
        if (hash!=std::string::npos && sz>hash) continue;  // comment line
        if (sz==std::string::npos) continue;
        std::string key=line.substr(0, sz);
        while (!key.empty() && (key.back()==' '||key.back()=='\t')) key.pop_back();
        std::string val=line.substr(sz+1);
        // value: trim, strip quotes / trailing comment
        // (`key = "#302270" # Night Crossing`)
        auto trim=[&](std::string s){ while(!s.empty()&&(s.front()==' '||s.front()=='\t')) s.erase(0,1);
                                      while(!s.empty()&&(s.back()==' '||s.back()=='\t'||s.back()=='\r')) s.pop_back();
                                      return s; };
        val=trim(val);
        if (!val.empty() && (val[0]=='"' || val[0]=='\'')) {
            size_t e=val.find(val[0], 1);
            if (e!=std::string::npos) val=val.substr(1, e-1);
            else val=val.substr(1);
        } else {
            size_t sp=val.find(' ');
            if (sp!=std::string::npos) val=val.substr(0, sp);
            else if (val.size()>1) {
                size_t h=val.find('#', 1);
                if (h!=std::string::npos) val=val.substr(0, h);
            }
        }
        if (val.empty()) continue;
        kv[key]=val;
    }
    auto pick=[&](const char*a, const char*b, const char*fallback){ return kv.count(a)?kv[a]:(kv.count(b)?kv[b]:fallback); };
    auto pick2=[&](const char*a, const char*fallback){ return kv.count(a)?kv[a]:fallback; };
    themeColor(pick("background","bg","#1e1e2e"), ubo.u_theme[0]);
    themeColor(pick("foreground","fg","#cdd6f4"), ubo.u_theme[1]);
    themeColor(pick2("accent", "#89b4fa"), ubo.u_theme[2]);
    themeColor(pick2("muted", "#6c7086"), ubo.u_theme[3]);
    themeColor(pick2("cursor", "#cba6f7"), ubo.u_theme[4]);
    themeColor(pick2("selection", "#f38ba8"), ubo.u_theme[5]);
    for (int i=0;i<16;i++) {
        std::string k="color"+std::to_string(i);
        if (kv.count(k)) themeColor(kv[k], ubo.u_theme[i]);
    }

    // Unprefixed semantic colors, kept out of the palette override's reach so
    // shaders can ask for the real UI accent/background/etc. Cursor and
    // selection fall back the way the web renderer does (cursor -> accent,
    // selection -> accent), never to a foreign default.
    std::string bg  = pick("background","bg","#1e1e2e");
    std::string fg  = pick("foreground","fg","#cdd6f4");
    std::string acc = pick2("accent", "#89b4fa");
    std::string mut = kv.count("muted") ? kv["muted"]
                    : (kv.count("color8") ? kv["color8"] : "#6c7086");
    std::string cursorCol = kv.count("cursor") ? kv["cursor"] : acc;
    std::string sel = kv.count("selection") ? kv["selection"] : acc;
    themeColor(bg,  ubo.u_theme[16]);
    themeColor(fg,  ubo.u_theme[17]);
    themeColor(acc, ubo.u_theme[18]);
    themeColor(mut, ubo.u_theme[19]);
    themeColor(cursorCol, ubo.u_theme[20]);
    themeColor(sel, ubo.u_theme[21]);
}

// ---------------------------------------------------------------------------
static std::string readFile(const std::string &p) {
    std::ifstream f(p); if (!f) return {};
    std::stringstream ss; ss<<f.rdbuf(); return ss.str();
}
static uint64_t mtimeOf(const std::string &p) {
    struct stat st; if (stat(p.c_str(), &st)) return 0; return (uint64_t)st.st_mtime;
}
static uint64_t nowMs() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

// ---------------------------------------------------------------------------
// render scale
//   wallpaper.json: "renderScale": 0.1 .. 1.0   (per wallpaper)
//   $XDG_STATE_HOME/omarchy-webwall/render-scale (global override; wins when
//   present, "off" or a removed file restores the manifest value)
// The shader renders into a smaller swapchain, which wp_viewporter stretches
// to the full layer surface. Everything else (uniforms, screenshots) behaves
// as if the wallpaper were full size, except iResolution, which is the render
// size — that is what shaders key their work off.
// ---------------------------------------------------------------------------
static std::string stateDir() {
    const char *xdg = getenv("XDG_STATE_HOME");
    std::string base = (xdg && *xdg) ? xdg : (std::string(getenv("HOME") ? getenv("HOME") : "") + "/.local/state");
    return base + "/omarchy-webwall";
}
static bool parseRenderScale(const std::string &text, double *out) {
    // accepts a bare number ("0.75") as well as wallpaper.json's
    // "renderScale": 0.75
    size_t p = text.find("\"renderScale\"");
    if (p != std::string::npos) {
        p = text.find(':', p + 13);
        if (p == std::string::npos) return false;
        ++p;
    } else {
        p = 0;
    }
    const char *s = text.c_str() + p;
    char *end = nullptr;
    double v = strtod(s, &end);
    if (end == s) return false;
    *out = v;
    return true;
}
static void resolveRenderScale() {
    double s = 1.0;
    double v;
    if (parseRenderScale(readFile(g_manifestPath), &v)) s = v;

    std::string ov = readFile(stateDir() + "/render-scale");
    if (!ov.empty()) {
        // trim whitespace/newlines; "off"/"auto" = no override
        size_t a = ov.find_first_not_of(" \t\r\n");
        size_t b = ov.find_last_not_of(" \t\r\n");
        std::string t = (a == std::string::npos) ? std::string() : ov.substr(a, b - a + 1);
        if (t == "off" || t == "auto" || t == "default") {
            // keep the manifest value
        } else if (parseRenderScale(t, &v)) {
            s = v;
        } else {
            fprintf(stderr, "SHADERHOST: ignoring bad render-scale override '%s'\n", t.c_str());
        }
    }

    if (s < 0.1) s = 0.1;
    if (s > 1.0) s = 1.0;
    g_renderScale = s;
}
static void updateRenderSize() {
    if (g_w <= 0 || g_h <= 0) return;
    int rw = (int)std::lround((double)g_w * g_renderScale);
    int rh = (int)std::lround((double)g_h * g_renderScale);
    g_rw = std::min(g_w, std::max(1, rw));
    g_rh = std::min(g_h, std::max(1, rh));
}

// Resolve "entry" (relative path) in wallpaper.json against the manifest dir.
static bool resolveEntry() {
    std::string man = readFile(g_manifestPath);
    size_t p = man.find("\"entry\"");
    if (p != std::string::npos) {
        size_t q = man.find('"', p+7); // after :
        size_t q2 = q==std::string::npos ? std::string::npos : man.find('"', q+1);
        if (q2 != std::string::npos) {
            std::string rel = man.substr(q+1, q2-q-1);
            size_t slash = g_manifestPath.find_last_of('/');
            g_entryPath = (slash==std::string::npos?std::string():g_manifestPath.substr(0,slash+1)) + rel;
        }
    }
    return !g_entryPath.empty();
}

// ---------------------------------------------------------------------------
// offscreen snapshot: render one frame to a PPM (no compositor, no surface).
// shader-host --snapshot <out.ppm> <w> <h> <wallpaper.json> <state-file> [--time <sec>]
// ---------------------------------------------------------------------------
static uint32_t snapMemType(VkBuffer buf, VkMemoryPropertyFlags flags) {
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_pdev, &mp);
    for (uint32_t i=0;i<mp.memoryTypeCount;i++)
        if ((mr.memoryTypeBits & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & flags)) return i;
    die("snapshot: no suitable memory type");
    return VK_MAX_MEMORY_TYPES;
}

static int snapshot(const std::string &out, int w, int h, double t0) {
    g_w=w; g_h=h; g_rw=w; g_rh=h; g_fmt=VK_FORMAT_R8G8B8A8_UNORM;

    // instance without any surface extensions
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion=VK_API_VERSION_1_0; ai.pApplicationName="webwall-snapshot";
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo=&ai;
    VKOK(vkCreateInstance(&ci, nullptr, &g_inst));

    uint32_t n=0; vkEnumeratePhysicalDevices(g_inst, &n, nullptr);
    if (!n) die("no vulkan physical device");
    std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(g_inst, &n, pds.data());
    g_pdev = pds[0];

    uint32_t nq=0; vkGetPhysicalDeviceQueueFamilyProperties(g_pdev, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq); vkGetPhysicalDeviceQueueFamilyProperties(g_pdev, &nq, qf.data());
    for (uint32_t i=0;i<nq;i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_qfam=i; break; }
    if (g_qfam==VK_QUEUE_FAMILY_IGNORED) die("no graphics queue family");

    float prio=1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex=g_qfam; qci.queueCount=1; qci.pQueuePriorities=&prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci;
    VKOK(vkCreateDevice(g_pdev, &dci, nullptr, &g_dev));
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_q);

    // render pass targeting the offscreen image (finalLayout transfer-src for readback)
    VkAttachmentDescription ad{};
    ad.format=g_fmt; ad.samples=VK_SAMPLE_COUNT_1_BIT;
    ad.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; ad.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    ad.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; ad.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    ad.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; ad.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{}; sp.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount=1; sp.pColorAttachments=&ref;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount=1; rpci.pAttachments=&ad; rpci.subpassCount=1; rpci.pSubpasses=&sp;
    VKOK(vkCreateRenderPass(g_dev, &rpci, nullptr, &g_pass));

    // descriptor set + pipeline layout + UBO (same as live path)
    VkDescriptorSetLayoutBinding dlb{};
    dlb.binding=0; dlb.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    dlb.descriptorCount=1; dlb.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount=1; dli.pBindings=&dlb;
    VKOK(vkCreateDescriptorSetLayout(g_dev, &dli, nullptr, &g_setLayout));
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount=1; pli.pSetLayouts=&g_setLayout;
    VKOK(vkCreatePipelineLayout(g_dev, &pli, nullptr, &g_plLayout));
    VkDescriptorPoolSize dps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets=1; dpci.poolSizeCount=1; dpci.pPoolSizes=&dps;
    VKOK(vkCreateDescriptorPool(g_dev, &dpci, nullptr, &g_dpool));
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool=g_dpool; dai.descriptorSetCount=1; dai.pSetLayouts=&g_setLayout;
    VKOK(vkAllocateDescriptorSets(g_dev, &dai, &g_descSet));
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size=sizeof(UBO); bci.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VKOK(vkCreateBuffer(g_dev, &bci, nullptr, &g_ubo));
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize=sizeof(UBO); mai.memoryTypeIndex=snapMemType(g_ubo, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VKOK(vkAllocateMemory(g_dev, &mai, nullptr, &g_uboMem));
    VKOK(vkBindBufferMemory(g_dev, g_ubo, g_uboMem, 0));
    VkDescriptorBufferInfo dbi{g_ubo, 0, sizeof(UBO)};
    VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wds.dstSet=g_descSet; wds.dstBinding=0; wds.descriptorCount=1;
    wds.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wds.pBufferInfo=&dbi;
    vkUpdateDescriptorSets(g_dev, 1, &wds, 0, nullptr);

    // offscreen color image + view + framebuffer
    VkImage img; VkDeviceMemory imgMem; uint32_t imgMtype;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType=VK_IMAGE_TYPE_2D; ici.format=g_fmt;
    ici.extent={(uint32_t)w,(uint32_t)h,1}; ici.mipLevels=1; ici.arrayLayers=1;
    ici.samples=VK_SAMPLE_COUNT_1_BIT; ici.tiling=VK_IMAGE_TILING_OPTIMAL;
    ici.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    VKOK(vkCreateImage(g_dev, &ici, nullptr, &img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, img, &mr);
    { VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_pdev, &mp);
      for (uint32_t i=0;i<mp.memoryTypeCount;i++)
        if ((mr.memoryTypeBits & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { imgMtype=i; break; } }
    VkMemoryAllocateInfo dmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    dmai.allocationSize=mr.size; dmai.memoryTypeIndex=imgMtype;
    VKOK(vkAllocateMemory(g_dev, &dmai, nullptr, &imgMem));
    VKOK(vkBindImageMemory(g_dev, img, imgMem, 0));
    VkImageView ivi{};
    { VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      iv.image=img; iv.viewType=VK_IMAGE_VIEW_TYPE_2D; iv.format=g_fmt;
      iv.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
      VKOK(vkCreateImageView(g_dev, &iv, nullptr, &ivi)); }
    VkFramebuffer fb;
    { VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fci.renderPass=g_pass; fci.attachmentCount=1; fci.pAttachments=&ivi;
      fci.width=(uint32_t)w; fci.height=(uint32_t)h; fci.layers=1;
      VKOK(vkCreateFramebuffer(g_dev, &fci, nullptr, &fb)); }

    // readback buffer
    VkBuffer dst; VkDeviceMemory dstMem;
    VkBufferCreateInfo dbc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    dbc.size=(VkDeviceSize)w*h*4; dbc.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    dbc.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VKOK(vkCreateBuffer(g_dev, &dbc, nullptr, &dst));
    { VkMemoryAllocateInfo am{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      am.allocationSize=dbc.size; am.memoryTypeIndex=snapMemType(dst, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      VKOK(vkAllocateMemory(g_dev, &am, nullptr, &dstMem)); }
    VKOK(vkBindBufferMemory(g_dev, dst, dstMem, 0));

    // command buffer
    VkCommandPool cpool;
    { VkCommandPoolCreateInfo c{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      c.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; c.queueFamilyIndex=g_qfam;
      VKOK(vkCreateCommandPool(g_dev, &c, nullptr, &cpool)); }
    VkCommandBuffer cb;
    { VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      a.commandPool=cpool; a.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; a.commandBufferCount=1;
      VKOK(vkAllocateCommandBuffers(g_dev, &a, &cb)); }

    // shader
    std::string fragSrc = readFile(g_entryPath);
    if (fragSrc.empty()) die("empty shader entry");
    std::string err;
    g_pipeline = buildPipeline(fragSrc, &err);
    if (!g_pipeline) { fprintf(stderr, "SHADERHOST: shader compile failed:\n%s\n", err.c_str()); return 1; }

    // UBO
    UBO ubo{}; memset(&ubo, 0, sizeof(ubo));
    ubo.iResolution[0]=(float)w; ubo.iResolution[1]=(float)h; ubo.iResolution[2]=0.0f;
    ubo.iTime=(float)t0; ubo.iFrame=1.0f;
    time_t tnow=time(nullptr); struct tm *lt=localtime(&tnow);
    ubo.iDate[0]=(float)(lt->tm_year+1900); ubo.iDate[1]=(float)(lt->tm_mon+1);
    ubo.iDate[2]=(float)lt->tm_mday; ubo.iDate[3]=(float)(lt->tm_hour*3600+lt->tm_min*60+lt->tm_sec);
    loadTheme(ubo);
    void *ptr=nullptr; VKOK(vkMapMemory(g_dev, g_uboMem, 0, sizeof(UBO), 0, &ptr));
    memcpy(ptr, &ubo, sizeof(UBO)); vkUnmapMemory(g_dev, g_uboMem);

    // record: render pass -> barrier to transfer-src -> copy to buffer
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VKOK(vkBeginCommandBuffer(cb, &bi));
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass=g_pass; rbi.framebuffer=fb;
    rbi.renderArea={{0,0},{(uint32_t)w,(uint32_t)h}};
    VkClearValue cv; cv.color={{0.05f,0.06f,0.07f,1.0f}};
    rbi.clearValueCount=1; rbi.pClearValues=&cv;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline);
    VkViewport svp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    VkRect2D ssc{{0,0},{(uint32_t)w,(uint32_t)h}};
    vkCmdSetViewport(cb, 0, 1, &svp);
    vkCmdSetScissor(cb, 0, 1, &ssc);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_plLayout, 0, 1, &g_descSet, 0, nullptr);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    VkImageMemoryBarrier ibr{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ibr.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    ibr.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    ibr.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ibr.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ibr.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; ibr.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    ibr.image=img; ibr.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &ibr);
    VkBufferImageCopy bic{};
    bic.bufferOffset=0; bic.bufferRowLength=0; bic.bufferImageHeight=0;
    bic.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    bic.imageOffset={0,0,0}; bic.imageExtent={(uint32_t)w,(uint32_t)h,1};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &bic);
    VKOK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&cb;
    VKOK(vkQueueSubmit(g_q, 1, &si, VK_NULL_HANDLE));
    VKOK(vkDeviceWaitIdle(g_dev));

    // write PPM (P6)
    unsigned char *px=nullptr; VKOK(vkMapMemory(g_dev, dstMem, 0, VK_WHOLE_SIZE, 0, (void**)&px));
    FILE *f=fopen(out.c_str(), "wb");
    if (!f) die("snapshot: cannot write output");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i=0;i<w*h;i++) {
        unsigned char r=px[i*4+0], g=px[i*4+1], b=px[i*4+2];
        fputc(r,f); fputc(g,f); fputc(b,f);
    }
    fclose(f);
    vkUnmapMemory(g_dev, dstMem);
    fprintf(stderr, "SHADERHOST: snapshot %dx%d -> %s\n", w, h, out.c_str());
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "--snapshot")) {
        if (argc < 6) { fprintf(stderr, "usage: shader-host --snapshot <out.ppm> <w> <h> <wallpaper.json> <state-file> [--time <sec>]\n"); return 2; }
        const std::string out = argv[2];
        int w = atoi(argv[3]), h = atoi(argv[4]);
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { fprintf(stderr, "bad snapshot size\n"); return 2; }
        g_manifestPath = argv[5];
        g_statePath = argv[6];
        double t0 = 1.0;
        for (int i=7;i<argc-1;i++) if (!strcmp(argv[i], "--time")) t0 = atof(argv[i+1]);
        if (!resolveEntry()) die("no entry in manifest");
        return snapshot(out, w, h, t0);
    }
    if (argc < 3) { fprintf(stderr, "usage: shader-host <wallpaper.json> <state-file>\n"); return 1; }
    g_manifestPath = argv[1];
    g_statePath = argv[2];

    if (!resolveEntry()) die("no entry in manifest");
    fprintf(stderr, "SHADERHOST: manifest=%s entry=%s\n", g_manifestPath.c_str(), g_entryPath.c_str());

    // optional mouse file from env
    if (const char *mf = getenv("WEBWALL_MOUSE_FILE")) g_mouseFile = mf;

    // --- wayland ---
    g_display = wl_display_connect(nullptr);
    if (!g_display) die("cannot connect to wayland");
    g_registry = wl_display_get_registry(g_display);
    wl_registry_add_listener(g_registry, &registryListener, nullptr);
    wl_display_roundtrip(g_display);
    if (!g_compositor || !g_layerShell) die("missing compositor/layer_shell globals");

    g_wlSurface = wl_compositor_create_surface(g_compositor);
    g_layerSurface = zwlr_layer_shell_v1_get_layer_surface(g_layerShell, g_wlSurface, nullptr,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "hapatapa-webwall");
    zwlr_layer_surface_v1_set_anchor(g_layerSurface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP|ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM|
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT|ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(g_layerSurface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(g_layerSurface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(g_layerSurface, &layerListener, nullptr);
    wl_surface_commit(g_wlSurface);
    wl_display_roundtrip(g_display);

    // wait for configure
    if (g_w<=0 || g_h<=0) {
        // some compositors may lag; pump until configured
        for (int i=0;i<200 && g_w<=0 && g_h<=0;i++) {
            wl_display_dispatch_pending(g_display);
            wl_display_flush(g_display);
            usleep(20000);
        }
    }
    if (g_w<=0 || g_h<=0) die("layer never configured with size");
    zwlr_layer_surface_v1_ack_configure(g_layerSurface, g_serial);
    fprintf(stderr, "SHADERHOST: configured %dx%d\n", g_w, g_h);

    // render scale: per-wallpaper value, global override wins
    resolveRenderScale();
    updateRenderSize();
    if (g_renderScale < 1.0) {
        if (g_viewporter) {
            g_viewport = wp_viewporter_get_viewport(g_viewporter, g_wlSurface);
            wp_viewport_set_destination(g_viewport, g_w, g_h);
        } else {
            fprintf(stderr, "SHADERHOST: no wp_viewporter, render scale forced to 1.0\n");
            g_renderScale = 1.0;
            updateRenderSize();
        }
    }
    fprintf(stderr, "SHADERHOST: render scale %.2f -> %dx%d (surface %dx%d)\n",
            g_renderScale, g_rw, g_rh, g_w, g_h);
    g_resizePend = 0;

    // --- vulkan ---
    initVulkan();
    createSwapchain();

    // initial pipeline
    std::string fragSrc = readFile(g_entryPath);
    if (fragSrc.empty()) die("empty shader entry");
    std::string err;
    g_pipeline = buildPipeline(fragSrc, &err);
    if (!g_pipeline) { fprintf(stderr, "SHADERHOST: shader compile failed:\n%s\n", err.c_str()); return 1; }
    fprintf(stderr, "SHADERHOST: pipeline built\n");

    uint64_t shMtime = mtimeOf(g_entryPath);
    std::string themeDir = themeColorsPath();
    uint64_t thMtime = mtimeOf(themeDir);
    uint64_t start = nowMs();
    uint64_t lastPrint = 0;
    int frame = 0;

    while (true) {
        wl_display_dispatch_pending(g_display);
        wl_display_flush(g_display);

        // theme reload
        uint64_t tm = mtimeOf(themeDir);
        if (tm != thMtime && tm != 0) { thMtime = tm; frame = 0; }

        // shader reload
        uint64_t sm = mtimeOf(g_entryPath);
        if (sm != shMtime && sm != 0) {
            shMtime = sm;
            std::string ns = readFile(g_entryPath);
            if (!ns.empty()) {
                std::string e2;
                VkPipeline p2 = buildPipeline(ns, &e2);
                if (p2) {
                    vkDeviceWaitIdle(g_dev);
                    if (g_pipeline) vkDestroyPipeline(g_dev, g_pipeline, nullptr);
                    g_pipeline = p2;
                    fprintf(stderr, "SHADERHOST: shader reloaded\n");
                } else {
                    fprintf(stderr, "SHADERHOST: reload failed:\n%s\n", e2.c_str());
                }
            }
        }

        // resize handled by configure event -> rebuild swapchain
        if (g_resizePend) {
            g_resizePend = 0;
            vkDeviceWaitIdle(g_dev);
            updateRenderSize();
            if (g_viewport) wp_viewport_set_destination(g_viewport, g_w, g_h);
            createSwapchain();
            fprintf(stderr, "SHADERHOST: resized surface %dx%d, render %dx%d\n",
                    g_w, g_h, g_rw, g_rh);
        }

        // write UBO
        UBO ubo{}; memset(&ubo, 0, sizeof(ubo));
        ubo.iResolution[0]=(float)g_rw; ubo.iResolution[1]=(float)g_rh; ubo.iResolution[2]=0.0f;
        ubo.iTime=(float)((double)(nowMs()-start)/1000.0);
        ubo.iFrame=(float)frame++;
        ubo.iMouse[0]=(float)g_mouseX; ubo.iMouse[1]=(float)g_mouseY;
        time_t tnow=time(nullptr); struct tm *lt=localtime(&tnow);
        ubo.iDate[0]=(float)(lt->tm_year+1900); ubo.iDate[1]=(float)(lt->tm_mon+1);
        ubo.iDate[2]=(float)lt->tm_mday; ubo.iDate[3]=(float)(lt->tm_hour*3600+lt->tm_min*60+lt->tm_sec);
        loadTheme(ubo);
        if (g_mouseFile.size()) {
            FILE *mf=fopen(g_mouseFile.c_str(),"r");
            if (mf) { fscanf(mf,"%lf %lf",&g_mouseX,&g_mouseY); fclose(mf); }
            ubo.iMouse[0]=(float)g_mouseX; ubo.iMouse[1]=(float)g_mouseY;
        }
        void *ptr=nullptr; VKOK(vkMapMemory(g_dev, g_uboMem, 0, sizeof(UBO), 0, &ptr));
        memcpy(ptr, &ubo, sizeof(UBO)); vkUnmapMemory(g_dev, g_uboMem);

        uint32_t imgIdx=0;
        VkResult ar = vkAcquireNextImageKHR(g_dev, g_swapchain, UINT64_MAX, g_imgAvail, VK_NULL_HANDLE, &imgIdx);
        if (ar==VK_ERROR_OUT_OF_DATE_KHR || ar==VK_SUBOPTIMAL_KHR) { createSwapchain(); continue; }
        if (ar!=VK_SUCCESS) { usleep(5000); continue; }

        VkCommandBuffer cb=g_cmds[imgIdx];
        VKOK(vkResetCommandBuffer(cb, 0));
        recordFrame(cb, imgIdx);

        VkPipelineStageFlags wst=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount=1; si.pWaitSemaphores=&g_imgAvail; si.pWaitDstStageMask=&wst;
        si.commandBufferCount=1; si.pCommandBuffers=&cb;
        si.signalSemaphoreCount=1; si.pSignalSemaphores=&g_renderDone;
        VKOK(vkQueueSubmit(g_q, 1, &si, VK_NULL_HANDLE));

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount=1; pi.pWaitSemaphores=&g_renderDone;
        pi.swapchainCount=1; pi.pSwapchains=&g_swapchain; pi.pImageIndices=&imgIdx;
        VkResult pr = vkQueuePresentKHR(g_q, &pi);
        if (pr==VK_ERROR_OUT_OF_DATE_KHR || pr==VK_SUBOPTIMAL_KHR) { vkDeviceWaitIdle(g_dev); createSwapchain(); }
        VKOK(vkQueueWaitIdle(g_q));

        wl_surface_commit(g_wlSurface);
        wl_display_flush(g_display);
        usleep(15000);
    }
    return 0;
}