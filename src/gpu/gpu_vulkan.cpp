// xyo-cpp: Vulkan compute executor — gpu_vm.comp の SPIR-V を動的ロード
// した Vulkan ランタイム上で dispatch する。ウィンドウ/サーフェス不要で
// headless `run` や Windows .exe でも GPU オフロードが効く。ローダは
// dlopen/LoadLibrary で遅延解決し、無い環境では supported=false →
// 呼出し側は CPU フォールバック。
#include "gpu_vulkan.hpp"

#include "gpu_analysis.hpp"   // penOrderOffsets

#ifndef XYO_GPU_VULKAN

namespace xyo::gpu {
bool vulkanExecSupported() { return false; }
bool vulkanExecParallel(ExecRequest&) { return false; }
} // namespace xyo::gpu

#else // XYO_GPU_VULKAN

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX          // std::min/std::max をマクロで壊させない
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace xyo::gpu {
namespace {

// SPIR-V（cmake が glslc -mfmt=c で生成 — 中身は {0x…} 初期化子列）
const uint32_t kGpuVmSpv[] =
#include "gpu_vm_spv.inc"
    ;

// ---- 動的ロードした関数ポインタ ------------------------------------
struct VkApi {
    PFN_vkGetInstanceProcAddr              GetInstanceProcAddr;
    PFN_vkCreateInstance                   CreateInstance;
    PFN_vkDestroyInstance                  DestroyInstance;
    PFN_vkEnumeratePhysicalDevices         EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties      GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkCreateDevice                     CreateDevice;
    PFN_vkGetDeviceProcAddr                GetDeviceProcAddr;
    PFN_vkDestroyDevice                    DestroyDevice;
    PFN_vkGetDeviceQueue                   GetDeviceQueue;
    PFN_vkDeviceWaitIdle                   DeviceWaitIdle;
    PFN_vkCreateBuffer                     CreateBuffer;
    PFN_vkGetBufferMemoryRequirements      GetBufferMemoryRequirements;
    PFN_vkAllocateMemory                   AllocateMemory;
    PFN_vkBindBufferMemory                 BindBufferMemory;
    PFN_vkMapMemory                        MapMemory;
    PFN_vkUnmapMemory                      UnmapMemory;
    PFN_vkFreeMemory                       FreeMemory;
    PFN_vkDestroyBuffer                    DestroyBuffer;
    PFN_vkCreateShaderModule               CreateShaderModule;
    PFN_vkDestroyShaderModule              DestroyShaderModule;
    PFN_vkCreateDescriptorSetLayout        CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout       DestroyDescriptorSetLayout;
    PFN_vkCreatePipelineLayout             CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout            DestroyPipelineLayout;
    PFN_vkCreateComputePipelines           CreateComputePipelines;
    PFN_vkDestroyPipeline                  DestroyPipeline;
    PFN_vkCreateDescriptorPool             CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool            DestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets           AllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets             UpdateDescriptorSets;
    PFN_vkCreateCommandPool                CreateCommandPool;
    PFN_vkDestroyCommandPool               DestroyCommandPool;
    PFN_vkAllocateCommandBuffers           AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers               FreeCommandBuffers;
    PFN_vkBeginCommandBuffer               BeginCommandBuffer;
    PFN_vkEndCommandBuffer                 EndCommandBuffer;
    PFN_vkResetCommandBuffer               ResetCommandBuffer;
    PFN_vkCmdBindPipeline                  CmdBindPipeline;
    PFN_vkCmdBindDescriptorSets            CmdBindDescriptorSets;
    PFN_vkCmdDispatch                      CmdDispatch;
    PFN_vkCmdPipelineBarrier               CmdPipelineBarrier;
    PFN_vkQueueSubmit                      QueueSubmit;
    PFN_vkCreateFence                      CreateFence;
    PFN_vkWaitForFences                    WaitForFences;
    PFN_vkResetFences                      ResetFences;
    PFN_vkDestroyFence                     DestroyFence;
};
VkApi vk;

struct Buf {
    VkBuffer       b = VK_NULL_HANDLE;
    VkDeviceMemory m = VK_NULL_HANDLE;
    void*          map = nullptr;
    VkDeviceSize   cap = 0;
};

struct VkCtx {
    bool                    inited = false;
    bool                    ok = false;
#ifdef _WIN32
    HMODULE                 lib = nullptr;
#else
    void*                   lib = nullptr;
#endif
    VkInstance              inst = VK_NULL_HANDLE;
    VkPhysicalDevice        phys = VK_NULL_HANDLE;
    VkDevice                dev = VK_NULL_HANDLE;
    uint32_t                qf = 0;
    VkQueue                 queue = VK_NULL_HANDLE;
    VkCommandPool           pool = VK_NULL_HANDLE;
    VkCommandBuffer         cmd = VK_NULL_HANDLE;
    VkDescriptorSetLayout   dsetLayout = VK_NULL_HANDLE;
    VkPipelineLayout        pipeLayout = VK_NULL_HANDLE;
    VkPipeline              pipe = VK_NULL_HANDLE;
    VkDescriptorPool        dpool = VK_NULL_HANDLE;
    VkDescriptorSet         dset = VK_NULL_HANDLE;
    VkFence                 fence = VK_NULL_HANDLE;
    Buf                     params, code, globs, recs, cnts, lout;
    VkDeviceSize            ssboMax = 0;
    uint32_t                maxGroupsX = 0;
    std::vector<uint32_t>   cntHost;
    std::mutex              mu;   // 並列スケジューラの複数スレッドからの dispatch 直列化
};
VkCtx g;

void* libSym(const char* name) {
#ifdef _WIN32
    return (void*)GetProcAddress(g.lib, name);
#else
    return dlsym(g.lib, name);
#endif
}

#define LOAD(sym)                                                   \
    do {                                                            \
        vk.sym = (PFN_vk##sym)vk.GetInstanceProcAddr(g.inst,        \
                                                 "vk" #sym);        \
        if (!vk.sym) return false;                                  \
    } while(0)
#define LOADD(sym)                                                  \
    do {                                                            \
        vk.sym = (PFN_vk##sym)vk.GetDeviceProcAddr(g.dev, "vk" #sym); \
        if (!vk.sym) return false;                                  \
    } while(0)

bool libOpen() {
#ifdef _WIN32
    g.lib = LoadLibraryA("vulkan-1.dll");
    if (!g.lib) g.lib = LoadLibraryA("vulkan.dll");
#else
    g.lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g.lib) g.lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
#endif
    return g.lib != nullptr;
}

uint32_t findMemType(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties props;
    vk.GetPhysicalDeviceMemoryProperties(g.phys, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

void destroyBuf(Buf& b) {
    if (b.b == VK_NULL_HANDLE) return;
    if (b.map) vk.UnmapMemory(g.dev, b.m);
    vk.DestroyBuffer(g.dev, b.b, nullptr);
    vk.FreeMemory(g.dev, b.m, nullptr);
    b = Buf{};
}

// wantMap=true なら HOST_VISIBLE|HOST_COHERENT で常駐マップ
bool makeBuf(Buf& b, VkDeviceSize size, bool wantMap) {
    destroyBuf(b);
    VkBufferCreateInfo bi = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size,
        .usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vk.CreateBuffer(g.dev, &bi, nullptr, &b.b) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    vk.GetBufferMemoryRequirements(g.dev, b.b, &req);
    uint32_t mt;
    if (wantMap)
        mt = findMemType(req.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    else {
        mt = findMemType(req.memoryTypeBits,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt == UINT32_MAX)
            mt = findMemType(req.memoryTypeBits, 0);
    }
    if (mt == UINT32_MAX) { destroyBuf(b); return false; }
    VkMemoryAllocateInfo ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = mt,
    };
    if (vk.AllocateMemory(g.dev, &ai, nullptr, &b.m) != VK_SUCCESS ||
        vk.BindBufferMemory(g.dev, b.b, b.m, 0) != VK_SUCCESS) {
        destroyBuf(b);
        return false;
    }
    b.cap = size;
    if (wantMap &&
        vk.MapMemory(g.dev, b.m, 0, size, 0, &b.map) != VK_SUCCESS) {
        destroyBuf(b);
        return false;
    }
    return true;
}

bool ensureBuf(Buf& b, VkDeviceSize need, bool wantMap) {
    if (b.cap >= need) return true;
    // 実行中参照は無いので waitIdle → 再確保で安全
    vk.DeviceWaitIdle(g.dev);
    return makeBuf(b, need * 2, wantMap);
}

void shutdown() {
    if (g.dev != VK_NULL_HANDLE) {
        if (vk.DeviceWaitIdle) vk.DeviceWaitIdle(g.dev);
        destroyBuf(g.params);
        destroyBuf(g.code);
        destroyBuf(g.globs);
        destroyBuf(g.recs);
        destroyBuf(g.cnts);
        destroyBuf(g.lout);
        if (g.fence) vk.DestroyFence(g.dev, g.fence, nullptr);
        if (g.pool) vk.DestroyCommandPool(g.dev, g.pool, nullptr);
        if (g.dpool) vk.DestroyDescriptorPool(g.dev, g.dpool, nullptr);
        if (g.pipe) vk.DestroyPipeline(g.dev, g.pipe, nullptr);
        if (g.pipeLayout)
            vk.DestroyPipelineLayout(g.dev, g.pipeLayout, nullptr);
        if (g.dsetLayout)
            vk.DestroyDescriptorSetLayout(g.dev, g.dsetLayout, nullptr);
        vk.DestroyDevice(g.dev, nullptr);
        g.dev = VK_NULL_HANDLE;
    }
    if (g.inst != VK_NULL_HANDLE && vk.DestroyInstance) {
        vk.DestroyInstance(g.inst, nullptr);
        g.inst = VK_NULL_HANDLE;
    }
    if (g.lib) {
#ifdef _WIN32
        FreeLibrary(g.lib);
#else
        dlclose(g.lib);
#endif
        g.lib = nullptr;
    }
}

bool initImpl();

bool init() {
    if (g.inited) return g.ok;
    g.inited = true;
    g.ok = initImpl();
    if (!g.ok) shutdown();
    return g.ok;
}

bool initImpl() {
    if (std::getenv("XYO_GPU_VK") &&
        std::getenv("XYO_GPU_VK")[0] == '0')
        return false;
    if (!libOpen()) return false;
    vk.GetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)libSym("vkGetInstanceProcAddr");
    if (!vk.GetInstanceProcAddr) return false;
    LOAD(CreateInstance);

    VkApplicationInfo app = {
        .sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "xyo",
        .apiVersion       = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ii = {
        .sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    if (vk.CreateInstance(&ii, nullptr, &g.inst) != VK_SUCCESS)
        return false;

    // instance-level 残り
    LOAD(EnumeratePhysicalDevices);
    LOAD(DestroyInstance);
    LOAD(GetPhysicalDeviceProperties);
    LOAD(GetPhysicalDeviceMemoryProperties);
    LOAD(GetPhysicalDeviceQueueFamilyProperties);
    LOAD(CreateDevice);
    LOAD(GetDeviceProcAddr);
    LOAD(DestroyDevice);

    // device pick: compute 可能 queue family を持ち、かつハードウェア
    // デバイスを優先する（DISCRETE > INTEGRATED > VIRTUAL）。CPU 実装
    // （llvmpipe/lavapipe）は同じ計算を CPU JIT の方が速く行うため、
    // 既定では使わない — XYO_VK_DEV=名前部分列 を指定したときだけ許可。
    uint32_t n = 0;
    vk.EnumeratePhysicalDevices(g.inst, &n, nullptr);
    if (!n) return false;
    std::vector<VkPhysicalDevice> devs(n);
    vk.EnumeratePhysicalDevices(g.inst, &n, devs.data());

    const char* want = std::getenv("XYO_VK_DEV");
    int bestRank = -1;
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties pp;
        vk.GetPhysicalDeviceProperties(devs[i], &pp);
        if (want && !std::strstr(pp.deviceName, want)) continue;
        int rank = 0;
        switch (pp.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: rank = 3; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: rank = 2; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: rank = 1; break;
        default: rank = 0; break;   // CPU / OTHER
        }
        if (rank == 0 && !want) continue;   // ソフトウェア実装は既定で拒否
        if (rank <= bestRank) continue;
        uint32_t qc = 0;
        vk.GetPhysicalDeviceQueueFamilyProperties(devs[i], &qc, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qc);
        vk.GetPhysicalDeviceQueueFamilyProperties(devs[i], &qc, qf.data());
        for (uint32_t j = 0; j < qc; ++j) {
            if (!(qf[j].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
            g.phys        = devs[i];
            g.qf          = j;
            g.ssboMax     = pp.limits.maxStorageBufferRange;
            g.maxGroupsX  = pp.limits.maxComputeWorkGroupCount[0];
            bestRank      = rank;
            break;
        }
    }
    if (g.phys == VK_NULL_HANDLE) return false;
    if (std::getenv("XYO_GPU_DEBUG")) {
        VkPhysicalDeviceProperties pp;
        vk.GetPhysicalDeviceProperties(g.phys, &pp);
        std::fprintf(stderr, "xyo-gpu-vk: device=%s type=%d\n",
                     pp.deviceName, int(pp.deviceType));
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g.qf,
        .queueCount       = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo di = {
        .sType                 = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount  = 1,
        .pQueueCreateInfos     = &qi,
    };
    if (vk.CreateDevice(g.phys, &di, nullptr, &g.dev) != VK_SUCCESS)
        return false;

    LOADD(GetDeviceQueue);
    LOADD(DeviceWaitIdle);
    LOADD(CreateBuffer);
    LOADD(GetBufferMemoryRequirements);
    LOADD(AllocateMemory);
    LOADD(BindBufferMemory);
    LOADD(MapMemory);
    LOADD(UnmapMemory);
    LOADD(FreeMemory);
    LOADD(DestroyBuffer);
    LOADD(CreateShaderModule);
    LOADD(DestroyShaderModule);
    LOADD(CreateDescriptorSetLayout);
    LOADD(DestroyDescriptorSetLayout);
    LOADD(CreatePipelineLayout);
    LOADD(DestroyPipelineLayout);
    LOADD(CreateComputePipelines);
    LOADD(DestroyPipeline);
    LOADD(CreateDescriptorPool);
    LOADD(DestroyDescriptorPool);
    LOADD(AllocateDescriptorSets);
    LOADD(UpdateDescriptorSets);
    LOADD(CreateCommandPool);
    LOADD(DestroyCommandPool);
    LOADD(AllocateCommandBuffers);
    LOADD(FreeCommandBuffers);
    LOADD(BeginCommandBuffer);
    LOADD(EndCommandBuffer);
    LOADD(ResetCommandBuffer);
    LOADD(CmdBindPipeline);
    LOADD(CmdBindDescriptorSets);
    LOADD(CmdDispatch);
    LOADD(CmdPipelineBarrier);
    LOADD(QueueSubmit);
    LOADD(CreateFence);
    LOADD(WaitForFences);
    LOADD(ResetFences);
    LOADD(DestroyFence);
    LOADD(DestroyDevice);

    vk.GetDeviceQueue(g.dev, g.qf, 0, &g.queue);

    // descriptor set layout: binding0..5 = STORAGE_BUFFER（シェーダ準拠）
    VkDescriptorSetLayoutBinding binds[6] = {};
    for (uint32_t i = 0; i < 6; ++i) {
        binds[i].binding         = i;
        binds[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dl = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 6,
        .pBindings    = binds,
    };
    if (vk.CreateDescriptorSetLayout(g.dev, &dl, nullptr,
                                     &g.dsetLayout) != VK_SUCCESS)
        return false;

    VkPipelineLayoutCreateInfo pl = {
        .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts    = &g.dsetLayout,
    };
    if (vk.CreatePipelineLayout(g.dev, &pl, nullptr, &g.pipeLayout) !=
        VK_SUCCESS)
        return false;

    VkShaderModule mod;
    VkShaderModuleCreateInfo sm = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(kGpuVmSpv),
        .pCode    = kGpuVmSpv,
    };
    if (vk.CreateShaderModule(g.dev, &sm, nullptr, &mod) != VK_SUCCESS)
        return false;
    VkComputePipelineCreateInfo cp = {
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage  = {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = mod,
            .pName  = "main",
        },
        .layout = g.pipeLayout,
    };
    VkResult pr = vk.CreateComputePipelines(g.dev, VK_NULL_HANDLE, 1,
                                            &cp, nullptr, &g.pipe);
    vk.DestroyShaderModule(g.dev, mod, nullptr);
    if (pr != VK_SUCCESS) return false;

    VkDescriptorPoolSize psz = {
        .type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 6,
    };
    VkDescriptorPoolCreateInfo dp = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = 1,
        .poolSizeCount = 1,
        .pPoolSizes    = &psz,
    };
    if (vk.CreateDescriptorPool(g.dev, &dp, nullptr, &g.dpool) !=
        VK_SUCCESS)
        return false;
    VkDescriptorSetAllocateInfo da = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = g.dpool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &g.dsetLayout,
    };
    if (vk.AllocateDescriptorSets(g.dev, &da, &g.dset) != VK_SUCCESS)
        return false;

    VkCommandPoolCreateInfo cp2 = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g.qf,
    };
    if (vk.CreateCommandPool(g.dev, &cp2, nullptr, &g.pool) != VK_SUCCESS)
        return false;
    VkCommandBufferAllocateInfo cba = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = g.pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vk.AllocateCommandBuffers(g.dev, &cba, &g.cmd) != VK_SUCCESS)
        return false;

    VkFenceCreateInfo fc = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    if (vk.CreateFence(g.dev, &fc, nullptr, &g.fence) != VK_SUCCESS)
        return false;

    g.ok = true;
    return true;
}

} // namespace

bool vulkanExecSupported() { return init(); }

bool vulkanExecParallel(ExecRequest& req) {
    if (!init()) return false;
    std::lock_guard<std::mutex> lk(g.mu);

    const uint32_t stride = laneOutStride(req.nWb);
    const uint64_t paramBytes = (uint64_t)(20 + req.nWb) * 4;
    const uint64_t codeBytes  = (uint64_t)req.codeLen * 4;
    const uint64_t globBytes  = (uint64_t)std::max<uint32_t>(req.nGlobals, 1) * 4;
    const uint64_t cntBytes   = (uint64_t)req.lanes * 4;
    const uint64_t loutBytes  = (uint64_t)req.lanes * stride * 4;
    const uint64_t recBytes   = (uint64_t)req.lanes * req.maxPerLane * 48;
    const uint64_t total = paramBytes + codeBytes + globBytes + cntBytes +
                           loutBytes + recBytes;
    if (!req.lanes || !req.code || !req.codeLen) return false;
    if (total > uint64_t(512) * 1024 * 1024) return false;
    if (req.nGlobals && !req.globals) return false;
    if (g.ssboMax &&
        int64_t(std::max({recBytes, loutBytes, codeBytes})) >
            int64_t(g.ssboMax))
        return false;
    const uint32_t groups = (req.lanes + 63) / 64;
    if (g.maxGroupsX && groups > g.maxGroupsX) return false;

    if (!ensureBuf(g.params, paramBytes, true) ||
        !ensureBuf(g.code, codeBytes, true) ||
        !ensureBuf(g.globs, globBytes, true) ||
        !ensureBuf(g.recs, recBytes, false) ||
        !ensureBuf(g.cnts, cntBytes, true) ||
        !ensureBuf(g.lout, loutBytes, true))
        return false;

    // params（GL 版と同一パッキング: 固定20 + wbSlot[]）
    float* pr = (float*)g.params.map;
    pr[0] = req.startX; pr[1] = req.startY;
    pr[2] = req.penSize;
    pr[3] = req.penHSV[0]; pr[4] = req.penHSV[1];
    pr[5] = req.penHSV[2]; pr[6] = req.penHSV[3];
    pr[7] = req.penDown ? 1.0f : 0.0f;
    pr[8] = req.fenceL; pr[9] = req.fenceR;
    pr[10] = req.fenceB; pr[11] = req.fenceT;
    pr[12] = float(req.nSlots);
    pr[13] = float(req.nWb);
    pr[14] = float(req.maxPerLane);
    pr[15] = float(req.lanes);
    {
        float fb;
        std::memcpy(&fb, &req.baseSeed, 4);   // u32 ビットパターン
        pr[16] = fb;
    }
    pr[17] = float(req.codeLen / 2);
    pr[18] = float(stride);
    pr[19] = req.miscLimits ? 1.0f : 0.0f;
    for (uint32_t i = 0; i < req.nWb; ++i)
        pr[20 + i] = float(req.wbSlots ? req.wbSlots[i] : 0);

    std::memcpy(g.code.map, req.code, size_t(codeBytes));
    if (req.nGlobals)
        std::memcpy(g.globs.map, req.globals, size_t(req.nGlobals) * 4);

    // descriptor 更新（バッファ再確保に備え毎回発行）
    VkDescriptorBufferInfo infos[6] = {
        {g.params.b, 0, VK_WHOLE_SIZE}, {g.code.b, 0, VK_WHOLE_SIZE},
        {g.recs.b, 0, VK_WHOLE_SIZE},   {g.cnts.b, 0, VK_WHOLE_SIZE},
        {g.lout.b, 0, VK_WHOLE_SIZE},   {g.globs.b, 0, VK_WHOLE_SIZE},
    };
    VkWriteDescriptorSet wr[6] = {};
    for (uint32_t i = 0; i < 6; ++i) {
        wr[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet          = g.dset;
        wr[i].dstBinding      = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wr[i].pBufferInfo     = &infos[i];
    }
    vk.UpdateDescriptorSets(g.dev, 6, wr, 0, nullptr);

    // 記録 → submit → 待機
    vk.ResetCommandBuffer(g.cmd, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    if (vk.BeginCommandBuffer(g.cmd, &bi) != VK_SUCCESS) return false;
    vk.CmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipe);
    vk.CmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                             g.pipeLayout, 0, 1, &g.dset, 0, nullptr);
    vk.CmdDispatch(g.cmd, groups, 1, 1);
    // shader write → host read（coherent でも実行可視性のため明示）
    VkMemoryBarrier mb = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    vk.CmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0,
                          nullptr, 0, nullptr);
    if (vk.EndCommandBuffer(g.cmd) != VK_SUCCESS) return false;

    vk.ResetFences(g.dev, 1, &g.fence);
    VkSubmitInfo si = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &g.cmd,
    };
    if (vk.QueueSubmit(g.queue, 1, &si, g.fence) != VK_SUCCESS)
        return false;
    if (vk.WaitForFences(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX) !=
        VK_SUCCESS)
        return false;

    // cnts → guarded/overflow 判定（GL 版と同じ penOrderOffsets）
    g.cntHost.resize(req.lanes);
    std::memcpy(g.cntHost.data(), g.cnts.map, size_t(cntBytes));
    std::vector<uint32_t> offsets(req.lanes);
    uint64_t totalRecords = 0;
    bool countsOk = penOrderOffsets(g.cntHost.data(), req.lanes,
                                    req.maxPerLane, offsets.data(),
                                    &totalRecords);
    req.overflow = !countsOk;
    if (!countsOk) return false;   // GL 版同様 guarded は CPU へ
    req.nRecords = uint32_t(std::min<uint64_t>(totalRecords, UINT32_MAX));

    req.laneOut  = (const float*)g.lout.map;
    req.executed = true;
    return true;
}

} // namespace xyo::gpu

#endif // XYO_GPU_VULKAN
