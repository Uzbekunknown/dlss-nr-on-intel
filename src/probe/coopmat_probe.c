/*
 * coopmat_probe — enumerate VkCooperativeMatrixPropertiesKHR on this machine, and measure what
 * the matrix units do: the GPU report a release ships as nr_gpu_probe.exe.
 *
 * Resolves notes/CLAUDE.md open questions 1 and 3:
 *   1. the actual cooperative matrix configuration table (vulkaninfo does not print it)
 *   3. whether FP32 accumulation is available for the BF16 configs
 *
 * and, for each float16 8xNx16 -> float32 shape a device offers, at every subgroup size it
 * allows: whether a product comes out exact, and how fast the matrix and the vector units run
 * (coopmat_bench.comp, built as coopmat_bench8.spv and coopmat_bench16.spv and looked for beside
 * the program, then in work/ beside it). Arc A-series offers 8x8x16 where NR's kernels are
 * written for Xe2's 8x16x16, and this is what an A-series path is to be designed from. 8x16x16
 * is tried where it is not offered too, since NR's kernels run it there: whether the driver
 * gets it right is whether NR's picture is. Needs no weights and nothing else of the build; the
 * output also goes to nr-gpu-report.txt.
 *
 * Build: cc -O2 -Iwork/vulkan-headers/include -o work/coopmat_probe \
 *              src/probe/coopmat_probe.c -lvulkan
 */
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <wchar.h>
#else
#define _DEFAULT_SOURCE         /* readlink, under -std=c11 */
#include <unistd.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static FILE *report;
static int shader_missing;      /* a bench shader was not found, so the bench is incomplete */

/* Everything the probe says goes to the terminal and to the report file. */
static void say(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	if (report) {
		va_start(ap, fmt);
		vfprintf(report, fmt, ap);
		va_end(ap);
	}
}

/* Rotating buffers: several ctype() results are live in one printf call. */
static const char *ctype(VkComponentTypeKHR t)
{
	static char pool[8][24];
	static int slot;

	switch (t) {
	case VK_COMPONENT_TYPE_FLOAT16_KHR:  return "fp16";
	case VK_COMPONENT_TYPE_FLOAT32_KHR:  return "fp32";
	case VK_COMPONENT_TYPE_FLOAT64_KHR:  return "fp64";
	case VK_COMPONENT_TYPE_BFLOAT16_KHR: return "bf16";
	case VK_COMPONENT_TYPE_SINT8_KHR:    return "sint8";
	case VK_COMPONENT_TYPE_SINT16_KHR:   return "sint16";
	case VK_COMPONENT_TYPE_SINT32_KHR:   return "sint32";
	case VK_COMPONENT_TYPE_SINT64_KHR:   return "sint64";
	case VK_COMPONENT_TYPE_UINT8_KHR:    return "uint8";
	case VK_COMPONENT_TYPE_UINT16_KHR:   return "uint16";
	case VK_COMPONENT_TYPE_UINT32_KHR:   return "uint32";
	case VK_COMPONENT_TYPE_UINT64_KHR:   return "uint64";
	case VK_COMPONENT_TYPE_FLOAT8_E4M3_EXT: return "e4m3";
	case VK_COMPONENT_TYPE_FLOAT8_E5M2_EXT: return "e5m2";
	case VK_COMPONENT_TYPE_SINT8_PACKED_NV: return "sint8p";
	case VK_COMPONENT_TYPE_UINT8_PACKED_NV: return "uint8p";
	default:
		break;
	}
	slot = (slot + 1) % 8;
	snprintf(pool[slot], sizeof pool[slot], "enum:%d", (int)t);
	return pool[slot];
}

static const char *scope_name(VkScopeKHR s)
{
	switch (s) {
	case VK_SCOPE_DEVICE_KHR:       return "device";
	case VK_SCOPE_WORKGROUP_KHR:    return "workgroup";
	case VK_SCOPE_SUBGROUP_KHR:     return "subgroup";
	case VK_SCOPE_QUEUE_FAMILY_KHR: return "queuefamily";
	default:                        return "?";
	}
}

static void print_stages(VkShaderStageFlags f)
{
	static const struct { VkShaderStageFlagBits bit; const char *name; } tbl[] = {
		{ VK_SHADER_STAGE_VERTEX_BIT,   "vertex"   },
		{ VK_SHADER_STAGE_FRAGMENT_BIT, "fragment" },
		{ VK_SHADER_STAGE_COMPUTE_BIT,  "compute"  },
		{ VK_SHADER_STAGE_TASK_BIT_EXT, "task"     },
		{ VK_SHADER_STAGE_MESH_BIT_EXT, "mesh"     },
	};
	int first = 1;

	for (size_t i = 0; i < sizeof tbl / sizeof tbl[0]; i++) {
		if (f & tbl[i].bit) {
			say("%s%s", first ? "" : "|", tbl[i].name);
			first = 0;
		}
	}
	if (first)
		say("(none)");
}

static int has_ext(VkExtensionProperties *e, uint32_t n, const char *want)
{
	for (uint32_t i = 0; i < n; i++)
		if (!strcmp(e[i].extensionName, want))
			return 1;
	return 0;
}

/* ---- The bench ------------------------------------------------------------------------------- */

enum { BM = 8, BK = 16, KT = 64, D_FLOATS = 1 << 21, RATE_GROUPS = 8192, VECTOR_LANES = 1 << 20 };

struct bench {
	VkDevice dev;
	VkQueue queue;
	VkBuffer buf[3];
	VkDeviceMemory mem[3];
	void *map[3];
	VkDescriptorSetLayout dsl;
	VkPipelineLayout layout;
	VkDescriptorPool pool;
	VkDescriptorSet set;
	VkCommandPool cpool;
	VkCommandBuffer cmd;
	VkFence fence;
	VkQueryPool query;
	double period;      /* nanoseconds a timestamp tick */
	uint64_t mask;      /* the bits a timestamp holds */
	int full;           /* computeFullSubgroups */
	int lost;           /* a dispatch failed or ran past 10 s: nothing more is sent */
};

/* Exact for what the probe writes: zero and normal halves, small multiples of 1/4. */
static uint16_t half_of(float f)
{
	uint32_t x;

	memcpy(&x, &f, 4);
	uint32_t sign = (x >> 16) & 0x8000, e = (x >> 23) & 0xff;
	return e ? (uint16_t)(sign | ((e - 112) << 10) | ((x & 0x7fffff) >> 13)) : (uint16_t)sign;
}

static float a_value(int i, int k) { return (float)((i * 7 + k * 3) % 5 - 2) * 0.5f; }
static float b_value(int k, int j) { return (float)((k * 5 + j * 3) % 7 - 3) * 0.25f; }

/* The shader beside this program (a build's work/) or in work/ beside it (a release). Windows gives
 * the program's path in UTF-16: its ANSI form turns a folder outside the code page, Cyrillic on an
 * English Windows say, into question marks, and nothing is found there. */
static FILE *open_beside(const char *name)
{
	static const char *const where[] = { "", "work/" };
#ifdef _WIN32
	wchar_t path[4096], full[4200];
	DWORD n = GetModuleFileNameW(NULL, path, 4096);
	if (n >= 4096)
		n = 0;
	path[n] = 0;
	wchar_t *slash = wcsrchr(path, L'\\'), *forward = wcsrchr(path, L'/');
	if (!slash || (forward && forward > slash))
		slash = forward;
	size_t dir = slash ? (size_t)(slash - path) + 1 : 0;
	for (int i = 0; i < 2; i++) {
		char rest[256];
		snprintf(rest, sizeof rest, "%s%s", where[i], name);
		wmemcpy(full, path, dir);
		if (!MultiByteToWideChar(CP_UTF8, 0, rest, -1, full + dir, (int)(4200 - dir)))
			continue;
		FILE *f = _wfopen(full, L"rb");
		if (f)
			return f;
	}
	return NULL;
#else
	char path[4096];
	ssize_t got = readlink("/proc/self/exe", path, sizeof path - 1);
	size_t n = got > 0 ? (size_t)got : 0;
	path[n] = 0;
	char *slash = strrchr(path, '/');
	int dir = slash ? (int)(slash - path) : 0;
	for (int i = 0; i < 2; i++) {
		char full[4200];
		snprintf(full, sizeof full, "%.*s%s%s%s", dir, path, slash ? "/" : "", where[i], name);
		FILE *f = fopen(full, "rb");
		if (f)
			return f;
	}
	return NULL;
#endif
}

static VkShaderModule load_module(struct bench *b, const char *name)
{
	FILE *f = open_beside(name);
	if (!f)
		return VK_NULL_HANDLE;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint32_t *code = size > 0 ? malloc((size_t)size) : NULL;
	size_t got = code ? fread(code, 1, (size_t)size, f) : 0;
	fclose(f);
	VkShaderModule mod = VK_NULL_HANDLE;
	if (got == (size_t)size && size % 4 == 0) {
		VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
						.codeSize = (size_t)size, .pCode = code };
		if (vkCreateShaderModule(b->dev, &ci, NULL, &mod) != VK_SUCCESS)
			mod = VK_NULL_HANDLE;
	}
	free(code);
	return mod;
}

static void bench_close(struct bench *b)
{
	if (!b->dev)
		return;
	vkDeviceWaitIdle(b->dev);
	if (b->query) vkDestroyQueryPool(b->dev, b->query, NULL);
	if (b->fence) vkDestroyFence(b->dev, b->fence, NULL);
	if (b->cpool) vkDestroyCommandPool(b->dev, b->cpool, NULL);
	if (b->pool) vkDestroyDescriptorPool(b->dev, b->pool, NULL);
	if (b->layout) vkDestroyPipelineLayout(b->dev, b->layout, NULL);
	if (b->dsl) vkDestroyDescriptorSetLayout(b->dev, b->dsl, NULL);
	for (int i = 0; i < 3; i++) {
		if (b->buf[i]) vkDestroyBuffer(b->dev, b->buf[i], NULL);
		if (b->mem[i]) vkFreeMemory(b->dev, b->mem[i], NULL);
	}
	vkDestroyDevice(b->dev, NULL);
	b->dev = VK_NULL_HANDLE;
}

/* A device of its own with exactly what the shader needs. NULL, or why not. */
static const char *bench_open(struct bench *b, VkPhysicalDevice pd)
{
	memset(b, 0, sizeof *b);
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	if (props.apiVersion < VK_API_VERSION_1_3)
		return "the device offers Vulkan below 1.3";
	b->period = props.limits.timestampPeriod;

	uint32_t nq = 0, family = UINT32_MAX;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
	VkQueueFamilyProperties *q = calloc(nq ? nq : 1, sizeof *q);
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, q);
	for (uint32_t i = 0; i < nq && family == UINT32_MAX; i++)
		if ((q[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && q[i].timestampValidBits) {
			family = i;
			b->mask = q[i].timestampValidBits >= 64 ? ~0ull : (1ull << q[i].timestampValidBits) - 1;
		}
	free(q);
	if (family == UINT32_MAX)
		return "no compute queue with timestamps";

	VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR };
	VkPhysicalDeviceVulkan11Features v11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
						 .pNext = &cm };
	VkPhysicalDeviceVulkan12Features v12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
						 .pNext = &v11 };
	VkPhysicalDeviceVulkan13Features v13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
						 .pNext = &v12 };
	VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v13 };
	vkGetPhysicalDeviceFeatures2(pd, &f2);
	if (!cm.cooperativeMatrix || !v12.shaderFloat16 || !v11.storageBuffer16BitAccess || !v13.subgroupSizeControl)
		return "it lacks cooperativeMatrix, shaderFloat16, 16-bit storage or subgroupSizeControl";

	VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm_on = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, .cooperativeMatrix = VK_TRUE };
	VkPhysicalDeviceVulkan11Features v11_on = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
						    .pNext = &cm_on, .storageBuffer16BitAccess = VK_TRUE };
	VkPhysicalDeviceVulkan12Features v12_on = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
						    .pNext = &v11_on, .shaderFloat16 = VK_TRUE,
						    .vulkanMemoryModel = v12.vulkanMemoryModel,
						    .vulkanMemoryModelDeviceScope = v12.vulkanMemoryModelDeviceScope };
	VkPhysicalDeviceVulkan13Features v13_on = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
						    .pNext = &v12_on, .subgroupSizeControl = VK_TRUE,
						    .computeFullSubgroups = v13.computeFullSubgroups };
	b->full = v13.computeFullSubgroups;
	float priority = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
					.queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority };
	const char *ext[] = { VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &v13_on,
				   .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
				   .enabledExtensionCount = 1, .ppEnabledExtensionNames = ext };
	if (vkCreateDevice(pd, &dci, NULL, &b->dev) != VK_SUCCESS)
		return "vkCreateDevice failed";
	vkGetDeviceQueue(b->dev, family, 0, &b->queue);

	/* A few megabytes, host-visible: where they live does not matter to what is measured. */
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	const VkDeviceSize sizes[3] = { BM * KT * 2, KT * 16 * 2, (VkDeviceSize)D_FLOATS * 4 };
	const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (int i = 0; i < 3; i++) {
		VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizes[i],
					   .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
		if (vkCreateBuffer(b->dev, &bci, NULL, &b->buf[i]) != VK_SUCCESS)
			return "vkCreateBuffer failed";
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(b->dev, b->buf[i], &req);
		uint32_t type = UINT32_MAX;
		for (uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; t++)
			if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want)
				type = t;
		if (type == UINT32_MAX)
			return "no host-visible memory";
		VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
					     .allocationSize = req.size, .memoryTypeIndex = type };
		if (vkAllocateMemory(b->dev, &mai, NULL, &b->mem[i]) != VK_SUCCESS
		    || vkBindBufferMemory(b->dev, b->buf[i], b->mem[i], 0) != VK_SUCCESS
		    || vkMapMemory(b->dev, b->mem[i], 0, VK_WHOLE_SIZE, 0, &b->map[i]) != VK_SUCCESS)
			return "buffer memory failed";
	}

	VkDescriptorSetLayoutBinding bind[3];
	VkDescriptorBufferInfo info[3];
	VkWriteDescriptorSet write[3];
	for (uint32_t i = 0; i < 3; i++) {
		bind[i] = (VkDescriptorSetLayoutBinding){ .binding = i, .descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
		info[i] = (VkDescriptorBufferInfo){ .buffer = b->buf[i], .range = VK_WHOLE_SIZE };
	}
	VkDescriptorSetLayoutCreateInfo dlci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
						 .bindingCount = 3, .pBindings = bind };
	VkPushConstantRange push = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 4 };
	VkDescriptorPoolSize ps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 3 };
	VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
					    .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
	if (vkCreateDescriptorSetLayout(b->dev, &dlci, NULL, &b->dsl) != VK_SUCCESS)
		return "vkCreateDescriptorSetLayout failed";
	VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
					    .setLayoutCount = 1, .pSetLayouts = &b->dsl,
					    .pushConstantRangeCount = 1, .pPushConstantRanges = &push };
	if (vkCreatePipelineLayout(b->dev, &plci, NULL, &b->layout) != VK_SUCCESS
	    || vkCreateDescriptorPool(b->dev, &dpci, NULL, &b->pool) != VK_SUCCESS)
		return "the pipeline layout failed";
	VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
					     .descriptorPool = b->pool, .descriptorSetCount = 1, .pSetLayouts = &b->dsl };
	if (vkAllocateDescriptorSets(b->dev, &dsai, &b->set) != VK_SUCCESS)
		return "vkAllocateDescriptorSets failed";
	for (uint32_t i = 0; i < 3; i++)
		write[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = b->set,
			.dstBinding = i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &info[i] };
	vkUpdateDescriptorSets(b->dev, 3, write, 0, NULL);

	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
					 .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
					 .queueFamilyIndex = family };
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkQueryPoolCreateInfo qpci = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
				       .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 };
	if (vkCreateCommandPool(b->dev, &cpci, NULL, &b->cpool) != VK_SUCCESS
	    || vkCreateFence(b->dev, &fci, NULL, &b->fence) != VK_SUCCESS
	    || vkCreateQueryPool(b->dev, &qpci, NULL, &b->query) != VK_SUCCESS)
		return "the command pool, fence or query pool failed";
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
					     .commandPool = b->cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
					     .commandBufferCount = 1 };
	if (vkAllocateCommandBuffers(b->dev, &cbai, &b->cmd) != VK_SUCCESS)
		return "vkAllocateCommandBuffers failed";

	uint16_t *a = b->map[0];
	for (int i = 0; i < BM; i++)
		for (int k = 0; k < KT; k++)
			a[i * KT + k] = half_of(a_value(i, k));
	return NULL;
}

static VkPipeline bench_pipeline(struct bench *b, VkShaderModule mod, uint32_t subgroup, uint32_t mode,
				 VkResult *result)
{
	const uint32_t spec[2] = { subgroup, mode };
	const VkSpecializationMapEntry map[2] = { { 0, 0, 4 }, { 1, 4, 4 } };
	const VkSpecializationInfo si = { 2, map, sizeof spec, spec };
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfo width = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO,
		.requiredSubgroupSize = subgroup };
	VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = &width,
			   .flags = b->full ? VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT : 0,
			   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main",
			   .pSpecializationInfo = &si },
		.layout = b->layout };
	VkPipeline p = VK_NULL_HANDLE;
	*result = vkCreateComputePipelines(b->dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
	return *result == VK_SUCCESS ? p : VK_NULL_HANDLE;
}

/* One dispatch, timed by the GPU: milliseconds, or -1 when it did not finish. */
static double bench_run(struct bench *b, VkPipeline p, uint32_t groups, uint32_t iters)
{
	if (b->lost)
		return -1;
	vkResetFences(b->dev, 1, &b->fence);
	vkResetCommandBuffer(b->cmd, 0);
	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
					.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	vkBeginCommandBuffer(b->cmd, &bi);
	vkCmdResetQueryPool(b->cmd, b->query, 0, 2);
	vkCmdBindPipeline(b->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
	vkCmdBindDescriptorSets(b->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b->layout, 0, 1, &b->set, 0, NULL);
	vkCmdPushConstants(b->cmd, b->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &iters);
	vkCmdWriteTimestamp(b->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, b->query, 0);
	vkCmdDispatch(b->cmd, groups, 1, 1);
	vkCmdWriteTimestamp(b->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, b->query, 1);
	VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
			       .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
	vkCmdPipelineBarrier(b->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
			     1, &mb, 0, NULL, 0, NULL);
	vkEndCommandBuffer(b->cmd);
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
			    .pCommandBuffers = &b->cmd };
	uint64_t ts[2];
	if (vkQueueSubmit(b->queue, 1, &si, b->fence) != VK_SUCCESS
	    || vkWaitForFences(b->dev, 1, &b->fence, VK_TRUE, 10ull * 1000 * 1000 * 1000) != VK_SUCCESS
	    || vkGetQueryPoolResults(b->dev, b->query, 0, 2, sizeof ts, ts, 8,
				     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) {
		b->lost = 1;
		return -1;
	}
	return (double)((ts[1] - ts[0]) & b->mask) * b->period / 1e6;
}

/* GFLOP/s of a rate pipeline, run for about 20 ms, best of eight: the GPU's clock climbs through
 * the first runs, and the best is the one at full clock. -1 when it did not run. */
static double bench_rate(struct bench *b, VkPipeline p, uint32_t groups, double flops_per_iter)
{
	uint32_t iters = 16;
	double ms = bench_run(b, p, groups, iters);
	if (ms <= 0)
		return -1;
	if (ms < 20.0) {
		double want = iters * 20.0 / ms;
		iters = want > (double)(1u << 20) ? 1u << 20 : (uint32_t)want;
	}
	double best = -1;
	for (int i = 0; i < 8; i++) {
		double t = bench_run(b, p, groups, iters);
		if (t > 0 && (best < 0 || t < best))
			best = t;
	}
	return best > 0 ? flops_per_iter * iters / (best * 1e6) : -1;
}

static void bench_device(VkPhysicalDevice pd, int has8, int has16)
{
	struct bench b;
	say("\n  bench (one subgroup a workgroup, timed by the GPU):\n");
	const char *why = bench_open(&b, pd);
	if (why) {
		say("    not run: %s\n", why);
		bench_close(&b);
		return;
	}
	VkPhysicalDeviceVulkan13Properties p13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES };
	VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p13 };
	vkGetPhysicalDeviceProperties2(pd, &p2);
	int pick = (p13.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
	say("    subgroup sizes %u-%u%s; shared memory %u bytes a workgroup\n", p13.minSubgroupSize,
	    p13.maxSubgroupSize, pick ? "" : ", but a compute shader cannot ask for one",
	    p2.properties.limits.maxComputeSharedMemorySize);
	uint32_t sizes[3];
	int nsizes = 0;
	for (uint32_t s = 8; s <= 32 && pick; s *= 2)
		if (s >= p13.minSubgroupSize && s <= p13.maxSubgroupSize)
			sizes[nsizes++] = s;

	int ours = 0;                       /* NR's kernels: 8x16x16 at subgroup 32, exact */
	int unchecked = 0;                  /* 8x16x16 wanted, but its shader is missing */
	VkShaderModule kept = VK_NULL_HANDLE;
	for (int shape = 0; shape < 2; shape++) {
		const int n = shape ? 16 : 8;
		const char *spv = shape ? "coopmat_bench16.spv" : "coopmat_bench8.spv";
		if (!shape && !has8) {
			say("    matrix 8x8x16: not offered\n");
			continue;
		}
		/* NR's kernels run 8x16x16 whether the device offers it or not, so it is tried either
		 * way: a driver may run a shape it does not offer, right or wrong (Intel's computes
		 * 8x8x16 wrong on Xe2), and what it gives is what NR gets on an A-series card. The
		 * report is saved first, in case the driver does not survive it. */
		if (shape && !has16) {
			say("    matrix 8x16x16: not offered; tried anyway, as NR's kernels run it\n");
			fflush(stdout);
			if (report)
				fflush(report);
		}
		VkShaderModule mod = load_module(&b, spv);
		if (!mod) {
			say("    matrix 8x%dx16: %s was not found beside the probe or in work/\n", n, spv);
			shader_missing = 1;
			unchecked |= n == 16;
			continue;
		}
		uint16_t *bh = b.map[1];
		float ref[BM * 16];
		for (int k = 0; k < KT; k++)
			for (int j = 0; j < n; j++)
				bh[k * n + j] = half_of(b_value(k, j));
		for (int i = 0; i < BM; i++)
			for (int j = 0; j < n; j++) {
				float sum = 0;
				for (int k = 0; k < KT; k++)
					sum += a_value(i, k) * b_value(k, j);
				ref[i * n + j] = sum;
			}
		for (int s = 0; s < nsizes; s++) {
			VkResult res;
			VkPipeline check = bench_pipeline(&b, mod, sizes[s], 0, &res);
			if (!check) {
				say("    matrix 8x%dx16, subgroup %2u: the pipeline was refused (%d)\n", n, sizes[s], res);
				continue;
			}
			memset(b.map[2], 0, BM * 16 * sizeof(float));
			double ms = bench_run(&b, check, 1, 1);
			vkDestroyPipeline(b.dev, check, NULL);
			if (ms < 0) {
				say("    matrix 8x%dx16, subgroup %2u: the product did not finish\n", n, sizes[s]);
				break;
			}
			const float *got = b.map[2];
			int bad = 0;
			float worst = 0;
			for (int e = 0; e < BM * n; e++) {
				float off = got[e] > ref[e] ? got[e] - ref[e] : ref[e] - got[e];
				if (got[e] != ref[e]) {
					bad++;
					if (!(off <= worst))
						worst = off;
				}
			}
			if (bad) {
				say("    matrix 8x%dx16, subgroup %2u: WRONG, %d of %d values (off by up to %g)\n",
				    n, sizes[s], bad, BM * n, worst);
				continue;
			}
			VkPipeline rate = bench_pipeline(&b, mod, sizes[s], 1, &res);
			double gflops = rate ? bench_rate(&b, rate, RATE_GROUPS, 2.0 * BM * n * BK * 4 * RATE_GROUPS) : -1;
			if (rate)
				vkDestroyPipeline(b.dev, rate, NULL);
			if (gflops > 0)
				say("    matrix 8x%dx16, subgroup %2u: exact, %.0f GFLOP/s\n", n, sizes[s], gflops);
			else
				say("    matrix 8x%dx16, subgroup %2u: exact; the rate was not measured\n", n, sizes[s]);
			ours |= n == 16 && sizes[s] == 32;
		}
		if (kept)
			vkDestroyShaderModule(b.dev, mod, NULL);
		else
			kept = mod;
	}
	/* The vector units, which a path without the matrix units would run on. */
	for (int s = 0; kept && s < nsizes; s++) {
		VkResult res;
		VkPipeline rate = bench_pipeline(&b, kept, sizes[s], 2, &res);
		double gflops = rate ? bench_rate(&b, rate, VECTOR_LANES / sizes[s], 16.0 * VECTOR_LANES) : -1;
		if (rate)
			vkDestroyPipeline(b.dev, rate, NULL);
		if (gflops > 0)
			say("    vector float32 FMA, subgroup %2u: %.0f GFLOP/s\n", sizes[s], gflops);
		else
			say("    vector float32 FMA, subgroup %2u: not measured (%d)\n", sizes[s], res);
	}
	if (kept)
		vkDestroyShaderModule(b.dev, kept, NULL);
	if (b.lost)
		say("    a dispatch failed or ran past 10 s, so nothing more was measured\n");
	say("    NR's kernels (float16 8x16x16 at subgroup 32): %s\n",
	    ours ? (has16 ? "run here" : "exact here, though the GPU does not offer the shape")
		 : unchecked ? "not checked, without coopmat_bench16.spv"
			     : "do NOT run here; please send this report");
	bench_close(&b);
}

/* Where the report went, in full, on the terminal only: the file is for posting, and a full path
 * carries the user's name. Windows names it through the wide API, so a folder in Cyrillic or
 * Chinese reaches the console whatever its code page; a redirected stdout takes UTF-8. */
static void say_where(void)
{
	if (!report) {
		say("\nnr-gpu-report.txt could not be written in this folder: copy the text above instead.\n");
		return;
	}
	fputs("\n", report);
#ifdef _WIN32
	wchar_t wide[1024];
	char utf8[3 * 1024];
	DWORD n = GetFullPathNameW(L"nr-gpu-report.txt", 1024, wide, NULL);
	if (n && n < 1024 && WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, (int)sizeof utf8, NULL, NULL)) {
		HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
		DWORD mode, wrote;
		if (GetConsoleMode(out, &mode)) {
			printf("\nThis report is also in ");
			fflush(stdout);
			WriteConsoleW(out, wide, n, &wrote, NULL);
			printf("\n");
		} else {
			printf("\nThis report is also in %s\n", utf8);
		}
		return;
	}
	printf("\nThis report is also in nr-gpu-report.txt\n");
#else
	char *full = realpath("nr-gpu-report.txt", NULL);
	printf("\nThis report is also in %s\n", full ? full : "nr-gpu-report.txt");
	free(full);
#endif
}

/* Every way out: a double click gives the probe a console of its own, which closes with it, so
 * it stays open until Enter, a failure included. */
static int finish(int code)
{
	if (report)
		fclose(report);
	report = NULL;
#ifdef _WIN32
	DWORD procs[2];
	if (GetConsoleProcessList(procs, 2) == 1) {   /* a console of its own: started by a double click */
		printf("\nPress Enter to close.");
		getchar();
	}
#endif
	return code;
}

int main(void)
{
	report = fopen("nr-gpu-report.txt", "w");
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
				  .pApplicationName = "coopmat_probe",
				  .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
				     .pApplicationInfo = &app };
	VkInstance inst;
	VkResult r = vkCreateInstance(&ici, NULL, &inst);

	if (r != VK_SUCCESS) {
		say("vkCreateInstance failed: %d. No Vulkan driver answered: is the GPU's driver installed?\n", r);
		return finish(1);
	}

	PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR get_coopmat =
		(PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)
		vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
	PFN_vkGetPhysicalDeviceCooperativeMatrixFlexibleDimensionsPropertiesNV get_flex =
		(PFN_vkGetPhysicalDeviceCooperativeMatrixFlexibleDimensionsPropertiesNV)
		vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceCooperativeMatrixFlexibleDimensionsPropertiesNV");

	uint32_t ndev = 0;
	vkEnumeratePhysicalDevices(inst, &ndev, NULL);
	VkPhysicalDevice *devs = calloc(ndev ? ndev : 1, sizeof *devs);
	vkEnumeratePhysicalDevices(inst, &ndev, devs);
	say("physical devices: %u\n", ndev);

	for (uint32_t d = 0; d < ndev; d++) {
		VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
		VkPhysicalDeviceCooperativeMatrixPropertiesKHR cmp = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_PROPERTIES_KHR };
		VkPhysicalDeviceSubgroupProperties sgp = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, .pNext = &cmp };
		VkPhysicalDeviceDriverProperties drv = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, .pNext = &sgp };

		p2.pNext = &drv;
		vkGetPhysicalDeviceProperties2(devs[d], &p2);

		uint32_t next = 0;
		vkEnumerateDeviceExtensionProperties(devs[d], NULL, &next, NULL);
		VkExtensionProperties *exts = calloc(next ? next : 1, sizeof *exts);
		vkEnumerateDeviceExtensionProperties(devs[d], NULL, &next, exts);

		int have_cm   = has_ext(exts, next, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
		int have_bf16 = has_ext(exts, next, VK_KHR_SHADER_BFLOAT16_EXTENSION_NAME);
		int have_cm2  = has_ext(exts, next, VK_NV_COOPERATIVE_MATRIX_2_EXTENSION_NAME);

		say("\n=== device %u: %s ===\n", d, p2.properties.deviceName);
		say("  apiVersion    : %u.%u.%u\n", VK_API_VERSION_MAJOR(p2.properties.apiVersion),
		       VK_API_VERSION_MINOR(p2.properties.apiVersion),
		       VK_API_VERSION_PATCH(p2.properties.apiVersion));
		say("  driver        : %s, %s\n", drv.driverName, drv.driverInfo);
		say("  subgroup size : %u\n", sgp.subgroupSize);
		say("  extensions    : KHR_cooperative_matrix=%d KHR_shader_bfloat16=%d NV_cooperative_matrix2=%d\n",
		       have_cm, have_bf16, have_cm2);

		VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
		VkPhysicalDeviceCooperativeMatrixFeaturesKHR cmf = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR };
		VkPhysicalDeviceShaderBfloat16FeaturesKHR bff = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_BFLOAT16_FEATURES_KHR };

		f2.pNext = &cmf;
		if (have_bf16)
			cmf.pNext = &bff;
		vkGetPhysicalDeviceFeatures2(devs[d], &f2);

		say("  cooperativeMatrix              = %u\n", cmf.cooperativeMatrix);
		say("  cooperativeMatrixRobustBufferAccess = %u\n", cmf.cooperativeMatrixRobustBufferAccess);
		say("  supportedStages                = ");
		print_stages(cmp.cooperativeMatrixSupportedStages);
		say("\n");
		if (have_bf16) {
			say("  shaderBFloat16Type             = %u\n", bff.shaderBFloat16Type);
			say("  shaderBFloat16CooperativeMatrix= %u\n", bff.shaderBFloat16CooperativeMatrix);
			say("  shaderBFloat16DotProduct       = %u\n", bff.shaderBFloat16DotProduct);
		}

		if (!have_cm || !get_coopmat) {
			say("  -- no KHR cooperative matrix here --\n");
			free(exts);
			continue;
		}

		int has8 = 0, has16 = 0;     /* float16 8xNx16 -> float32 at subgroup scope */
		uint32_t n = 0;
		r = get_coopmat(devs[d], &n, NULL);
		say("\n  vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR -> %d, count=%u\n", r, n);
		if (r == VK_SUCCESS && n) {
			VkCooperativeMatrixPropertiesKHR *props = calloc(n, sizeof *props);
			for (uint32_t i = 0; i < n; i++)
				props[i].sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR;
			r = get_coopmat(devs[d], &n, props);

			say("\n   idx   M   N   K   A      B      C      Result  satAcc  scope\n");
			say("   ---------------------------------------------------------------\n");
			for (uint32_t i = 0; i < n; i++)
				say("   %3u %3u %3u %3u   %-6s %-6s %-6s %-6s  %-6u %s\n", i,
				       props[i].MSize, props[i].NSize, props[i].KSize,
				       ctype(props[i].AType), ctype(props[i].BType),
				       ctype(props[i].CType), ctype(props[i].ResultType),
				       props[i].saturatingAccumulation, scope_name(props[i].scope));
			for (uint32_t i = 0; i < n; i++)
				if (props[i].MSize == 8 && props[i].KSize == 16 && props[i].scope == VK_SCOPE_SUBGROUP_KHR
				    && props[i].AType == VK_COMPONENT_TYPE_FLOAT16_KHR
				    && props[i].BType == VK_COMPONENT_TYPE_FLOAT16_KHR
				    && props[i].CType == VK_COMPONENT_TYPE_FLOAT32_KHR
				    && props[i].ResultType == VK_COMPONENT_TYPE_FLOAT32_KHR) {
					has8 |= props[i].NSize == 8;
					has16 |= props[i].NSize == 16;
				}

			/* Open question 3: FP32 accumulation for BF16 inputs. */
			unsigned bf16 = 0, bf16_fp32acc = 0, fp16_fp32acc = 0;
			for (uint32_t i = 0; i < n; i++) {
				if (props[i].AType == VK_COMPONENT_TYPE_BFLOAT16_KHR) {
					bf16++;
					if (props[i].CType == VK_COMPONENT_TYPE_FLOAT32_KHR &&
					    props[i].ResultType == VK_COMPONENT_TYPE_FLOAT32_KHR)
						bf16_fp32acc++;
				}
				if (props[i].AType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
				    props[i].CType == VK_COMPONENT_TYPE_FLOAT32_KHR)
					fp16_fp32acc++;
			}
			say("\n   summary: %u configs total; %u with A=bf16, of those %u accumulate in fp32; "
			       "%u fp16 configs accumulate in fp32\n", n, bf16, bf16_fp32acc, fp16_fp32acc);
			free(props);
		}

		if (have_cm2 && get_flex) {
			uint32_t nf = 0;
			r = get_flex(devs[d], &nf, NULL);
			say("\n  NV_cooperative_matrix2 flexible dimensions -> %d, count=%u\n", r, nf);
			if (r == VK_SUCCESS && nf) {
				VkCooperativeMatrixFlexibleDimensionsPropertiesNV *fp = calloc(nf, sizeof *fp);
				for (uint32_t i = 0; i < nf; i++)
					fp[i].sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_FLEXIBLE_DIMENSIONS_PROPERTIES_NV;
				get_flex(devs[d], &nf, fp);
				say("\n   idx  Mgran Ngran Kgran  A      B      C      Result  scope    wgsize saturate\n");
				say("   ------------------------------------------------------------------------------\n");
				for (uint32_t i = 0; i < nf; i++)
					say("   %3u  %5u %5u %5u  %-6s %-6s %-6s %-6s  %-8s %-6u %u\n", i,
					       fp[i].MGranularity, fp[i].NGranularity, fp[i].KGranularity,
					       ctype(fp[i].AType), ctype(fp[i].BType), ctype(fp[i].CType),
					       ctype(fp[i].ResultType), scope_name(fp[i].scope),
					       fp[i].workgroupInvocations, fp[i].saturatingAccumulation);
				free(fp);
			}
		}
		bench_device(devs[d], has8, has16);
		free(exts);
	}
	vkDestroyInstance(inst, NULL);
	if (shader_missing)
		say("\nNot everything was measured: coopmat_bench8.spv and coopmat_bench16.spv belong beside\n"
		    "the probe or in the work folder beside it. From a release, unpack the whole ZIP first and\n"
		    "run the probe in that folder.\n");
	say_where();
	say("For an Arc A-series card, please post it with the template at\n"
	    "https://github.com/Uzbekunknown/dlss-nr-on-intel/issues/new?template=arc-a-series.yml\n");
	return finish(0);
}
