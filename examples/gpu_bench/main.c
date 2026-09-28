/* Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GPU ceilings through ps5vk: peak FP32 FMA throughput (wave32 and wave64),
 * straight-line code throughput against code size (instruction fetch),
 * memory read bandwidth, cached re-read bandwidth and the per-dispatch cost
 * of a chained submission. Times are submit-to-fence wall clock minus an
 * empty-submission baseline, so sub-0.1 ms results are only indicative.
 */
#include <ps5vk/ps5vk.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "gpu_bench_shaders.h"

extern int sceKernelDebugOutText(int level, const char *text);
extern int native_heap_init(void);

static FILE *log_file;
static void report(const char *format, ...)
{
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    sceKernelDebugOutText(0, message);
    if (log_file) { fputs(message, log_file); fflush(log_file); }
}

#define CHECK(call) do { VkResult rc_ = (call); if (rc_ != VK_SUCCESS) { \
    report("GPU_BENCH_ERROR %s = %d\n", #call, (int)rc_); return 1; } } while (0)

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static VkDevice device;
static VkQueue queue;
static VkCommandBuffer cmd;
static VkFence fence;
static VkPhysicalDeviceMemoryProperties memory_properties;

struct buffer { VkBuffer buffer; VkDeviceMemory memory; };
struct kernel { VkDescriptorSetLayout set_layout; VkPipelineLayout layout; VkPipeline pipeline; VkDescriptorSet set; };

static int create_buffer(struct buffer *b, VkDeviceSize bytes)
{
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    CHECK(vkCreateBuffer(device, &bi, NULL, &b->buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b->buffer, &req);
    uint32_t type = 0;
    while (type < memory_properties.memoryTypeCount && !(req.memoryTypeBits & (1u << type))) ++type;
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                               .memoryTypeIndex = type};
    CHECK(vkAllocateMemory(device, &ai, NULL, &b->memory));
    CHECK(vkBindBufferMemory(device, b->buffer, b->memory, 0));
    return 0;
}

static int create_kernel(struct kernel *k, const uint32_t *code, size_t bytes, uint32_t buffers, uint32_t push,
                         uint32_t wave, VkDescriptorPool pool, const struct buffer *const *bound)
{
    VkDescriptorSetLayoutBinding bindings[2];
    for (uint32_t i = 0; i < buffers; ++i)
        bindings[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                    VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo sl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = buffers, .pBindings = bindings};
    CHECK(vkCreateDescriptorSetLayout(device, &sl, NULL, &k->set_layout));
    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, push};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &k->set_layout, .pushConstantRangeCount = push ? 1u : 0u,
                                     .pPushConstantRanges = push ? &range : NULL};
    CHECK(vkCreatePipelineLayout(device, &pl, NULL, &k->layout));
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes,
                                   .pCode = code};
    VkShaderModule module;
    CHECK(vkCreateShaderModule(device, &mi, NULL, &module));
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo size = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO,
        .requiredSubgroupSize = wave};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = wave ? &size : NULL,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
        .layout = k->layout};
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &k->pipeline));
    vkDestroyShaderModule(device, module, NULL);
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &k->set_layout};
    CHECK(vkAllocateDescriptorSets(device, &ai, &k->set));
    for (uint32_t i = 0; i < buffers; ++i) {
        VkDescriptorBufferInfo info = {bound[i]->buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = k->set, .dstBinding = i,
            .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &info};
        vkUpdateDescriptorSets(device, 1, &w, 0, NULL);
    }
    return 0;
}

/* Record `repeat` dispatches of one kernel, submit, wait; returns wall milliseconds. */
static double run(const struct kernel *k, const void *push, uint32_t push_bytes, uint32_t groups, uint32_t repeat)
{
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return -1;
    const VkMemoryBarrier between = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT,
                                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    for (uint32_t i = 0; k && i < repeat; ++i) {
        if (i) vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    0, 1, &between, 0, NULL, 0, NULL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k->pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k->layout, 0, 1, &k->set, 0, NULL);
        if (push_bytes) vkCmdPushConstants(cmd, k->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes, push);
        vkCmdDispatch(cmd, groups, 1, 1);
    }
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) return -1;
    VkSubmitInfo s = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    const double t0 = now_ms();
    if (vkQueueSubmit(queue, 1, &s, fence) != VK_SUCCESS) return -1;
    if (vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(20000000000)) != VK_SUCCESS) {
        report("GPU_BENCH_ERROR fence\n");
        for (;;) usleep(100000);
    }
    const double elapsed = now_ms() - t0;
    vkResetFences(device, 1, &fence);
    vkResetCommandBuffer(cmd, 0);
    return elapsed;
}

static double best(const struct kernel *k, const void *push, uint32_t push_bytes, uint32_t groups, uint32_t repeat)
{
    double result = 1e30;
    for (int i = 0; i < 3; ++i) {
        const double t = run(k, push, push_bytes, groups, repeat);
        if (t >= 0 && t < result) result = t;
    }
    return result;
}

/* Loads wrapped in `region` bytes: each invocation visits 1024 distinct
 * slots of the region in scattered order; returns GB/s over 5 dispatches. */
static double reread_gbps(const struct kernel *k, uint32_t region, uint32_t groups)
{
    const uint32_t elements = region / 16;
    struct { uint32_t loads, stride, mask; } push = {1024, elements / 1024 * 7, elements - 1};
    const double t1 = best(k, &push, sizeof(push), groups, 1);
    const double t5 = best(k, &push, sizeof(push), groups, 5);
    const double bytes = (double)groups * 64.0 * push.loads * 16.0;
    return bytes / ((t5 - t1) / 4.0) / 1e6;
}

static int bench(void)
{
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance;
    CHECK(vkCreateInstance(&ii, NULL, &instance));
    uint32_t count = 1;
    VkPhysicalDevice physical;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, &physical));
    VkPhysicalDeviceVulkan13Features offered = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &offered};
    vkGetPhysicalDeviceFeatures2(physical, &query);
    VkPhysicalDeviceVulkan13Features enable = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                               .subgroupSizeControl = offered.subgroupSizeControl};
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
                                  .pQueuePriorities = &priority};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = offered.subgroupSizeControl ? &enable : NULL, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
    CHECK(vkCreateDevice(physical, &di, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);

    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool command_pool;
    CHECK(vkCreateCommandPool(device, &cpi, NULL, &command_pool));
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(device, &fi, NULL, &fence));

    enum { GROUPS = 4608, READ_BYTES = 64 << 20 };
    struct buffer out, data;
    if (create_buffer(&out, (VkDeviceSize)GROUPS * 64 * 4) || create_buffer(&data, READ_BYTES)) return 1;
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 128};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 64,
                                      .poolSizeCount = 1, .pPoolSizes = &size};
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(device, &dpi, NULL, &pool));
    const struct buffer *out_only[1] = {&out}, *read_bound[2] = {&data, &out};
    struct kernel fma32, fma64, reader, empty, unrolled32, unrolled64, small32;
    if (create_kernel(&fma32, gpu_bench_fma_spv, sizeof(gpu_bench_fma_spv), 1, 8, 0, pool, out_only) ||
        create_kernel(&fma64, gpu_bench_fma_spv, sizeof(gpu_bench_fma_spv), 1, 8,
                      offered.subgroupSizeControl ? 64 : 0, pool, out_only) ||
        create_kernel(&reader, gpu_bench_read_spv, sizeof(gpu_bench_read_spv), 2, 12, 0, pool, read_bound) ||
        create_kernel(&empty, gpu_bench_empty_spv, sizeof(gpu_bench_empty_spv), 1, 0, 0, pool, out_only) ||
        create_kernel(&unrolled32, gpu_bench_unrolled_spv, sizeof(gpu_bench_unrolled_spv), 1, 0, 0, pool,
                      out_only) ||
        create_kernel(&unrolled64, gpu_bench_unrolled_spv, sizeof(gpu_bench_unrolled_spv), 1, 0,
                      offered.subgroupSizeControl ? 64 : 0, pool, out_only) ||
        create_kernel(&small32, gpu_bench_unrolled_small_spv, sizeof(gpu_bench_unrolled_small_spv), 1, 0, 0,
                      pool, out_only))
        return 1;
    report("GPU_BENCH_READY device=%s wave64=%u\n", properties.deviceName, offered.subgroupSizeControl);

    const double baseline = best(NULL, NULL, 0, 0, 0);
    const double one = best(&empty, NULL, 0, 1, 1), many = best(&empty, NULL, 0, 1, 64);
    report("GPU_BENCH_SUBMIT empty_ms=%.3f one_dispatch_ms=%.3f per_chained_dispatch_us=%.2f\n",
           baseline, one, (many - one) / 63.0 * 1000.0);

    for (uint32_t groups = 576; groups <= GROUPS; groups *= 2) {
        struct { uint32_t iterations; float scale; } push = {4096, 0.999f};
        const double t32 = best(&fma32, &push, sizeof(push), groups, 1) - one;
        const double t64 = best(&fma64, &push, sizeof(push), groups, 1) - one;
        const double flops = 2.0 * 16.0 * push.iterations * 64.0 * groups;
        report("GPU_BENCH_FMA groups=%u wave32_ms=%.3f tflops32=%.2f wave64_ms=%.3f tflops64=%.2f\n",
               groups, t32, flops / t32 / 1e9, t64, flops / t64 / 1e9);
    }
    {
        /* Straight-line chains: a literal weight per FMA (like the baked-weight
         * network passes) versus 16 repeated weights with the same FMA count. */
        const double flops = 2.0 * GPU_BENCH_UNROLLED_FMAS * 64.0 * GROUPS;
        const double d32 = best(&unrolled32, NULL, 0, GROUPS, 1) - one;
        const double d64 = best(&unrolled64, NULL, 0, GROUPS, 1) - one;
        const double s32 = best(&small32, NULL, 0, GROUPS, 1) - one;
        report("GPU_BENCH_UNROLLED distinct_wave32_ms=%.3f tflops=%.2f distinct_wave64_ms=%.3f tflops=%.2f "
               "repeated_wave32_ms=%.3f tflops=%.2f\n", d32, flops / d32 / 1e9, d64, flops / d64 / 1e9,
               s32, flops / s32 / 1e9);
    }
    struct { uint32_t loads, stride, mask; } push = {64, READ_BYTES / 16 / 64, 0xffffffffu};
    const uint32_t read_groups = push.stride / 64;
    const double t1 = best(&reader, &push, sizeof(push), read_groups, 1);
    const double t4 = best(&reader, &push, sizeof(push), read_groups, 5);
    report("GPU_BENCH_READ bytes=%u one_ms=%.3f per_pass_ms=%.3f gbps=%.1f\n", READ_BYTES, t1,
           (t4 - t1) / 4.0, READ_BYTES / ((t4 - t1) / 4.0) / 1e6);
    /* Re-reads wrapped inside a region: 1 GB of loads per dispatch. */
    for (uint32_t region = 256u << 10; region <= (16u << 20); region *= 8) {
        const double gb = reread_gbps(&reader, region, read_groups);
        report("GPU_BENCH_REREAD region_kb=%u gbps=%.1f\n", region >> 10, gb);
    }
    /* Code-size sweep: distinct-weight straight-line kernels (128 bytes of
     * code per 16 FMAs), full grid for throughput and one group for latency. */
    for (unsigned i = 0; i < sizeof(gpu_bench_sweep) / sizeof(gpu_bench_sweep[0]); ++i) {
        struct kernel k32, k64;
        if (create_kernel(&k32, gpu_bench_sweep[i].spv, gpu_bench_sweep[i].bytes, 1, 0, 0, pool, out_only) ||
            create_kernel(&k64, gpu_bench_sweep[i].spv, gpu_bench_sweep[i].bytes, 1, 0,
                          offered.subgroupSizeControl ? 64 : 0, pool, out_only))
            return 1;
        const double fmas = 16.0 * gpu_bench_sweep[i].steps;
        const double t32 = best(&k32, NULL, 0, GROUPS, 1) - one;
        const double t64 = best(&k64, NULL, 0, GROUPS, 1) - one;
        const double w32 = best(&k32, NULL, 0, 1, 1) - one;
        const double w64 = best(&k64, NULL, 0, 1, 1) - one;
        report("GPU_BENCH_SWEEP code_kb=%u tflops32=%.2f tflops64=%.2f one_group32_us=%.1f one_group64_us=%.1f\n",
               gpu_bench_sweep[i].steps * 128u / 1024u, 2.0 * fmas * 64.0 * GROUPS / t32 / 1e9,
               2.0 * fmas * 64.0 * GROUPS / t64 / 1e9, w32 * 1000.0, w64 * 1000.0);
    }
    report("GPU_BENCH_DONE\n");
    return 0;
}

int main(void)
{
    static const char *const roots[] = {"/data/fsr4-results", "/app0/results", "/download0"};
    for (unsigned i = 0; !log_file && i < 3; ++i) {
        char path[128];
        mkdir(roots[i], 0777);
        snprintf(path, sizeof(path), "%s/gpu-bench-log.txt", roots[i]);
        log_file = fopen(path, "w");
    }
    int result = native_heap_init();
    report("GPU_BENCH_BEGIN heap=%d\n", result);
    if (!result) result = bench();
    report("GPU_BENCH_END result=%s\n", result ? "FAIL" : "PASS");
    if (log_file) fclose(log_file);
    for (;;) usleep(100000);
}
