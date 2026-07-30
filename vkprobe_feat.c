// Enumerate exactly which VkPhysicalDeviceVulkan11/12/13Features fields an
// implementation does NOT support. SwiftShader's vkCreateDevice returns
// VK_ERROR_FEATURE_NOT_PRESENT (-8) if a requested field is not in this set, so
// this list IS the candidate set for a DXVK device-creation failure.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define P(st, f) do { if (!(st).f) printf("    MISSING  %s\n", #f); else nsup++; } while (0)

int main(int argc, char **argv) {
    const char *lib = (argc > 1) ? argv[1] : "libvulkan.so";
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
    if (!gipa) { printf("no gipa\n"); return 1; }

    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo ai = {0}; ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ic = {0}; ic.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ic.pApplicationInfo = &ai;
    VkInstance inst; if (ci(&ic, NULL, &inst) != VK_SUCCESS) { printf("createInstance failed\n"); return 1; }

    PFN_vkEnumeratePhysicalDevices epd = (PFN_vkEnumeratePhysicalDevices)gipa(inst,"vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceFeatures2 gf2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(inst,"vkGetPhysicalDeviceFeatures2");
    if (!gf2) gf2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(inst,"vkGetPhysicalDeviceFeatures2KHR");
    if (!gf2) { printf("no GetPhysicalDeviceFeatures2\n"); return 1; }

    uint32_t n=0; epd(inst,&n,NULL);
    VkPhysicalDevice *pd = calloc(n,sizeof(*pd)); epd(inst,&n,pd);
    if (!n) { printf("no devices\n"); return 1; }

    VkPhysicalDeviceVulkan11Features f11 = {0}; f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceVulkan12Features f12 = {0}; f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceVulkan13Features f13 = {0}; f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f11.pNext = &f12; f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 f2 = {0}; f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2; f2.pNext = &f11;
    gf2(pd[0], &f2);

    int nsup = 0;
    printf("=== %s : unsupported feature fields ===\n", lib);
    printf("  --- core VkPhysicalDeviceFeatures ---\n");
    P(f2.features, robustBufferAccess);   P(f2.features, geometryShader);
    P(f2.features, tessellationShader);   P(f2.features, dualSrcBlend);
    P(f2.features, logicOp);              P(f2.features, fillModeNonSolid);
    P(f2.features, depthClamp);           P(f2.features, depthBiasClamp);
    P(f2.features, multiViewport);        P(f2.features, samplerAnisotropy);
    P(f2.features, sampleRateShading);    P(f2.features, shaderClipDistance);
    P(f2.features, shaderCullDistance);   P(f2.features, shaderImageGatherExtended);
    P(f2.features, pipelineStatisticsQuery);
    P(f2.features, occlusionQueryPrecise);
    P(f2.features, fragmentStoresAndAtomics);
    P(f2.features, vertexPipelineStoresAndAtomics);
    P(f2.features, shaderStorageImageWriteWithoutFormat);
    P(f2.features, shaderStorageImageReadWithoutFormat);
    P(f2.features, imageCubeArray);       P(f2.features, independentBlend);
    P(f2.features, drawIndirectFirstInstance);
    P(f2.features, textureCompressionBC);

    printf("  --- Vulkan11Features ---\n");
    P(f11, storageBuffer16BitAccess);     P(f11, uniformAndStorageBuffer16BitAccess);
    P(f11, multiview);                    P(f11, variablePointers);
    P(f11, variablePointersStorageBuffer); P(f11, shaderDrawParameters);
    P(f11, samplerYcbcrConversion);

    printf("  --- Vulkan12Features (DXVK prime suspects) ---\n");
    P(f12, samplerMirrorClampToEdge);     P(f12, drawIndirectCount);
    P(f12, storageBuffer8BitAccess);      P(f12, shaderInt8);
    P(f12, shaderFloat16);                P(f12, descriptorIndexing);
    P(f12, shaderSampledImageArrayNonUniformIndexing);
    P(f12, shaderStorageBufferArrayNonUniformIndexing);
    P(f12, descriptorBindingPartiallyBound);
    P(f12, runtimeDescriptorArray);       P(f12, samplerFilterMinmax);
    P(f12, scalarBlockLayout);            P(f12, imagelessFramebuffer);
    P(f12, uniformBufferStandardLayout);  P(f12, shaderSubgroupExtendedTypes);
    P(f12, separateDepthStencilLayouts);  P(f12, hostQueryReset);
    P(f12, timelineSemaphore);            P(f12, bufferDeviceAddress);
    P(f12, vulkanMemoryModel);            P(f12, shaderOutputViewportIndex);
    P(f12, shaderOutputLayer);            P(f12, subgroupBroadcastDynamicId);

    printf("  --- Vulkan13Features (DXVK prime suspects) ---\n");
    P(f13, robustImageAccess);            P(f13, inlineUniformBlock);
    P(f13, pipelineCreationCacheControl); P(f13, privateData);
    P(f13, shaderDemoteToHelperInvocation);
    P(f13, shaderTerminateInvocation);    P(f13, subgroupSizeControl);
    P(f13, computeFullSubgroups);         P(f13, synchronization2);
    P(f13, textureCompressionASTC_HDR);   P(f13, shaderZeroInitializeWorkgroupMemory);
    P(f13, dynamicRendering);             P(f13, shaderIntegerDotProduct);
    P(f13, maintenance4);

    printf("\n(%d of the checked fields ARE supported)\n", nsup);
    return 0;
}
