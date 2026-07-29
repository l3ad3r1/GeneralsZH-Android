// Milestone 0 probe: report the REAL Vulkan capability ceiling of the device,
// independent of the android.hardware.vulkan.* framework feature flags.
//
// Answers three questions that gate the TCL port plan:
//   1. What apiVersion does the driver actually expose? (DXVK 2.x needs 1.3)
//   2. Is textureCompressionBC supported? (Generals ships DXT/BC .dds textures)
//   3. Which device extensions exist? (DXVK 1.10.3 vs 2.x requirements)
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

static const char *devTypeStr(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "INTEGRATED_GPU";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "DISCRETE_GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "VIRTUAL_GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU";
        default:                                     return "OTHER";
    }
}

static void checkFormat(VkPhysicalDevice pd, VkFormat fmt, const char *name) {
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(pd, fmt, &fp);
    int sampled = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    printf("    %-34s sampled=%s  (optimalTilingFeatures=0x%08x)\n",
           name, sampled ? "YES" : "no ", fp.optimalTilingFeatures);
}

int main(void) {
    printf("=== VULKAN PROBE ===\n");

    // ---- instance-level version (1.1+ entry point; NULL on a 1.0 loader) ----
    uint32_t instVer = VK_API_VERSION_1_0;
    PFN_vkEnumerateInstanceVersion pEnumInstVer =
        (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
    if (pEnumInstVer && pEnumInstVer(&instVer) == VK_SUCCESS) {
        printf("Loader instance version: %u.%u.%u\n",
               VK_VERSION_MAJOR(instVer), VK_VERSION_MINOR(instVer), VK_VERSION_PATCH(instVer));
    } else {
        printf("Loader instance version: 1.0 (vkEnumerateInstanceVersion absent)\n");
    }

    // Ask for the highest instance version the loader advertises, so the driver
    // is never capped by our own apiVersion request.
    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vkprobe";
    app.apiVersion = instVer;

    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) {
        printf("FATAL: vkCreateInstance failed: %d\n", r);
        return 1;
    }

    uint32_t nDev = 0;
    vkEnumeratePhysicalDevices(inst, &nDev, NULL);
    printf("Physical devices: %u\n", nDev);
    if (!nDev) { printf("FATAL: no Vulkan devices\n"); return 1; }

    VkPhysicalDevice *devs = calloc(nDev, sizeof(*devs));
    vkEnumeratePhysicalDevices(inst, &nDev, devs);

    for (uint32_t i = 0; i < nDev; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);

        printf("\n--- device %u ---\n", i);
        printf("  name          : %s\n", p.deviceName);
        printf("  type          : %s\n", devTypeStr(p.deviceType));
        printf("  vendorID      : 0x%04x   deviceID: 0x%04x\n", p.vendorID, p.deviceID);
        printf("  *** apiVersion: %u.%u.%u ***\n",
               VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
               VK_VERSION_PATCH(p.apiVersion));
        printf("  driverVersion : %u (raw 0x%08x) = %u.%u.%u\n",
               p.driverVersion, p.driverVersion,
               VK_VERSION_MAJOR(p.driverVersion), VK_VERSION_MINOR(p.driverVersion),
               VK_VERSION_PATCH(p.driverVersion));
        printf("  maxImageDim2D : %u\n", p.limits.maxImageDimension2D);

        // driver identity (core in 1.2; via KHR ext on 1.1)
        if (VK_VERSION_MINOR(p.apiVersion) >= 1) {
            VkPhysicalDeviceDriverProperties dp = {0};
            dp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
            VkPhysicalDeviceProperties2 p2 = {0};
            p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            p2.pNext = &dp;
            PFN_vkGetPhysicalDeviceProperties2 f2 =
                (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties2");
            if (f2) {
                f2(devs[i], &p2);
                if (dp.driverName[0])
                    printf("  driverName    : %s\n  driverInfo    : %s\n", dp.driverName, dp.driverInfo);
            }
        }

        // ---- texture compression: the M2 risk item ----
        VkPhysicalDeviceFeatures f;
        vkGetPhysicalDeviceFeatures(devs[i], &f);
        printf("\n  FEATURES (port-relevant):\n");
        printf("    *** textureCompressionBC      : %s ***  <- Generals ships DXT/BC textures\n",
               f.textureCompressionBC ? "YES" : "NO");
        printf("    textureCompressionETC2        : %s\n", f.textureCompressionETC2 ? "YES" : "no");
        printf("    textureCompressionASTC_LDR    : %s\n", f.textureCompressionASTC_LDR ? "YES" : "no");
        printf("    geometryShader                : %s\n", f.geometryShader ? "YES" : "no");
        printf("    tessellationShader            : %s\n", f.tessellationShader ? "YES" : "no");
        printf("    depthClamp                    : %s\n", f.depthClamp ? "YES" : "no");
        printf("    depthBiasClamp                : %s\n", f.depthBiasClamp ? "YES" : "no");
        printf("    fillModeNonSolid              : %s\n", f.fillModeNonSolid ? "YES" : "no");
        printf("    samplerAnisotropy             : %s\n", f.samplerAnisotropy ? "YES" : "no");
        printf("    dualSrcBlend                  : %s\n", f.dualSrcBlend ? "YES" : "no");
        printf("    independentBlend              : %s\n", f.independentBlend ? "YES" : "no");
        printf("    multiViewport                 : %s\n", f.multiViewport ? "YES" : "no");
        printf("    shaderImageGatherExtended     : %s\n", f.shaderImageGatherExtended ? "YES" : "no");

        printf("\n  BC/DXT FORMAT SUPPORT (what the .big archives contain):\n");
        checkFormat(devs[i], VK_FORMAT_BC1_RGB_UNORM_BLOCK,  "BC1_RGB_UNORM  (DXT1)");
        checkFormat(devs[i], VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1_RGBA_UNORM (DXT1a)");
        checkFormat(devs[i], VK_FORMAT_BC2_UNORM_BLOCK,      "BC2_UNORM      (DXT3)");
        checkFormat(devs[i], VK_FORMAT_BC3_UNORM_BLOCK,      "BC3_UNORM      (DXT5)");
        printf("  fallback formats:\n");
        checkFormat(devs[i], VK_FORMAT_R8G8B8A8_UNORM,       "R8G8B8A8_UNORM (decoded RGBA)");
        checkFormat(devs[i], VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "ASTC_4x4_UNORM");
        checkFormat(devs[i], VK_FORMAT_B8G8R8A8_UNORM,       "B8G8R8A8_UNORM (swapchain)");

        // ---- device extensions ----
        uint32_t nExt = 0;
        vkEnumerateDeviceExtensionProperties(devs[i], NULL, &nExt, NULL);
        VkExtensionProperties *ext = calloc(nExt, sizeof(*ext));
        vkEnumerateDeviceExtensionProperties(devs[i], NULL, &nExt, ext);
        printf("\n  DEVICE EXTENSIONS: %u total\n", nExt);

        // extensions DXVK actually cares about (2.x needs the 1.3-core ones)
        static const char *want[] = {
            "VK_KHR_swapchain",
            "VK_KHR_dynamic_rendering",
            "VK_KHR_synchronization2",
            "VK_KHR_maintenance4",
            "VK_KHR_copy_commands2",
            "VK_KHR_format_feature_flags2",
            "VK_KHR_shader_float_controls",
            "VK_KHR_imageless_framebuffer",
            "VK_KHR_image_format_list",
            "VK_KHR_driver_properties",
            "VK_EXT_extended_dynamic_state",
            "VK_EXT_extended_dynamic_state2",
            "VK_EXT_extended_dynamic_state3",
            "VK_EXT_graphics_pipeline_library",
            "VK_EXT_transform_feedback",
            "VK_EXT_robustness2",
            "VK_EXT_vertex_attribute_divisor",
            "VK_EXT_depth_clip_enable",
            "VK_EXT_host_query_reset",
            "VK_EXT_memory_priority",
            "VK_EXT_shader_demote_to_helper_invocation",
            "VK_EXT_4444_formats",
            "VK_EXT_custom_border_color",
            "VK_EXT_non_seamless_cube_map",
            "VK_EXT_attachment_feedback_loop_layout",
            "VK_EXT_shader_module_identifier",
            "VK_EXT_texture_compression_astc_hdr",
            "VK_EXT_swapchain_maintenance1",
        };
        printf("  DXVK-relevant extension presence:\n");
        for (size_t w = 0; w < sizeof(want)/sizeof(want[0]); w++) {
            int found = 0;
            for (uint32_t e = 0; e < nExt; e++)
                if (!strcmp(ext[e].extensionName, want[w])) { found = 1; break; }
            printf("    [%s] %s\n", found ? "x" : " ", want[w]);
        }

        printf("  full extension list:\n");
        for (uint32_t e = 0; e < nExt; e++)
            printf("      %s (rev %u)\n", ext[e].extensionName, ext[e].specVersion);
        free(ext);

        // ---- memory heaps ----
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(devs[i], &mp);
        printf("\n  MEMORY HEAPS: %u\n", mp.memoryHeapCount);
        for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
            printf("    heap %u: %.2f GB  flags=0x%x%s\n", h,
                   mp.memoryHeaps[h].size / (1024.0*1024.0*1024.0),
                   mp.memoryHeaps[h].flags,
                   (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " DEVICE_LOCAL" : "");
    }

    free(devs);
    vkDestroyInstance(inst, NULL);
    printf("\n=== PROBE COMPLETE ===\n");
    return 0;
}
