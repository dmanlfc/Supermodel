/**
 ** Supermodel
 ** A Sega Model 3 Arcade Emulator.
 ** Copyright 2003-2026 The Supermodel Team
 **
 ** This file is part of Supermodel.
 **
 ** Supermodel is free software: you can redistribute it and/or modify it under
 ** the terms of the GNU General Public License as published by the Free
 ** Software Foundation, either version 3 of the License, or (at your option)
 ** any later version.
 **
 ** Supermodel is distributed in the hope that it will be useful, but WITHOUT
 ** ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 ** FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 ** more details.
 **
 ** You should have received a copy of the GNU General Public License along
 ** with Supermodel.  If not, see <http://www.gnu.org/licenses/>.
 **/

#ifdef SUPERMODEL_VULKAN

#include "VulkanContext.h"

#include "OSD/Logger.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>

#ifdef SUPERMODEL_OSX
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#else
#include <SDL.h>
#include <SDL_vulkan.h>
#endif

namespace Vulkan
{
namespace
{

std::string VkError(const char *operation, VkResult result)
{
  std::ostringstream message;
  message << operation << " failed with VkResult " << static_cast<int>(result);
  return message.str();
}

bool FormatHasOptimalFeatures(VkPhysicalDevice physicalDevice, VkFormat format,
                              VkFormatFeatureFlags features)
{
  VkFormatProperties properties{};
  vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
  return (properties.optimalTilingFeatures & features) == features;
}

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment)
{
  if (alignment <= 1)
    return value;
  return (value + alignment - 1) / alignment * alignment;
}

} // anonymous namespace

Context::~Context()
{
  Shutdown();
}

bool Context::SetError(std::string *error, const std::string &message) const
{
  if (error != nullptr)
    *error = message;
  return false;
}

uint32_t Context::GraphicsQueueFamily() const
{
  return m_queueFamilies.graphics >= 0 ? static_cast<uint32_t>(m_queueFamilies.graphics) : 0u;
}

VkCommandBuffer Context::CommandBuffer() const
{
  return m_frameActive ? m_commandBuffers[m_currentFrame] : VK_NULL_HANDLE;
}

bool Context::Init(SDL_Window *window, bool vsync, std::string *error)
{
  return Init(window, vsync, false, error);
}

bool Context::Init(SDL_Window *window, bool vsync, bool quadRendering, std::string *error)
{
  if (window == nullptr)
    return SetError(error, "Vulkan initialization received no SDL window");

  m_window = window;
  m_vsync = vsync;
  m_quadRenderingRequested = quadRendering;

  if (SDL_Vulkan_LoadLibrary(nullptr) != 0)
    return SetError(error, std::string("SDL could not load the Vulkan loader: ") + SDL_GetError());
  m_loaderLoaded = true;

  if (SDL_Vulkan_GetVkGetInstanceProcAddr() == nullptr)
    return SetError(error, "SDL loaded Vulkan, but vkGetInstanceProcAddr is unavailable");

  if (!CreateInstance(error) || !CreateSurface(error) || !SelectPhysicalDevice(error) ||
      !CreateDevice(error) || !CreateCommandResources(error) || !CreateSyncResources(error) ||
      !CreateSwapchain(VK_NULL_HANDLE, error) || !CreateUploadResources(error) ||
      !CreateStreamingResources(error))
  {
    Shutdown();
    return false;
  }

  return true;
}

void Context::Shutdown()
{
  if (m_device != VK_NULL_HANDLE)
    vkDeviceWaitIdle(m_device);

  // The GPU is idle: run the hooks before the resources they read are destroyed.
  for (uint32_t i = 0; i < kFramesInFlight; ++i)
    RunFrameCompletions(i);

  DestroySceneResources();
  DestroyStreamingResources();
  DestroyUploadResources();
  DestroySwapchain();

  if (m_device != VK_NULL_HANDLE)
  {
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
      if (m_imageAvailable[i] != VK_NULL_HANDLE)
        vkDestroySemaphore(m_device, m_imageAvailable[i], nullptr);
      if (m_uploadDone[i] != VK_NULL_HANDLE)
        vkDestroySemaphore(m_device, m_uploadDone[i], nullptr);
      if (m_inFlight[i] != VK_NULL_HANDLE)
        vkDestroyFence(m_device, m_inFlight[i], nullptr);
    }

    if (m_commandPool != VK_NULL_HANDLE)
      vkDestroyCommandPool(m_device, m_commandPool, nullptr);
    vkDestroyDevice(m_device, nullptr);
  }

  if (m_surface != VK_NULL_HANDLE && m_instance != VK_NULL_HANDLE)
    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
  if (m_instance != VK_NULL_HANDLE)
    vkDestroyInstance(m_instance, nullptr);
  if (m_loaderLoaded)
    SDL_Vulkan_UnloadLibrary();

  m_window = nullptr;
  m_loaderLoaded = false;
  m_instance = VK_NULL_HANDLE;
  m_surface = VK_NULL_HANDLE;
  m_physicalDevice = VK_NULL_HANDLE;
  m_device = VK_NULL_HANDLE;
  m_graphicsQueue = VK_NULL_HANDLE;
  m_presentQueue = VK_NULL_HANDLE;
  m_deviceContext = DeviceContext{};
  m_commandPool = VK_NULL_HANDLE;
  m_commandBuffers.fill(VK_NULL_HANDLE);
  m_imageAvailable.fill(VK_NULL_HANDLE);
  m_inFlight.fill(VK_NULL_HANDLE);
  m_currentFrame = 0;
  m_frameActive = false;
  m_fatalError = false;
  m_outputImageWritten = false;
  m_geometryShaderEnabled = false;
  m_canBlitToSwapchain = false;
  m_canCopyToSwapchain = false;
}

bool Context::CreateInstance(std::string *error)
{
  unsigned extensionCount = 0;
  if (SDL_Vulkan_GetInstanceExtensions(m_window, &extensionCount, nullptr) != SDL_TRUE || extensionCount == 0)
    return SetError(error, std::string("SDL could not determine required Vulkan instance extensions: ") + SDL_GetError());

  std::vector<const char *> extensions(extensionCount);
  if (SDL_Vulkan_GetInstanceExtensions(m_window, &extensionCount, extensions.data()) != SDL_TRUE)
    return SetError(error, std::string("SDL could not provide required Vulkan instance extensions: ") + SDL_GetError());

  VkApplicationInfo applicationInfo{};
  applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  applicationInfo.pApplicationName = "Supermodel";
  applicationInfo.applicationVersion = VK_MAKE_VERSION(0, 3, 0);
  applicationInfo.pEngineName = "Supermodel Vulkan";
  applicationInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
  applicationInfo.apiVersion = VK_API_VERSION_1_1;

  // Without portability enumeration the loader hides any driver implementing only a
  // portable subset, leaving vkEnumeratePhysicalDevices empty. Gated on the extension
  // being advertised rather than on the platform.
  uint32_t availableCount = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, nullptr);
  std::vector<VkExtensionProperties> available(availableCount);
  if (availableCount != 0)
    vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, available.data());

  const auto instanceExtensionAvailable = [&available](const char *name) {
    for (const VkExtensionProperties &properties : available)
    {
      if (std::strcmp(properties.extensionName, name) == 0)
        return true;
    }
    return false;
  };

  VkInstanceCreateFlags createFlags = 0;
  if (instanceExtensionAvailable(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
  {
    extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    createFlags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  }

  VkInstanceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.flags = createFlags;
  createInfo.pApplicationInfo = &applicationInfo;
  createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  createInfo.ppEnabledExtensionNames = extensions.data();

  VkResult result = vkCreateInstance(&createInfo, nullptr, &m_instance);
  if (result == VK_ERROR_INCOMPATIBLE_DRIVER)
    return SetError(error, "No Vulkan 1.1-compatible driver is available. Install a Vulkan loader and a GPU driver exposing Vulkan 1.1 or newer");
  if (result != VK_SUCCESS)
    return SetError(error, VkError("vkCreateInstance", result));
  return true;
}

bool Context::CreateSurface(std::string *error)
{
  if (SDL_Vulkan_CreateSurface(m_window, m_instance, &m_surface) != SDL_TRUE)
    return SetError(error, std::string("SDL could not create the Vulkan window surface: ") + SDL_GetError());
  return true;
}

Context::QueueFamilies Context::FindQueueFamilies(VkPhysicalDevice device) const
{
  QueueFamilies families;
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
  std::vector<VkQueueFamilyProperties> properties(count);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, properties.data());

  for (uint32_t i = 0; i < count; ++i)
  {
    if (properties[i].queueCount > 0 && (properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
      families.graphics = static_cast<int>(i);

    VkBool32 supportsPresent = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &supportsPresent) == VK_SUCCESS && supportsPresent == VK_TRUE)
      families.present = static_cast<int>(i);

    if (families.Complete())
      break;
  }
  return families;
}

bool Context::SupportsRequiredDeviceExtensions(VkPhysicalDevice device) const
{
  uint32_t count = 0;
  if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS)
    return false;
  std::vector<VkExtensionProperties> available(count);
  if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data()) != VK_SUCCESS)
    return false;

  for (const auto &extension : available)
  {
    if (std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
      return true;
  }
  return false;
}

Context::SwapchainSupport Context::QuerySwapchainSupport(VkPhysicalDevice device) const
{
  SwapchainSupport support;
  if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, m_surface, &support.capabilities) != VK_SUCCESS)
    return support;

  uint32_t count = 0;
  if (vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &count, nullptr) == VK_SUCCESS && count > 0)
  {
    support.formats.resize(count);
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &count, support.formats.data()) != VK_SUCCESS)
      support.formats.clear();
  }

  count = 0;
  if (vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &count, nullptr) == VK_SUCCESS && count > 0)
  {
    support.presentModes.resize(count);
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &count, support.presentModes.data()) != VK_SUCCESS)
      support.presentModes.clear();
  }
  return support;
}

bool Context::SelectPhysicalDevice(std::string *error)
{
  uint32_t count = 0;
  VkResult result = vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
  if (result != VK_SUCCESS)
    return SetError(error, VkError("vkEnumeratePhysicalDevices", result));
  if (count == 0)
    return SetError(error, "The Vulkan loader found no physical devices");

  std::vector<VkPhysicalDevice> devices(count);
  result = vkEnumeratePhysicalDevices(m_instance, &count, devices.data());
  if (result != VK_SUCCESS)
    return SetError(error, VkError("vkEnumeratePhysicalDevices", result));

  // Remembered so the failure can name independentBlend rather than report a generic
  // "no compatible device".
  std::string independentBlendRejection;

  // Every device that clears the hard requirements, gathered so the choice between
  // them (named preference, then non-integrated) is made once, over the whole set.
  struct Candidate
  {
    VkPhysicalDevice device;
    VkPhysicalDeviceProperties properties;
    QueueFamilies families;
    bool geometryShader;
  };
  std::vector<Candidate> candidates;

  for (VkPhysicalDevice device : devices)
  {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    QueueFamilies families = FindQueueFamilies(device);
    if (properties.apiVersion < VK_API_VERSION_1_1 || !families.Complete() || !SupportsRequiredDeviceExtensions(device))
      continue;

    SwapchainSupport support = QuerySwapchainSupport(device);
    if (support.formats.empty() || support.presentModes.empty() ||
        (support.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
      continue;

    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(device, &features);

    // The New3D layer scheme is expressed with one colour write mask per
    // attachment, which Vulkan only permits with independentBlend.
    if (features.independentBlend != VK_TRUE)
    {
      if (independentBlendRejection.empty())
      {
        std::ostringstream message;
        message << "The Vulkan device \"" << properties.deviceName
                << "\" does not support the required feature "
                   "VkPhysicalDeviceFeatures::independentBlend. The Real3D renderer needs a "
                   "separate colour write mask per attachment to reproduce the New3D opaque, "
                   "transparency and line-of-sight layers, so this device cannot be used. Use a "
                   "GPU and driver that report independentBlend, or run with "
                   "GraphicsBackend=OpenGL";
        independentBlendRejection = message.str();
      }
      continue;
    }

    candidates.push_back({device, properties, families, features.geometryShader == VK_TRUE});
  }

  if (candidates.empty())
  {
    if (!independentBlendRejection.empty())
      return SetError(error, independentBlendRejection);
    return SetError(error, "No compatible Vulkan device was found. Vulkan 1.1, graphics and presentation queues, VK_KHR_swapchain, swapchain transfer-destination images and VkPhysicalDeviceFeatures::independentBlend are required");
  }

  const Candidate *chosen = nullptr;

  // An explicit request wins: match the configured name exactly against what Vulkan
  // reports, which is what the GUI and config store. A stale or incompatible name
  // never hard-fails; it just falls through to auto-selection with a warning.
  if (!m_preferredDeviceName.empty() && m_preferredDeviceName != "Auto")
  {
    for (const Candidate &c : candidates)
    {
      if (m_preferredDeviceName == c.properties.deviceName)
      {
        chosen = &c;
        break;
      }
    }
    if (chosen == nullptr)
      ErrorLog("The requested Vulkan device \"%s\" was not found among the compatible GPUs; "
               "auto-selecting instead.", m_preferredDeviceName.c_str());
  }

  // Auto: with two GPUs (integrated + discrete) pick the non-integrated one, and
  // when quad rendering was requested prefer a GPU that can build the geometry-shader
  // pipeline. Highest score wins; ties keep enumeration order.
  if (chosen == nullptr)
  {
    auto Score = [&](const Candidate &c) {
      int score = 0;
      if (m_quadRenderingRequested && c.geometryShader)
        score += 4;
      if (c.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
        score += 2;
      return score;
    };
    int bestScore = -1;
    for (const Candidate &c : candidates)
    {
      const int score = Score(c);
      if (score > bestScore)
      {
        bestScore = score;
        chosen = &c;
      }
    }
  }

  m_physicalDevice = chosen->device;
  m_deviceProperties = chosen->properties;
  m_queueFamilies = chosen->families;
  m_geometryShaderEnabled = chosen->geometryShader && m_quadRenderingRequested;
  return true;
}

std::vector<std::string> Context::EnumerateDeviceNames()
{
  std::vector<std::string> names;

  if (SDL_Vulkan_LoadLibrary(nullptr) != 0)
    return names;

  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "Supermodel";
  appInfo.apiVersion = VK_API_VERSION_1_1;

  // Mirror CreateInstance: without portability enumeration the loader hides drivers
  // implementing only a portable subset (MoltenVK), so add it only when advertised.
  std::vector<const char *> extensions;
  VkInstanceCreateFlags createFlags = 0;
  uint32_t availableCount = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, nullptr);
  std::vector<VkExtensionProperties> available(availableCount);
  if (availableCount != 0)
    vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, available.data());
  for (const VkExtensionProperties &properties : available)
  {
    if (std::strcmp(properties.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0)
    {
      extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
      createFlags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
      break;
    }
  }

  VkInstanceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.flags = createFlags;
  createInfo.pApplicationInfo = &appInfo;
  createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  createInfo.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

  VkInstance instance = VK_NULL_HANDLE;
  if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS)
  {
    SDL_Vulkan_UnloadLibrary();
    return names;
  }

  uint32_t count = 0;
  if (vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS && count > 0)
  {
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) == VK_SUCCESS)
    {
      for (VkPhysicalDevice device : devices)
      {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        names.emplace_back(properties.deviceName);
      }
    }
  }

  vkDestroyInstance(instance, nullptr);
  SDL_Vulkan_UnloadLibrary();
  return names;
}

bool Context::CreateDevice(std::string *error)
{
  std::set<uint32_t> uniqueFamilies = {
    static_cast<uint32_t>(m_queueFamilies.graphics),
    static_cast<uint32_t>(m_queueFamilies.present)
  };
  const float priority = 1.0f;
  std::vector<VkDeviceQueueCreateInfo> queues;
  for (uint32_t family : uniqueFamilies)
  {
    VkDeviceQueueCreateInfo queue{};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    queues.push_back(queue);
  }

  std::vector<const char *> extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

  // A device implementing only a portable subset requires this extension enabled. Gated
  // on the device advertising it, so inert on a fully conformant driver.
  {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    if (count != 0)
      vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &count, available.data());
    for (const VkExtensionProperties &properties : available)
    {
      if (std::strcmp(properties.extensionName, "VK_KHR_portability_subset") == 0)
      {
        extensions.push_back("VK_KHR_portability_subset");
        break;
      }
    }
  }

  // SelectPhysicalDevice() already refused any device without independentBlend
  // and decided whether the quad pipeline is reachable.
  VkPhysicalDeviceFeatures features{};
  features.independentBlend = VK_TRUE;
  if (m_geometryShaderEnabled)
    features.geometryShader = VK_TRUE;

  VkDeviceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  createInfo.queueCreateInfoCount = static_cast<uint32_t>(queues.size());
  createInfo.pQueueCreateInfos = queues.data();
  createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  createInfo.ppEnabledExtensionNames = extensions.data();
  createInfo.pEnabledFeatures = &features;

  VkResult result = vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device);
  if (result != VK_SUCCESS)
    return SetError(error, VkError("vkCreateDevice", result));

  vkGetDeviceQueue(m_device, static_cast<uint32_t>(m_queueFamilies.graphics), 0, &m_graphicsQueue);
  vkGetDeviceQueue(m_device, static_cast<uint32_t>(m_queueFamilies.present), 0, &m_presentQueue);

  m_deviceContext.device = m_device;
  m_deviceContext.physicalDevice = m_physicalDevice;
  m_deviceContext.graphicsQueue = m_graphicsQueue;
  m_deviceContext.graphicsFamily = static_cast<uint32_t>(m_queueFamilies.graphics);
  m_uniformOffsetAlignment =
      std::max<VkDeviceSize>(1, m_deviceProperties.limits.minUniformBufferOffsetAlignment);
  return true;
}

bool Context::CreateCommandResources(std::string *error)
{
  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = static_cast<uint32_t>(m_queueFamilies.graphics);
  VkResult result = vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool);
  if (result != VK_SUCCESS)
    return SetError(error, VkError("vkCreateCommandPool", result));

  VkCommandBufferAllocateInfo allocation{};
  allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocation.commandPool = m_commandPool;
  allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocation.commandBufferCount = kFramesInFlight;
  result = vkAllocateCommandBuffers(m_device, &allocation, m_commandBuffers.data());
  if (result != VK_SUCCESS)
    return SetError(error, VkError("vkAllocateCommandBuffers", result));

  result = vkAllocateCommandBuffers(m_device, &allocation, m_uploadCommandBuffers.data());
  if (result != VK_SUCCESS)
    return SetError(error, VkError("vkAllocateCommandBuffers(uploads)", result));

  return true;
}

bool Context::CreateSyncResources(std::string *error)
{
  VkSemaphoreCreateInfo semaphoreInfo{};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  for (uint32_t i = 0; i < kFramesInFlight; ++i)
  {
    VkResult result = vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_imageAvailable[i]);
    if (result != VK_SUCCESS)
      return SetError(error, VkError("vkCreateSemaphore(image available)", result));
    result = vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_uploadDone[i]);
    if (result != VK_SUCCESS)
      return SetError(error, VkError("vkCreateSemaphore(upload done)", result));
    result = vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlight[i]);
    if (result != VK_SUCCESS)
      return SetError(error, VkError("vkCreateFence", result));
  }
  return true;
}

bool Context::CreatePresentSemaphores(std::string *error)
{
  // One per swapchain image, not per frame in flight: indexing by acquired image
  // keeps every signal paired with exactly one wait.
  DestroyPresentSemaphores();

  VkSemaphoreCreateInfo semaphoreInfo{};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  m_renderFinished.resize(m_swapchainImages.size(), VK_NULL_HANDLE);
  for (size_t i = 0; i < m_renderFinished.size(); ++i)
  {
    const VkResult result = vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_renderFinished[i]);
    if (result != VK_SUCCESS)
    {
      DestroyPresentSemaphores();
      return SetError(error, VkError("vkCreateSemaphore(render finished)", result));
    }
  }
  return true;
}

void Context::DestroyPresentSemaphores()
{
  if (m_device != VK_NULL_HANDLE)
  {
    for (VkSemaphore semaphore : m_renderFinished)
    {
      if (semaphore != VK_NULL_HANDLE)
        vkDestroySemaphore(m_device, semaphore, nullptr);
    }
  }
  m_renderFinished.clear();
}

VkSurfaceFormatKHR Context::ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &formats) const
{
  // A sole VK_FORMAT_UNDEFINED entry means the surface accepts any format.
  if (formats.size() == 1 && formats.front().format == VK_FORMAT_UNDEFINED)
    return {VK_FORMAT_B8G8R8A8_UNORM, formats.front().colorSpace};

  // Render format first: a match lets EndFrame() present by image copy instead of blit.
  // UNORM before SRGB regardless, since the output image is UNORM and an SRGB destination
  // would add a linear-to-sRGB encode the OpenGL renderer never performed.
  const VkFormat preferred[] = {
    SceneTargets::kColourFormat,
    VK_FORMAT_B8G8R8A8_UNORM,
    VK_FORMAT_R8G8B8A8_UNORM,
    VK_FORMAT_B8G8R8A8_SRGB,
    VK_FORMAT_R8G8B8A8_SRGB
  };
  for (VkFormat candidate : preferred)
  {
    for (const auto &format : formats)
    {
      if (format.format == candidate && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        return format;
    }
  }
  return {};
}

VkPresentModeKHR Context::ChoosePresentMode(const std::vector<VkPresentModeKHR> &modes) const
{
  if (m_vsync)
    return VK_PRESENT_MODE_FIFO_KHR;

  for (VkPresentModeKHR mode : modes)
  {
    if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
      return mode;
  }
  for (VkPresentModeKHR mode : modes)
  {
    if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
      return mode;
  }
  return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D Context::ChooseExtent(const VkSurfaceCapabilitiesKHR &capabilities) const
{
  if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
    return capabilities.currentExtent;

  int width = 0;
  int height = 0;
  SDL_Vulkan_GetDrawableSize(m_window, &width, &height);
  VkExtent2D extent = {
    static_cast<uint32_t>(std::max(width, 0)),
    static_cast<uint32_t>(std::max(height, 0))
  };
  extent.width = std::max(capabilities.minImageExtent.width, std::min(capabilities.maxImageExtent.width, extent.width));
  extent.height = std::max(capabilities.minImageExtent.height, std::min(capabilities.maxImageExtent.height, extent.height));
  return extent;
}

void Context::UpdateSwapchainCompositeSupport()
{
  m_canBlitToSwapchain =
      FormatHasOptimalFeatures(m_physicalDevice, SceneTargets::kColourFormat,
                               VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
      FormatHasOptimalFeatures(m_physicalDevice, m_swapchainFormat, VK_FORMAT_FEATURE_BLIT_DST_BIT);
  // A raw copy is only faithful on an exact format match: same channel order and encoding.
  m_canCopyToSwapchain = m_swapchainFormat == SceneTargets::kColourFormat;

  // Logged because the copy is the cheaper path, and a format match is also what would
  // allow rendering straight into the swapchain image.
  InfoLog("Vulkan presents by %s (swapchain format %d, render format %d).",
          m_canCopyToSwapchain ? "image copy" : (m_canBlitToSwapchain ? "image blit" : "neither"),
          static_cast<int>(m_swapchainFormat), static_cast<int>(SceneTargets::kColourFormat));
}

bool Context::CreateSwapchain(VkSwapchainKHR oldSwapchain, std::string *error)
{
  SwapchainSupport support = QuerySwapchainSupport(m_physicalDevice);
  if (support.formats.empty() || support.presentModes.empty())
    return SetError(error, "The Vulkan surface no longer provides usable formats or presentation modes");

  VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(support.formats);
  if (surfaceFormat.format == VK_FORMAT_UNDEFINED)
    return SetError(error, "The Vulkan surface does not expose an RGBA8 or BGRA8 transfer destination format");
  VkPresentModeKHR presentMode = ChoosePresentMode(support.presentModes);
  VkExtent2D extent = ChooseExtent(support.capabilities);
  if (extent.width == 0 || extent.height == 0)
    return SetError(error, "The Vulkan drawable is minimized");

  // One more image than there are frames in flight, so a frame that has finished recording
  // always has somewhere to present to. Equal counts move the CPU stall from the frame
  // fence to the image, rather than removing it.
  uint32_t imageCount = std::max(support.capabilities.minImageCount + 1, kFramesInFlight + 1);
  if (support.capabilities.maxImageCount > 0)
    imageCount = std::min(imageCount, support.capabilities.maxImageCount);

  uint32_t familyIndices[] = {
    static_cast<uint32_t>(m_queueFamilies.graphics),
    static_cast<uint32_t>(m_queueFamilies.present)
  };
  VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  const VkCompositeAlphaFlagBitsKHR compositeCandidates[] = {
    VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
    VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
    VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
    VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
  };
  for (VkCompositeAlphaFlagBitsKHR candidate : compositeCandidates)
  {
    if ((support.capabilities.supportedCompositeAlpha & candidate) != 0)
    {
      compositeAlpha = candidate;
      break;
    }
  }

  VkSwapchainCreateInfoKHR createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  createInfo.surface = m_surface;
  createInfo.minImageCount = imageCount;
  createInfo.imageFormat = surfaceFormat.format;
  createInfo.imageColorSpace = surfaceFormat.colorSpace;
  createInfo.imageExtent = extent;
  createInfo.imageArrayLayers = 1;
  createInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (m_queueFamilies.graphics != m_queueFamilies.present)
  {
    createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    createInfo.queueFamilyIndexCount = 2;
    createInfo.pQueueFamilyIndices = familyIndices;
  }
  else
  {
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }
  createInfo.preTransform = support.capabilities.currentTransform;
  createInfo.compositeAlpha = compositeAlpha;
  createInfo.presentMode = presentMode;
  createInfo.clipped = VK_TRUE;
  createInfo.oldSwapchain = oldSwapchain;

  VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
  VkResult result = vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &newSwapchain);
  if (result != VK_SUCCESS)
    return SetError(error, VkError("vkCreateSwapchainKHR", result));

  uint32_t actualCount = 0;
  result = vkGetSwapchainImagesKHR(m_device, newSwapchain, &actualCount, nullptr);
  if (result != VK_SUCCESS || actualCount == 0)
  {
    vkDestroySwapchainKHR(m_device, newSwapchain, nullptr);
    return SetError(error, result == VK_SUCCESS ? "The Vulkan swapchain contains no images" : VkError("vkGetSwapchainImagesKHR", result));
  }
  std::vector<VkImage> images(actualCount);
  result = vkGetSwapchainImagesKHR(m_device, newSwapchain, &actualCount, images.data());
  if (result != VK_SUCCESS)
  {
    vkDestroySwapchainKHR(m_device, newSwapchain, nullptr);
    return SetError(error, VkError("vkGetSwapchainImagesKHR", result));
  }

  m_swapchain = newSwapchain;
  m_swapchainFormat = surfaceFormat.format;
  m_swapchainExtent = extent;
  m_swapchainImages = std::move(images);
  m_swapchainImageInitialized.assign(m_swapchainImages.size(), false);
  m_imagesInFlight.assign(m_swapchainImages.size(), VK_NULL_HANDLE);
  m_recreateRequested = false;
  // Frames in flight are only useful up to one fewer than there are images. Requested and
  // granted counts are both logged so a driver that clamped the request is visible.
  InfoLog("Vulkan swapchain has %u images for %u frames in flight%s (requested %u, device allows "
          "%u to %u).",
          static_cast<uint32_t>(m_swapchainImages.size()), kFramesInFlight,
          m_swapchainImages.size() > kFramesInFlight ? "" : " -- too few to overlap fully",
          imageCount, support.capabilities.minImageCount, support.capabilities.maxImageCount);
  UpdateSwapchainCompositeSupport();
  // Per swapchain image, so the count follows the image count across every recreation.
  if (!CreatePresentSemaphores(error))
    return false;
  return true;
}

uint32_t Context::FindMemoryTypeIndex(uint32_t typeBits, VkMemoryPropertyFlags required,
                                      VkMemoryPropertyFlags preferred, bool *coherent) const
{
  VkPhysicalDeviceMemoryProperties properties{};
  vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &properties);

  uint32_t fallback = UINT32_MAX;
  for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
  {
    if ((typeBits & (1u << i)) == 0 ||
        (properties.memoryTypes[i].propertyFlags & required) != required)
      continue;
    if ((properties.memoryTypes[i].propertyFlags & preferred) == preferred)
    {
      if (coherent != nullptr)
        *coherent = (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
      return i;
    }
    if (fallback == UINT32_MAX)
      fallback = i;
  }

  if (fallback != UINT32_MAX && coherent != nullptr)
    *coherent = (properties.memoryTypes[fallback].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
  return fallback;
}

bool Context::CreateUploadResources(std::string *error)
{
  m_uploadSize = static_cast<VkDeviceSize>(m_swapchainExtent.width) * m_swapchainExtent.height * 4;
  if (m_uploadSize == 0)
    return SetError(error, "Cannot create Vulkan upload buffers for an empty swapchain");

  for (uint32_t i = 0; i < kFramesInFlight; ++i)
  {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = m_uploadSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_uploadBuffers[i]);
    if (result != VK_SUCCESS)
      return SetError(error, VkError("vkCreateBuffer(TileGen upload)", result));

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(m_device, m_uploadBuffers[i], &requirements);
    bool coherent = false;
    const uint32_t memoryType = FindMemoryTypeIndex(requirements.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                    &coherent);
    if (memoryType == UINT32_MAX)
      return SetError(error, "No host-visible Vulkan memory type is available for TileGen uploads");

    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    result = vkAllocateMemory(m_device, &allocation, nullptr, &m_uploadMemory[i]);
    if (result != VK_SUCCESS)
      return SetError(error, VkError("vkAllocateMemory(TileGen upload)", result));
    result = vkBindBufferMemory(m_device, m_uploadBuffers[i], m_uploadMemory[i], 0);
    if (result != VK_SUCCESS)
      return SetError(error, VkError("vkBindBufferMemory(TileGen upload)", result));
    result = vkMapMemory(m_device, m_uploadMemory[i], 0, requirements.size, 0, &m_uploadMapped[i]);
    if (result != VK_SUCCESS)
      return SetError(error, VkError("vkMapMemory(TileGen upload)", result));
    m_uploadCoherent[i] = coherent;
    m_uploadAllocationSize[i] = requirements.size;
  }
  return true;
}

void Context::DestroyUploadResources()
{
  if (m_device == VK_NULL_HANDLE)
    return;
  for (uint32_t i = 0; i < kFramesInFlight; ++i)
  {
    if (m_uploadMapped[i] != nullptr && m_uploadMemory[i] != VK_NULL_HANDLE)
      vkUnmapMemory(m_device, m_uploadMemory[i]);
    if (m_uploadBuffers[i] != VK_NULL_HANDLE)
      vkDestroyBuffer(m_device, m_uploadBuffers[i], nullptr);
    if (m_uploadMemory[i] != VK_NULL_HANDLE)
      vkFreeMemory(m_device, m_uploadMemory[i], nullptr);
    m_uploadBuffers[i] = VK_NULL_HANDLE;
    m_uploadMemory[i] = VK_NULL_HANDLE;
    m_uploadMapped[i] = nullptr;
    m_uploadCoherent[i] = false;
    m_uploadAllocationSize[i] = 0;
  }
  m_uploadSize = 0;
}

bool Context::CreateStreamingResources(std::string *error)
{
  if (!m_uniformRing.Create(m_deviceContext, kUniformRingBytesPerFrame, kFramesInFlight,
                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            error))
    return false;
  if (!m_stagingRing.Create(m_deviceContext, kStagingRingBytesPerFrame, kFramesInFlight,
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, error))
    return false;
  if (!m_vertexStaging.Create(m_deviceContext, kVertexStagingBytesPerFrame, kFramesInFlight,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT, error))
    return false;
  if (!m_oneShotStaging.Create(m_deviceContext, kOneShotStagingBytes, 1,
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT, error))
    return false;

  m_losSliceBytes = AlignUp(kLosSlotBytes * kLosSlotsPerFrame,
                            std::max<VkDeviceSize>(16, m_deviceProperties.limits.nonCoherentAtomSize));
  if (!m_losReadback.Create(m_deviceContext, m_losSliceBytes * kFramesInFlight,
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            error))
    return false;
  if (m_losReadback.MappedPointer() == nullptr)
    return SetError(error, "The Vulkan line-of-sight readback buffer could not be mapped");
  std::memset(m_losReadback.MappedPointer(), 0, static_cast<size_t>(m_losReadback.Size()));
  return true;
}

void Context::DestroyStreamingResources()
{
  if (m_device == VK_NULL_HANDLE)
    return;
  m_uniformRing.Destroy(m_deviceContext);
  m_stagingRing.Destroy(m_deviceContext);
  m_vertexStaging.Destroy(m_deviceContext);
  m_oneShotStaging.Destroy(m_deviceContext);
  m_losReadback.Destroy(m_deviceContext);
  m_screenshotReadback.Destroy(m_deviceContext);
  m_losSliceBytes = 0;
  m_screenshotSliceBytes = 0;
}

void Context::DestroySceneResources()
{
  if (m_device == VK_NULL_HANDLE)
    return;
  m_pipelines.Destroy(m_deviceContext);
  m_sceneTargets.Destroy(m_deviceContext);
  m_targetInfo = TargetInfo{};
}

void Context::DestroySwapchain()
{
  DestroyPresentSemaphores();
  m_imagesInFlight.clear();
  m_swapchainImageInitialized.clear();
  m_swapchainImages.clear();
  if (m_swapchain != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
    vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
  m_swapchain = VK_NULL_HANDLE;
  m_swapchainFormat = VK_FORMAT_UNDEFINED;
  m_swapchainExtent = {};
}

void Context::RequestSwapchainRecreation()
{
  m_recreateRequested = true;
}

// VK_SUBOPTIMAL_KHR is advisory; the image is usable and the present succeeds. Compositor
// scaling and rotation report it permanently, so rebuild once per extent only. A real change
// still arrives as VK_ERROR_OUT_OF_DATE_KHR or a drawable size mismatch.
void Context::NoteSuboptimal()
{
  if (m_swapchainExtent.width == m_suboptimalExtent.width &&
      m_swapchainExtent.height == m_suboptimalExtent.height)
  {
    if (!m_suboptimalReported)
    {
      InfoLog("The Vulkan surface still reports VK_SUBOPTIMAL_KHR at %ux%u after the swapchain was "
              "rebuilt for it, so further rebuilds are suppressed.",
              m_swapchainExtent.width, m_swapchainExtent.height);
      m_suboptimalReported = true;
    }
    return;
  }
  m_suboptimalExtent = m_swapchainExtent;
  m_recreateRequested = true;
}

bool Context::RecreateSwapchain(std::string *error)
{
  if (m_device == VK_NULL_HANDLE)
    return SetError(error, "Vulkan swapchain recreation requested before device initialization");

  int width = 0;
  int height = 0;
  SDL_Vulkan_GetDrawableSize(m_window, &width, &height);
  if (width == 0 || height == 0)
  {
    m_recreateRequested = true;
    return false;
  }

  VkResult result = vkDeviceWaitIdle(m_device);
  if (result != VK_SUCCESS)
    return SetError(error, VkError("vkDeviceWaitIdle", result));

  VkSwapchainKHR oldSwapchain = m_swapchain;
  DestroyUploadResources();
  m_swapchain = VK_NULL_HANDLE;
  m_swapchainImages.clear();
  m_swapchainImageInitialized.clear();
  m_imagesInFlight.clear();
  if (!CreateSwapchain(oldSwapchain, error))
  {
    m_swapchain = oldSwapchain;
    m_recreateRequested = true;
    return false;
  }
  vkDestroySwapchainKHR(m_device, oldSwapchain, nullptr);
  if (!CreateUploadResources(error))
  {
    m_recreateRequested = true;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Frame completions
// ---------------------------------------------------------------------------

bool Context::AddFrameCompletion(FrameCompletionFn function, void *user, uint64_t token,
                                 std::string *error)
{
  if (function == nullptr)
    return SetError(error, "Vulkan frame completion registered without a callback");
  if (!m_frameActive)
    return SetError(error, "Vulkan frame completion registered outside an active frame");

  FrameCompletionList &list = m_frameCompletions[m_currentFrame];
  if (list.count >= kMaxFrameCompletions)
    return SetError(error, "Too many Vulkan frame completions registered in a single frame");

  list.entries[list.count].function = function;
  list.entries[list.count].user = user;
  list.entries[list.count].token = token;
  ++list.count;
  return true;
}

void Context::CancelFrameCompletions(void *user)
{
  for (FrameCompletionList &list : m_frameCompletions)
  {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < list.count; ++i)
    {
      if (list.entries[i].user == user)
        continue;
      list.entries[kept] = list.entries[i];
      ++kept;
    }
    for (uint32_t i = kept; i < list.count; ++i)
      list.entries[i] = FrameCompletion{};
    list.count = kept;
  }
}

void Context::RunFrameCompletions(uint32_t frameIndex)
{
  if (frameIndex >= kFramesInFlight)
    return;

  FrameCompletionList &list = m_frameCompletions[frameIndex];
  const uint32_t count = list.count;
  // Cleared first so a callback registering new work cannot be dropped or run twice.
  list.count = 0;
  for (uint32_t i = 0; i < count; ++i)
  {
    if (list.entries[i].function != nullptr)
      list.entries[i].function(list.entries[i].user, list.entries[i].token);
    list.entries[i] = FrameCompletion{};
  }
}

// ---------------------------------------------------------------------------
// Scene targets
// ---------------------------------------------------------------------------

bool Context::RecordInitialSceneTransitions(std::string *error)
{
  ScopedCommandBuffer scoped;
  if (!scoped.Begin(m_deviceContext, error))
    return false;
  m_sceneTargets.RecordInitialTransitions(scoped.Handle());
  return scoped.SubmitAndWait(m_deviceContext, error);
}

bool Context::EnsureSceneTargets(uint32_t width, uint32_t height, uint32_t sceneWidth,
                                 uint32_t sceneHeight, uint32_t outputWidth, uint32_t outputHeight,
                                 std::string *error)
{
  if (m_device == VK_NULL_HANDLE)
    return SetError(error, "Vulkan render targets requested before device initialization");
  if (m_frameActive)
    return SetError(error, "Vulkan render targets cannot be resized while a frame is recording");

  // A zero dimension means "leave unchanged", so VulkanNew3D and PostProcess can each set
  // their own sizes independently. Before the targets exist a missing size borrows a
  // supplied one.
  const bool valid = m_sceneTargets.IsValid();
  const uint32_t resolvedWidth =
      width != 0 ? width : (valid ? m_sceneTargets.Width() : (sceneWidth != 0 ? sceneWidth : outputWidth));
  const uint32_t resolvedHeight =
      height != 0 ? height : (valid ? m_sceneTargets.Height() : (sceneHeight != 0 ? sceneHeight : outputHeight));
  const uint32_t resolvedSceneWidth =
      sceneWidth != 0 ? sceneWidth
                      : (valid ? m_sceneTargets.SceneWidth() : (width != 0 ? width : outputWidth));
  const uint32_t resolvedSceneHeight =
      sceneHeight != 0 ? sceneHeight
                       : (valid ? m_sceneTargets.SceneHeight() : (height != 0 ? height : outputHeight));
  const uint32_t resolvedOutputWidth =
      outputWidth != 0
          ? outputWidth
          : (valid ? m_sceneTargets.OutputWidth() : (sceneWidth != 0 ? sceneWidth : width));
  const uint32_t resolvedOutputHeight =
      outputHeight != 0
          ? outputHeight
          : (valid ? m_sceneTargets.OutputHeight() : (sceneHeight != 0 ? sceneHeight : height));

  if (resolvedWidth == 0 || resolvedHeight == 0 || resolvedSceneWidth == 0 ||
      resolvedSceneHeight == 0 || resolvedOutputWidth == 0 || resolvedOutputHeight == 0)
    return SetError(error, "Vulkan render targets requested with an empty resolution");

  const bool geometryUnchanged = valid && m_sceneTargets.Width() == resolvedWidth &&
                                 m_sceneTargets.Height() == resolvedHeight &&
                                 m_sceneTargets.SceneWidth() == resolvedSceneWidth &&
                                 m_sceneTargets.SceneHeight() == resolvedSceneHeight &&
                                 m_sceneTargets.OutputWidth() == resolvedOutputWidth &&
                                 m_sceneTargets.OutputHeight() == resolvedOutputHeight;
  if (geometryUnchanged && m_pipelines.IsValid())
    return true;

  const VkResult idle = vkDeviceWaitIdle(m_device);
  if (idle != VK_SUCCESS)
    return SetError(error, VkError("vkDeviceWaitIdle", idle));

  if (!geometryUnchanged)
  {
    // The render passes are about to go, so every pipeline built against them must too.
    m_pipelines.Destroy(m_deviceContext);
    m_screenshotReadback.Destroy(m_deviceContext);
    m_screenshotSliceBytes = 0;
    ++m_screenshotGeneration;

    if (m_sceneTargets.IsValid())
    {
      if (!m_sceneTargets.Resize(m_deviceContext, resolvedWidth, resolvedHeight,
                                 resolvedSceneWidth, resolvedSceneHeight, resolvedOutputWidth,
                                 resolvedOutputHeight, error))
        return false;
    }
    else if (!m_sceneTargets.Create(m_deviceContext, resolvedWidth, resolvedHeight,
                                   resolvedSceneWidth, resolvedSceneHeight, resolvedOutputWidth,
                                   resolvedOutputHeight, error))
    {
      return false;
    }

    // Required after every Create()/Resize(), see VulkanRenderTargets.h.
    if (!RecordInitialSceneTransitions(error))
    {
      m_sceneTargets.Destroy(m_deviceContext);
      return false;
    }
  }

  m_targetInfo = TargetInfo{};
  m_targetInfo.scenePass = m_sceneTargets.ScenePassHandle(SceneTargets::ScenePass::ClearAll);
  m_targetInfo.scenePassNoLos =
      m_sceneTargets.ScenePassHandle(SceneTargets::ScenePass::ClearAllNoLos);
  m_targetInfo.scenePassTransFirst =
      m_sceneTargets.ScenePassHandle(SceneTargets::ScenePass::TransFirst);
  m_targetInfo.scenePassTransSecond =
      m_sceneTargets.ScenePassHandle(SceneTargets::ScenePass::TransSecond);
  m_targetInfo.sceneColourPass = m_sceneTargets.SceneColourPassHandle(SceneTargets::SimplePass::Clear);
  m_targetInfo.outputPass = m_sceneTargets.OutputPassHandle(SceneTargets::SimplePass::Clear);
  m_targetInfo.colourFormat = m_sceneTargets.ColourFormat();
  m_targetInfo.depthStencilFormat = m_sceneTargets.DepthStencilFormat();
  m_targetInfo.geometryShader = m_geometryShaderEnabled;

  if (!m_pipelines.IsValid() && !m_pipelines.Create(m_deviceContext, error))
  {
    m_pipelines.Destroy(m_deviceContext);
    return false;
  }

  if (!m_canBlitToSwapchain && !m_canCopyToSwapchain)
  {
    return SetError(error,
                    "The Vulkan swapchain format supports neither VK_FORMAT_FEATURE_BLIT_DST_BIT "
                    "nor a direct copy from VK_FORMAT_R8G8B8A8_UNORM, so the rendered image cannot "
                    "be presented. Use a driver that advertises blitting to its swapchain formats, "
                    "or run with GraphicsBackend=OpenGL");
  }
  if (!m_canBlitToSwapchain &&
      (m_sceneTargets.OutputWidth() != m_swapchainExtent.width ||
       m_sceneTargets.OutputHeight() != m_swapchainExtent.height))
  {
    return SetError(error,
                    "The Vulkan swapchain format cannot be blitted to, so the render resolution "
                    "must match the drawable exactly. Disable RenderScale or use a driver that "
                    "advertises VK_FORMAT_FEATURE_BLIT_DST_BIT for its swapchain formats");
  }

  return true;
}

// ---------------------------------------------------------------------------
// Uploads
// ---------------------------------------------------------------------------

bool Context::OneShotBufferUpload(VkBuffer destination, VkDeviceSize destinationOffset,
                                  const void *source, VkDeviceSize bytes, std::string *error)
{
  const uint8_t *cursor = static_cast<const uint8_t *>(source);
  VkDeviceSize remaining = bytes;
  VkDeviceSize offset = destinationOffset;

  while (remaining > 0)
  {
    const VkDeviceSize chunk = std::min<VkDeviceSize>(remaining, m_oneShotStaging.BytesPerFrame());
    m_oneShotStaging.BeginFrame(0);

    void *staged = nullptr;
    VkDeviceSize stagingOffset = 0;
    if (!m_oneShotStaging.Suballocate(chunk, 4, &staged, &stagingOffset))
      return SetError(error, "The Vulkan one-shot staging buffer could not satisfy an upload chunk");
    std::memcpy(staged, cursor, static_cast<size_t>(chunk));
    if (!m_oneShotStaging.FlushFrame(m_deviceContext, error))
      return false;

    ScopedCommandBuffer scoped;
    if (!scoped.Begin(m_deviceContext, error))
      return false;

    VkBufferCopy region{};
    region.srcOffset = stagingOffset;
    region.dstOffset = offset;
    region.size = chunk;
    vkCmdCopyBuffer(scoped.Handle(), m_oneShotStaging.Handle(), destination, 1, &region);

    if (!scoped.SubmitAndWait(m_deviceContext, error))
      return false;

    cursor += chunk;
    offset += chunk;
    remaining -= chunk;
  }
  return true;
}

bool Context::RecordBufferUpload(VkBuffer destination, VkDeviceSize destinationOffset,
                                 const void *source, VkDeviceSize bytes, std::string *error,
                                 bool forceOwnSubmission)
{
  if (destination == VK_NULL_HANDLE)
    return SetError(error, "Vulkan buffer upload received no destination buffer");
  if (bytes == 0)
    return true;
  if (source == nullptr)
    return SetError(error, "Vulkan buffer upload received no source data");

  if (m_frameActive && !forceOwnSubmission)
  {
    void *staged = nullptr;
    VkDeviceSize stagingOffset = 0;
    if (m_stagingRing.Suballocate(bytes, 4, &staged, &stagingOffset))
    {
      std::memcpy(staged, source, static_cast<size_t>(bytes));

      VkBufferCopy region{};
      region.srcOffset = stagingOffset;
      region.dstOffset = destinationOffset;
      region.size = bytes;
      RecordTransferHazardBarrier();
      vkCmdCopyBuffer(m_commandBuffers[m_currentFrame], m_stagingRing.Handle(), destination, 1, &region);
      return true;
    }
  }

  // No frame recording, or this frame's staging slice is spent. The one-shot path is
  // slower but bounded; it submits and waits mid-recording, so it is reported once.
  if (m_frameActive && !m_stagingFallbackReported)
  {
    ErrorLog("This frame's Vulkan staging buffer was exhausted, so a %llu byte upload fell back to "
             "its own submission. Rendering is correct but this stalls the frame.",
             static_cast<unsigned long long>(bytes));
    m_stagingFallbackReported = true;
  }
  return OneShotBufferUpload(destination, destinationOffset, source, bytes, error);
}

bool Context::RecordVertexUpload(VkBuffer destination, VkDeviceSize destinationOffset,
                                 const void *source, VkDeviceSize bytes, std::string *error,
                                 bool forceOwnSubmission)
{
  if (destination == VK_NULL_HANDLE)
    return SetError(error, "Vulkan vertex upload received no destination buffer");
  if (bytes == 0)
    return true;
  if (source == nullptr)
    return SetError(error, "Vulkan vertex upload received no source data");

  if (m_frameActive && !forceOwnSubmission)
  {
    void *staged = nullptr;
    VkDeviceSize stagingOffset = 0;
    if (m_vertexStaging.Suballocate(bytes, 4, &staged, &stagingOffset))
    {
      std::memcpy(staged, source, static_cast<size_t>(bytes));

      VkBufferCopy region{};
      region.srcOffset = stagingOffset;
      region.dstOffset = destinationOffset;
      region.size = bytes;
      vkCmdCopyBuffer(m_uploadCommandBuffers[m_currentFrame], m_vertexStaging.Handle(), destination,
                      1, &region);
      m_uploadRecorded = true;
      return true;
    }
  }
  return OneShotBufferUpload(destination, destinationOffset, source, bytes, error);
}

bool Context::RecordTextureBankUpload(TextureBank &bank, int level, int x, int y, int width,
                                      int height, std::string *error)
{
  if (!m_frameActive)
    return SetError(error, "Vulkan texture bank upload requested outside an active frame");
  // The staging slice holds a whole 2048x1024 sheet, both banks and every mip level, in one
  // frame, so a failure here means a malformed request or an unusual streaming volume.
  return bank.RecordUpload(m_commandBuffers[m_currentFrame], m_stagingRing, level, x, y, width,
                           height, error);
}

// ---------------------------------------------------------------------------
// Readback
// ---------------------------------------------------------------------------

VkDeviceSize Context::LosReadbackOffset(uint32_t frameIndex, uint32_t slot) const
{
  const uint32_t frame = frameIndex % kFramesInFlight;
  const uint32_t index = slot % kLosSlotsPerFrame;
  return m_losSliceBytes * frame + kLosSlotBytes * index;
}

bool Context::ReadLosSlot(uint32_t frameIndex, uint32_t slot, uint32_t *depthBits,
                          uint32_t *noLosReturn, std::string *error)
{
  if (depthBits == nullptr || noLosReturn == nullptr)
    return SetError(error, "Vulkan line-of-sight readback received no destination");
  if (!m_losReadback.Valid() || m_losReadback.MappedPointer() == nullptr)
    return SetError(error, "Vulkan line-of-sight readback buffer is unavailable");

  const uint32_t frame = frameIndex % kFramesInFlight;
  if (!m_losReadback.Invalidate(m_deviceContext, m_losSliceBytes * frame, m_losSliceBytes, error))
    return false;

  const uint8_t *base = static_cast<const uint8_t *>(m_losReadback.MappedPointer());
  uint32_t texel[2] = { 0, 0 };
  std::memcpy(texel, base + LosReadbackOffset(frame, slot), sizeof(texel));
  *depthBits = texel[0];
  *noLosReturn = texel[1];
  return true;
}

bool Context::EnsureScreenshotReadback(std::string *error)
{
  if (!m_sceneTargets.IsValid())
    return SetError(error, "Vulkan screenshot readback requested before the render targets exist");

  const VkDeviceSize slice =
      AlignUp(m_sceneTargets.ScreenshotBufferSize(),
              std::max<VkDeviceSize>(16, m_deviceProperties.limits.nonCoherentAtomSize));
  if (slice == 0)
    return SetError(error, "Vulkan screenshot readback requested for an empty output image");
  if (m_screenshotReadback.Valid() && m_screenshotSliceBytes == slice)
    return true;

  m_screenshotReadback.Destroy(m_deviceContext);
  m_screenshotSliceBytes = 0;
  ++m_screenshotGeneration;
  if (!m_screenshotReadback.Create(m_deviceContext, slice * kFramesInFlight,
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                   VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                   error))
    return false;
  if (m_screenshotReadback.MappedPointer() == nullptr)
  {
    m_screenshotReadback.Destroy(m_deviceContext);
    return SetError(error, "The Vulkan screenshot readback buffer could not be mapped");
  }
  m_screenshotSliceBytes = slice;
  return true;
}

VkDeviceSize Context::ScreenshotReadbackOffset(uint32_t frameIndex) const
{
  return m_screenshotSliceBytes * (frameIndex % kFramesInFlight);
}

const void *Context::ScreenshotReadbackPointer(uint32_t frameIndex) const
{
  const uint8_t *base = static_cast<const uint8_t *>(m_screenshotReadback.MappedPointer());
  if (base == nullptr)
    return nullptr;
  return base + ScreenshotReadbackOffset(frameIndex);
}

bool Context::InvalidateScreenshotReadback(uint32_t frameIndex, std::string *error)
{
  if (!m_screenshotReadback.Valid())
    return SetError(error, "Vulkan screenshot readback buffer is unavailable");
  return m_screenshotReadback.Invalidate(m_deviceContext, ScreenshotReadbackOffset(frameIndex),
                                         m_screenshotSliceBytes, error);
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

bool Context::BeginFrame(std::string *error)
{
  if (m_fatalError)
    return SetError(error, "Vulkan rendering stopped after a fatal queue submission or device error");
  if (m_frameActive)
    return SetError(error, "Vulkan BeginFrame called while another frame is active");

  int width = 0;
  int height = 0;
  SDL_Vulkan_GetDrawableSize(m_window, &width, &height);
  if (width == 0 || height == 0)
  {
    m_recreateRequested = true;
    return false;
  }
  if (m_recreateRequested || static_cast<uint32_t>(width) != m_swapchainExtent.width || static_cast<uint32_t>(height) != m_swapchainExtent.height)
  {
    if (!RecreateSwapchain(error))
      return false;
  }

  VkResult result = vkWaitForFences(m_device, 1, &m_inFlight[m_currentFrame], VK_TRUE, UINT64_MAX);
  if (result != VK_SUCCESS)
    return SetError(error, VkError("vkWaitForFences", result));

  // This frame index's previous submission has completed, so anything waiting on its
  // results drains without adding a stall.
  RunFrameCompletions(m_currentFrame);

  result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, m_imageAvailable[m_currentFrame], VK_NULL_HANDLE, &m_currentImage);
  if (result == VK_ERROR_OUT_OF_DATE_KHR)
  {
    m_recreateRequested = true;
    return false;
  }
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    return SetError(error, VkError("vkAcquireNextImageKHR", result));
  if (result == VK_SUBOPTIMAL_KHR)
    NoteSuboptimal();

  // The acquire succeeded, so m_imageAvailable[m_currentFrame] can only be consumed by a
  // submission. Failures from here are fatal rather than skippable: abandoning the
  // semaphore leaves it signalled and desynchronises every later frame.
  if (m_imagesInFlight[m_currentImage] != VK_NULL_HANDLE)
  {
    result = vkWaitForFences(m_device, 1, &m_imagesInFlight[m_currentImage], VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS)
    {
      m_fatalError = true;
      return SetError(error, VkError("vkWaitForFences(swapchain image)", result));
    }
  }
  m_imagesInFlight[m_currentImage] = m_inFlight[m_currentFrame];

  result = vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0);
  if (result != VK_SUCCESS)
  {
    m_fatalError = true;
    return SetError(error, VkError("vkResetCommandBuffer", result));
  }

  m_uniformRing.BeginFrame(m_currentFrame);
  m_stagingRing.BeginFrame(m_currentFrame);
  m_vertexStaging.BeginFrame(m_currentFrame);
  m_outputImageWritten = false;

  m_uploadRecorded = false;
  m_transferHazardOrdered = false;
  result = vkResetCommandBuffer(m_uploadCommandBuffers[m_currentFrame], 0);
  if (result != VK_SUCCESS)
  {
    m_fatalError = true;
    return SetError(error, VkError("vkResetCommandBuffer(uploads)", result));
  }
  {
    VkCommandBufferBeginInfo uploadBegin{};
    uploadBegin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    uploadBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(m_uploadCommandBuffers[m_currentFrame], &uploadBegin);
    if (result != VK_SUCCESS)
    {
      m_fatalError = true;
      return SetError(error, VkError("vkBeginCommandBuffer(uploads)", result));
    }
  }

  if (!RecordClearCommands(m_commandBuffers[m_currentFrame], m_currentImage, error))
  {
    m_fatalError = true;
    return false;
  }

  m_frameActive = true;
  return true;
}

bool Context::RecordClearCommands(VkCommandBuffer commandBuffer, uint32_t imageIndex, std::string *error)
{
  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VkResult result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
  if (result != VK_SUCCESS)
    return SetError(error, VkError("vkBeginCommandBuffer", result));

  VkImageMemoryBarrier toTransfer{};
  toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toTransfer.srcAccessMask = 0;
  toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toTransfer.oldLayout = m_swapchainImageInitialized[imageIndex] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
  toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.image = m_swapchainImages[imageIndex];
  toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  toTransfer.subresourceRange.baseMipLevel = 0;
  toTransfer.subresourceRange.levelCount = 1;
  toTransfer.subresourceRange.baseArrayLayer = 0;
  toTransfer.subresourceRange.layerCount = 1;
  // srcStageMask must be TRANSFER: the submit waits on the image-available semaphore at that
  // stage and this is the image's first use. BOTTOM_OF_PIPE or TOP_OF_PIPE establishes no such
  // dependency, which synchronization validation reports as a WAR against vkAcquireNextImageKHR.
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

  // No clear here: RecordOutputComposite() covers the whole image by copy or full-extent
  // blit, and issues its own clear on the one path where it does not run.

  // The vertex-read against transfer-write hazard barrier is issued lazily by
  // RecordTransferHazardBarrier(), so a frame that uploads nothing pays nothing.

  return true;
}

// Orders the previous frame's vertex fetch against this frame's first transfer write into a
// buffer those draws may still read. Once per frame, only when such a write occurs.
void Context::RecordTransferHazardBarrier()
{
  if (m_transferHazardOrdered)
    return;
  m_transferHazardOrdered = true;

  VkMemoryBarrier vertexHazard{};
  vertexHazard.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  vertexHazard.srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
  vertexHazard.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(m_commandBuffers[m_currentFrame], VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &vertexHazard, 0, nullptr, 0, nullptr);
}

bool Context::UploadRGBA(const uint32_t *pixels, uint32_t width, uint32_t height,
                         uint32_t x, uint32_t y, uint32_t outputWidth, uint32_t outputHeight,
                         std::string *error)
{
  if (!m_frameActive)
    return SetError(error, "Vulkan TileGen upload requested outside an active frame");
  if (pixels == nullptr || width == 0 || height == 0 || outputWidth == 0 || outputHeight == 0)
    return SetError(error, "Vulkan TileGen upload received an empty surface");
  if (m_uploadMapped[m_currentFrame] == nullptr)
    return SetError(error, "Vulkan TileGen upload buffer is unavailable");

  uint8_t *destination = static_cast<uint8_t *>(m_uploadMapped[m_currentFrame]);
  std::memset(destination, 0, static_cast<size_t>(m_uploadSize));

  const uint32_t clippedWidth = std::min(outputWidth, m_swapchainExtent.width > x ? m_swapchainExtent.width - x : 0u);
  const uint32_t clippedHeight = std::min(outputHeight, m_swapchainExtent.height > y ? m_swapchainExtent.height - y : 0u);
  const bool bgra = m_swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB ||
                    m_swapchainFormat == VK_FORMAT_B8G8R8A8_UNORM;
  for (uint32_t outputY = 0; outputY < clippedHeight; ++outputY)
  {
    const uint32_t sourceY = std::min(height - 1, static_cast<uint32_t>((static_cast<uint64_t>(outputY) * height) / outputHeight));
    for (uint32_t outputX = 0; outputX < clippedWidth; ++outputX)
    {
      const uint32_t sourceX = std::min(width - 1, static_cast<uint32_t>((static_cast<uint64_t>(outputX) * width) / outputWidth));
      const uint32_t source = pixels[sourceY * width + sourceX];
      uint8_t *target = destination + (static_cast<size_t>(y + outputY) * m_swapchainExtent.width + x + outputX) * 4;
      const uint8_t red = static_cast<uint8_t>(source >> 0);
      const uint8_t green = static_cast<uint8_t>(source >> 8);
      const uint8_t blue = static_cast<uint8_t>(source >> 16);
      target[0] = bgra ? blue : red;
      target[1] = green;
      target[2] = bgra ? red : blue;
      target[3] = 0xff;
    }
  }

  if (!m_uploadCoherent[m_currentFrame])
  {
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = m_uploadMemory[m_currentFrame];
    range.offset = 0;
    range.size = m_uploadAllocationSize[m_currentFrame];
    const VkResult result = vkFlushMappedMemoryRanges(m_device, 1, &range);
    if (result != VK_SUCCESS)
      return SetError(error, VkError("vkFlushMappedMemoryRanges(TileGen upload)", result));
  }

  // The frame starts with a transfer clear so frames without TileGen output stay black;
  // order this full-image upload after that write.
  VkImageMemoryBarrier uploadDependency{};
  uploadDependency.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  uploadDependency.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  uploadDependency.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  uploadDependency.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  uploadDependency.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  uploadDependency.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  uploadDependency.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  uploadDependency.image = m_swapchainImages[m_currentImage];
  uploadDependency.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  uploadDependency.subresourceRange.baseMipLevel = 0;
  uploadDependency.subresourceRange.levelCount = 1;
  uploadDependency.subresourceRange.baseArrayLayer = 0;
  uploadDependency.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(m_commandBuffers[m_currentFrame], VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                       1, &uploadDependency);

  VkBufferImageCopy copy{};
  copy.bufferOffset = 0;
  copy.bufferRowLength = m_swapchainExtent.width;
  copy.bufferImageHeight = m_swapchainExtent.height;
  copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy.imageSubresource.mipLevel = 0;
  copy.imageSubresource.baseArrayLayer = 0;
  copy.imageSubresource.layerCount = 1;
  copy.imageOffset = {0, 0, 0};
  copy.imageExtent = {m_swapchainExtent.width, m_swapchainExtent.height, 1};
  vkCmdCopyBufferToImage(m_commandBuffers[m_currentFrame], m_uploadBuffers[m_currentFrame],
                         m_swapchainImages[m_currentImage], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         1, &copy);
  return true;
}

bool Context::SurfaceRelayoutNeeded() const
{
  if (m_fatalError || !m_sceneTargets.IsValid() || m_swapchain == VK_NULL_HANDLE)
    return false;
  // Nothing to compare against while minimized or between a deferred recreation
  // and the next successful acquire.
  if (m_swapchainExtent.width == 0 || m_swapchainExtent.height == 0)
    return false;
  return m_swapchainExtent.width != m_sceneTargets.OutputWidth() ||
         m_swapchainExtent.height != m_sceneTargets.OutputHeight();
}

bool Context::RecordOutputComposite(std::string *error)
{
  if (!m_outputImageWritten || !m_sceneTargets.IsValid())
  {
    // Nothing rendered into the output image, so the swapchain image would otherwise
    // present whatever it last held.
    const VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;
    vkCmdClearColorImage(m_commandBuffers[m_currentFrame], m_swapchainImages[m_currentImage],
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    return true;
  }

  VkCommandBuffer commandBuffer = m_commandBuffers[m_currentFrame];
  Image &output = m_sceneTargets.OutputImage();
  output.RecordTransition(commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

  // The clear, and any TileGen upload, wrote this image in the transfer stage.
  VkImageMemoryBarrier dependency{};
  dependency.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  dependency.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  dependency.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  dependency.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  dependency.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  dependency.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  dependency.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  dependency.image = m_swapchainImages[m_currentImage];
  dependency.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  dependency.subresourceRange.baseMipLevel = 0;
  dependency.subresourceRange.levelCount = 1;
  dependency.subresourceRange.baseArrayLayer = 0;
  dependency.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &dependency);

  const uint32_t sourceWidth = m_sceneTargets.OutputWidth();
  const uint32_t sourceHeight = m_sceneTargets.OutputHeight();

  // A copy is preferred whenever the extents agree: identical pixels, but a driver may
  // implement a blit as a shader pass. Blitting is only needed to rescale.
  if (m_canCopyToSwapchain && sourceWidth == m_swapchainExtent.width &&
      sourceHeight == m_swapchainExtent.height)
  {
    VkImageCopy region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.mipLevel = 0;
    region.srcSubresource.baseArrayLayer = 0;
    region.srcSubresource.layerCount = 1;
    region.srcOffset = VkOffset3D{0, 0, 0};
    region.dstSubresource = region.srcSubresource;
    region.dstOffset = VkOffset3D{0, 0, 0};
    region.extent = VkExtent3D{sourceWidth, sourceHeight, 1};
    vkCmdCopyImage(commandBuffer, output.Handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   m_swapchainImages[m_currentImage], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region);
  }
  else if (m_canBlitToSwapchain)
  {
    VkImageBlit region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.mipLevel = 0;
    region.srcSubresource.baseArrayLayer = 0;
    region.srcSubresource.layerCount = 1;
    region.srcOffsets[0] = VkOffset3D{0, 0, 0};
    region.srcOffsets[1] = VkOffset3D{static_cast<int32_t>(sourceWidth),
                                      static_cast<int32_t>(sourceHeight), 1};
    region.dstSubresource = region.srcSubresource;
    region.dstOffsets[0] = VkOffset3D{0, 0, 0};
    region.dstOffsets[1] = VkOffset3D{static_cast<int32_t>(m_swapchainExtent.width),
                                      static_cast<int32_t>(m_swapchainExtent.height), 1};
    const VkFilter filter = (sourceWidth == m_swapchainExtent.width &&
                             sourceHeight == m_swapchainExtent.height)
                                ? VK_FILTER_NEAREST
                                : VK_FILTER_LINEAR;
    vkCmdBlitImage(commandBuffer, output.Handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   m_swapchainImages[m_currentImage], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region, filter);
  }
  else
  {
    output.RecordTransition(commandBuffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    return SetError(error,
                    "The Vulkan swapchain image can be neither blitted nor copied to from the "
                    "render output image, so the frame cannot be presented");
  }

  output.RecordTransition(commandBuffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  return true;
}

bool Context::EndFrame(std::string *error)
{
  if (!m_frameActive)
    return SetError(error, "Vulkan EndFrame called without a successful BeginFrame");

  // A composite failure still has to close the command buffer and drop the
  // frame, otherwise the recording state leaks into the next BeginFrame.
  std::string compositeError;
  const bool composited = RecordOutputComposite(&compositeError);

  VkImageMemoryBarrier toPresent{};
  toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toPresent.dstAccessMask = 0;
  toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toPresent.image = m_swapchainImages[m_currentImage];
  toPresent.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  toPresent.subresourceRange.baseMipLevel = 0;
  toPresent.subresourceRange.levelCount = 1;
  toPresent.subresourceRange.baseArrayLayer = 0;
  toPresent.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(m_commandBuffers[m_currentFrame], VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                       1, &toPresent);

  VkResult result = vkEndCommandBuffer(m_commandBuffers[m_currentFrame]);
  if (result != VK_SUCCESS)
  {
    m_frameActive = false;
    m_fatalError = true;
    return SetError(error, VkError("vkEndCommandBuffer", result));
  }

  if (!composited)
  {
    m_frameActive = false;
    m_fatalError = true;
    return SetError(error, compositeError);
  }

  // Everything written into the host-visible rings this frame becomes visible
  // to the device here, before the submission that reads it.
  if (!m_uniformRing.FlushFrame(m_deviceContext, error) ||
      !m_stagingRing.FlushFrame(m_deviceContext, error) ||
      !m_vertexStaging.FlushFrame(m_deviceContext, error))
  {
    m_frameActive = false;
    m_fatalError = true;
    return false;
  }

  result = vkResetFences(m_device, 1, &m_inFlight[m_currentFrame]);
  if (result != VK_SUCCESS)
  {
    m_frameActive = false;
    m_fatalError = true;
    return SetError(error, VkError("vkResetFences", result));
  }

  // Always ended so the next frame resets it cleanly, but only submitted when it holds
  // something. The separate submission is what makes the copies visible to the vertex
  // fetch; the semaphore keeps the CPU out of it.
  result = vkEndCommandBuffer(m_uploadCommandBuffers[m_currentFrame]);
  if (result != VK_SUCCESS)
  {
    m_frameActive = false;
    m_fatalError = true;
    return SetError(error, VkError("vkEndCommandBuffer(uploads)", result));
  }
  if (m_uploadRecorded)
  {
    VkSubmitInfo uploadSubmit{};
    uploadSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    uploadSubmit.commandBufferCount = 1;
    uploadSubmit.pCommandBuffers = &m_uploadCommandBuffers[m_currentFrame];
    uploadSubmit.signalSemaphoreCount = 1;
    uploadSubmit.pSignalSemaphores = &m_uploadDone[m_currentFrame];
    result = vkQueueSubmit(m_graphicsQueue, 1, &uploadSubmit, VK_NULL_HANDLE);
    if (result != VK_SUCCESS)
    {
      m_frameActive = false;
      m_fatalError = true;
      return SetError(error, VkError("vkQueueSubmit(uploads)", result));
    }
  }

  const VkSemaphore waitSemaphores[2] = { m_imageAvailable[m_currentFrame],
                                          m_uploadDone[m_currentFrame] };
  const VkPipelineStageFlags waitStages[2] = { VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               VK_PIPELINE_STAGE_VERTEX_INPUT_BIT };
  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.waitSemaphoreCount = m_uploadRecorded ? 2u : 1u;
  submitInfo.pWaitSemaphores = waitSemaphores;
  submitInfo.pWaitDstStageMask = waitStages;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &m_commandBuffers[m_currentFrame];
  submitInfo.signalSemaphoreCount = 1;
  submitInfo.pSignalSemaphores = &m_renderFinished[m_currentImage];

  result = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlight[m_currentFrame]);
  if (result != VK_SUCCESS)
  {
    m_frameActive = false;
    m_fatalError = true;
    return SetError(error, VkError("vkQueueSubmit", result));
  }
  m_swapchainImageInitialized[m_currentImage] = true;

  VkPresentInfoKHR presentInfo{};
  presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  presentInfo.waitSemaphoreCount = 1;
  presentInfo.pWaitSemaphores = &m_renderFinished[m_currentImage];
  presentInfo.swapchainCount = 1;
  presentInfo.pSwapchains = &m_swapchain;
  presentInfo.pImageIndices = &m_currentImage;
  result = vkQueuePresentKHR(m_presentQueue, &presentInfo);
  m_frameActive = false;

  if (result == VK_ERROR_OUT_OF_DATE_KHR)
    m_recreateRequested = true;
  else if (result == VK_SUBOPTIMAL_KHR)
    NoteSuboptimal();
  else if (result != VK_SUCCESS)
  {
    m_fatalError = true;
    return SetError(error, VkError("vkQueuePresentKHR", result));
  }

  m_currentFrame = (m_currentFrame + 1) % kFramesInFlight;
  return true;
}

void Context::LogDeviceInfo() const
{
  if (m_physicalDevice == VK_NULL_HANDLE)
    return;

  const char *deviceType = "other";
  switch (m_deviceProperties.deviceType)
  {
  case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: deviceType = "integrated GPU"; break;
  case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   deviceType = "discrete GPU";   break;
  case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    deviceType = "virtual GPU";    break;
  case VK_PHYSICAL_DEVICE_TYPE_CPU:            deviceType = "CPU";            break;
  default: break;
  }

  const VkPhysicalDeviceLimits &limits = m_deviceProperties.limits;
  const uint32_t api = m_deviceProperties.apiVersion;
  const uint32_t driver = m_deviceProperties.driverVersion;

  InfoLog("Vulkan information:");
  InfoLog("  Device                   : %s", m_deviceProperties.deviceName);
  InfoLog("  Device type              : %s", deviceType);
  InfoLog("  API version              : %u.%u.%u", VK_VERSION_MAJOR(api), VK_VERSION_MINOR(api),
          VK_VERSION_PATCH(api));
  InfoLog("  Driver version           : %u.%u.%u", VK_VERSION_MAJOR(driver),
          VK_VERSION_MINOR(driver), VK_VERSION_PATCH(driver));
  InfoLog("  Maximum Texture Size     : %u texels", limits.maxImageDimension2D);
  InfoLog("  Maximum Vertex Attributes: %u", limits.maxVertexInputAttributes);
  InfoLog("  Max Colour Attachments   : %u", limits.maxColorAttachments);
  InfoLog("  Maximum Viewport Size    : %ux%u", limits.maxViewportDimensions[0],
          limits.maxViewportDimensions[1]);
  InfoLog("  Uniform Offset Alignment : %llu bytes",
          static_cast<unsigned long long>(limits.minUniformBufferOffsetAlignment));
  InfoLog("  Geometry Shader          : %s", m_geometryShaderEnabled ? "yes" : "no");
  InfoLog("  Swapchain                : %u images, %ux%u, %s",
          static_cast<uint32_t>(m_swapchainImages.size()), m_swapchainExtent.width,
          m_swapchainExtent.height, m_vsync ? "FIFO (vsync on)" : "immediate or mailbox");
  // A non-coherent ring takes the explicit flush path, so it is worth reporting.
  InfoLog("  Staging ring             : %llu MB per frame, %s",
          static_cast<unsigned long long>(m_stagingRing.BytesPerFrameSlice() / (1024 * 1024)),
          m_stagingRing.IsCoherent() ? "host coherent" : "NOT coherent, needs explicit flushes");
  InfoLog("  Uniform ring             : %llu MB per frame, %s",
          static_cast<unsigned long long>(m_uniformRing.BytesPerFrameSlice() / (1024 * 1024)),
          m_uniformRing.IsCoherent() ? "host coherent" : "NOT coherent, needs explicit flushes");
  InfoLog("");
}

const char *Context::DeviceName() const
{
  return m_physicalDevice == VK_NULL_HANDLE ? "uninitialized" : m_deviceProperties.deviceName;
}

uint32_t Context::ApiVersion() const
{
  return m_physicalDevice == VK_NULL_HANDLE ? 0 : m_deviceProperties.apiVersion;
}

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
