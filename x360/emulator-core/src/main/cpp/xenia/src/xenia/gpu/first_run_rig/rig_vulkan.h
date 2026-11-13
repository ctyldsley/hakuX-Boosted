// hakuX-Boosted: the fork's own file. Part of the first-run rig (README.md
// here), not of the app: the little of Vulkan the rig needs, on whatever
// device the loader offers first (meant for Mesa's lavapipe, a software
// implementation, so that it runs on a desktop or a cloud machine).

#ifndef XENIA_GPU_FIRST_RUN_RIG_RIG_VULKAN_H_
#define XENIA_GPU_FIRST_RUN_RIG_RIG_VULKAN_H_

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>

namespace rig {

#define RIG_VK(call)                                                   \
  do {                                                                 \
    VkResult rig_vk_result = (call);                                   \
    if (rig_vk_result != VK_SUCCESS) {                                 \
      std::fprintf(stderr, "%s:%d: %s failed: %d\n", __FILE__, __LINE__, \
                   #call, int(rig_vk_result));                         \
      std::exit(1);                                                    \
    }                                                                  \
  } while (0)

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint8_t* data = nullptr;
  VkDeviceSize size = 0;
};

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0, layers = 1;
};

struct Device {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory_properties = {};
  char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};

  void Initialize() {
    VkApplicationInfo application_info = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application_info.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info = {
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &application_info;
    RIG_VK(vkCreateInstance(&instance_info, nullptr, &instance));
    uint32_t count = 0;
    RIG_VK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    if (!count) {
      std::fprintf(stderr, "no Vulkan device\n");
      std::exit(1);
    }
    std::vector<VkPhysicalDevice> physical_devices(count);
    RIG_VK(vkEnumeratePhysicalDevices(instance, &count,
                                      physical_devices.data()));
    // A software device first, if there is one: the rig is not about speed.
    physical_device = physical_devices[0];
    for (VkPhysicalDevice candidate : physical_devices) {
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(candidate, &properties);
      if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
        physical_device = candidate;
        break;
      }
    }
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical_device, &properties);
    std::memcpy(name, properties.deviceName, sizeof(name));
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count,
                                             nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count,
                                             families.data());
    for (uint32_t i = 0; i < family_count; ++i) {
      if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        queue_family = i;
        break;
      }
    }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures features = {};
    features.geometryShader = VK_TRUE;
    features.tessellationShader = VK_TRUE;
    features.vertexPipelineStoresAndAtomics = VK_TRUE;
    features.fragmentStoresAndAtomics = VK_TRUE;
    features.independentBlend = VK_TRUE;
    features.imageCubeArray = VK_TRUE;
    features.shaderClipDistance = VK_TRUE;
    features.shaderCullDistance = VK_TRUE;
    features.fullDrawIndexUint32 = VK_TRUE;
    features.samplerAnisotropy = VK_TRUE;
    VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.pEnabledFeatures = &features;
    RIG_VK(vkCreateDevice(physical_device, &device_info, nullptr, &device));
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    VkCommandPoolCreateInfo pool_info = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family;
    RIG_VK(vkCreateCommandPool(device, &pool_info, nullptr, &command_pool));
  }

  uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
      if ((type_bits & (1u << i)) &&
          (memory_properties.memoryTypes[i].propertyFlags & flags) == flags) {
        return i;
      }
    }
    std::fprintf(stderr, "no memory type\n");
    std::exit(1);
  }

  Buffer MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer buffer;
    buffer.size = size;
    VkBufferCreateInfo buffer_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = size;
    buffer_info.usage = usage;
    RIG_VK(vkCreateBuffer(device, &buffer_info, nullptr, &buffer.buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);
    VkMemoryAllocateInfo allocate_info = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex =
        FindMemoryType(requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    RIG_VK(vkAllocateMemory(device, &allocate_info, nullptr, &buffer.memory));
    RIG_VK(vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0));
    void* mapping;
    RIG_VK(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &mapping));
    buffer.data = static_cast<uint8_t*>(mapping);
    std::memset(buffer.data, 0, size_t(size));
    return buffer;
  }

  Image MakeImage(VkFormat format, uint32_t width, uint32_t height,
                  uint32_t layers, VkImageUsageFlags usage,
                  VkImageViewType view_type, bool cube = false) {
    Image image;
    image.format = format;
    image.width = width;
    image.height = height;
    image.layers = layers;
    VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = layers;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    RIG_VK(vkCreateImage(device, &image_info, nullptr, &image.image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image.image, &requirements);
    VkMemoryAllocateInfo allocate_info = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex =
        FindMemoryType(requirements.memoryTypeBits, 0);
    RIG_VK(vkAllocateMemory(device, &allocate_info, nullptr, &image.memory));
    RIG_VK(vkBindImageMemory(device, image.image, image.memory, 0));
    VkImageViewCreateInfo view_info = {
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = image.image;
    view_info.viewType = view_type;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    RIG_VK(vkCreateImageView(device, &view_info, nullptr, &image.view));
    return image;
  }

  VkShaderModule MakeShaderModule(const uint32_t* words, size_t size_bytes) {
    VkShaderModuleCreateInfo module_info = {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = size_bytes;
    module_info.pCode = words;
    VkShaderModule module;
    RIG_VK(vkCreateShaderModule(device, &module_info, nullptr, &module));
    return module;
  }

  VkCommandBuffer Begin() {
    VkCommandBufferAllocateInfo allocate_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate_info.commandPool = command_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    VkCommandBuffer command_buffer;
    RIG_VK(vkAllocateCommandBuffers(device, &allocate_info, &command_buffer));
    VkCommandBufferBeginInfo begin_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    RIG_VK(vkBeginCommandBuffer(command_buffer, &begin_info));
    return command_buffer;
  }

  void SubmitAndWait(VkCommandBuffer command_buffer) {
    RIG_VK(vkEndCommandBuffer(command_buffer));
    VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;
    RIG_VK(vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE));
    RIG_VK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(device, command_pool, 1, &command_buffer);
  }
};

}  // namespace rig

#endif  // XENIA_GPU_FIRST_RUN_RIG_RIG_VULKAN_H_
