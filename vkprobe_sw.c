// Probe an ARBITRARY Vulkan implementation by dlopen'ing it (e.g. SwiftShader),
// instead of the system loader. Answers whether the bundled libvk_sw.so gives
// DXVK what it needs: Vulkan 1.3, BC textures, and VK_KHR_android_surface.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *libpath = (argc > 1) ? argv[1] : "libvulkan.so";
    void *h = dlopen(libpath, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen(%s) FAILED: %s\n", libpath, dlerror()); return 1; }
    printf("=== probing %s ===\n", libpath);

    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
    if (!gipa) { printf("no vkGetInstanceProcAddr: %s\n", dlerror()); return 1; }
    printf("vkGetInstanceProcAddr: OK\n");

    PFN_vkEnumerateInstanceVersion eiv = (PFN_vkEnumerateInstanceVersion)gipa(NULL,"vkEnumerateInstanceVersion");
    uint32_t iv = VK_API_VERSION_1_0;
    if (eiv) eiv(&iv);
    printf("instance version: %u.%u.%u\n", VK_VERSION_MAJOR(iv), VK_VERSION_MINOR(iv), VK_VERSION_PATCH(iv));

    // instance extensions -- is VK_KHR_android_surface there?
    PFN_vkEnumerateInstanceExtensionProperties eiep =
        (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL,"vkEnumerateInstanceExtensionProperties");
    int haveAndroidSurface = 0, haveSurface = 0;
    if (eiep) {
        uint32_t n=0; eiep(NULL,&n,NULL);
        VkExtensionProperties *e = calloc(n,sizeof(*e)); eiep(NULL,&n,e);
        printf("INSTANCE EXTENSIONS (%u):\n", n);
        for (uint32_t i=0;i<n;i++) {
            printf("    %s\n", e[i].extensionName);
            if (!strcmp(e[i].extensionName,"VK_KHR_android_surface")) haveAndroidSurface=1;
            if (!strcmp(e[i].extensionName,"VK_KHR_surface")) haveSurface=1;
        }
        free(e);
    }
    printf("  *** VK_KHR_android_surface: %s ***\n", haveAndroidSurface?"YES":"NO  <-- swapchain to ANativeWindow impossible without this");
    printf("  *** VK_KHR_surface        : %s ***\n", haveSurface?"YES":"NO");

    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL,"vkCreateInstance");
    VkApplicationInfo app={0}; app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO; app.apiVersion=iv;
    VkInstanceCreateInfo ii={0}; ii.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ii.pApplicationInfo=&app;
    VkInstance inst=VK_NULL_HANDLE;
    VkResult r = ci(&ii,NULL,&inst);
    printf("vkCreateInstance: %d %s\n", r, r==VK_SUCCESS?"OK":"FAILED");
    if (r!=VK_SUCCESS) return 1;

    PFN_vkEnumeratePhysicalDevices epd=(PFN_vkEnumeratePhysicalDevices)gipa(inst,"vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties gpdp=(PFN_vkGetPhysicalDeviceProperties)gipa(inst,"vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceFeatures gpdf=(PFN_vkGetPhysicalDeviceFeatures)gipa(inst,"vkGetPhysicalDeviceFeatures");
    PFN_vkEnumerateDeviceExtensionProperties edep=(PFN_vkEnumerateDeviceExtensionProperties)gipa(inst,"vkEnumerateDeviceExtensionProperties");

    uint32_t nd=0; epd(inst,&nd,NULL);
    printf("physical devices: %u\n", nd);
    VkPhysicalDevice *pds=calloc(nd,sizeof(*pds)); epd(inst,&nd,pds);
    for (uint32_t i=0;i<nd;i++) {
        VkPhysicalDeviceProperties p; gpdp(pds[i],&p);
        printf("\n--- device %u: %s ---\n", i, p.deviceName);
        printf("  *** apiVersion: %u.%u.%u ***  deviceType=%d (4=CPU)\n",
            VK_VERSION_MAJOR(p.apiVersion),VK_VERSION_MINOR(p.apiVersion),VK_VERSION_PATCH(p.apiVersion), p.deviceType);
        VkPhysicalDeviceFeatures f; gpdf(pds[i],&f);
        printf("  *** textureCompressionBC: %s ***\n", f.textureCompressionBC?"YES":"NO");
        printf("  dualSrcBlend=%s fillModeNonSolid=%s multiViewport=%s geometryShader=%s\n",
            f.dualSrcBlend?"Y":"n", f.fillModeNonSolid?"Y":"n", f.multiViewport?"Y":"n", f.geometryShader?"Y":"n");

        static const char *want[]={
            "VK_KHR_swapchain","VK_KHR_dynamic_rendering","VK_KHR_synchronization2",
            "VK_KHR_maintenance4","VK_KHR_copy_commands2","VK_KHR_format_feature_flags2",
            "VK_EXT_extended_dynamic_state","VK_EXT_extended_dynamic_state2","VK_EXT_extended_dynamic_state3",
            "VK_EXT_graphics_pipeline_library","VK_EXT_transform_feedback","VK_EXT_robustness2",
            "VK_EXT_vertex_attribute_divisor","VK_EXT_depth_clip_enable","VK_EXT_host_query_reset",
            "VK_EXT_memory_priority","VK_EXT_shader_demote_to_helper_invocation","VK_EXT_4444_formats",
            "VK_EXT_custom_border_color","VK_EXT_non_seamless_cube_map","VK_EXT_attachment_feedback_loop_layout",
            "VK_EXT_shader_module_identifier","VK_EXT_swapchain_maintenance1","VK_KHR_image_format_list",
        };
        uint32_t ne=0; edep(pds[i],NULL,&ne,NULL);
        VkExtensionProperties *de=calloc(ne,sizeof(*de)); edep(pds[i],NULL,&ne,de);
        printf("  device extensions: %u total\n", ne);
        printf("  DXVK-relevant:\n");
        for (size_t w=0;w<sizeof(want)/sizeof(want[0]);w++){
            int found=0;
            for(uint32_t e2=0;e2<ne;e2++) if(!strcmp(de[e2].extensionName,want[w])){found=1;break;}
            printf("    [%s] %s\n", found?"x":" ", want[w]);
        }
        free(de);
    }
    printf("\n=== DONE ===\n");
    return 0;
}
