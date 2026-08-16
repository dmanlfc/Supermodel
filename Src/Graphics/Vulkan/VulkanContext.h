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

/*
 * VulkanContext.h
 *
 * Instance, device, swapchain and frame graph for the Vulkan backend.
 *
 * Renderers own no host-visible memory: the uniform and staging rings live here,
 * reset by BeginFrame() and flushed by EndFrame(). An upload too large for a
 * frame's slice falls back to a bounded one-shot submit-and-wait.
 *
 * The swapchain format prefers UNORM over SRGB, an SRGB image adding a
 * linear-to-sRGB encode the OpenGL renderer never performed.
 */

#ifndef INCLUDED_VULKAN_CONTEXT_H
#define INCLUDED_VULKAN_CONTEXT_H

#ifdef SUPERMODEL_VULKAN

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "VulkanPipelines.h"
#include "VulkanRenderTargets.h"
#include "VulkanResources.h"
#include "VulkanTextureBank.h"

struct SDL_Window;

namespace Vulkan
{

class Context
{
public:
  // Three, not two, so the next frame's command buffer is free while the GPU works
  // on the current one; with two, recording waits on the frame fence and a frame
  // costs CPU + GPU instead of max(CPU, GPU). Costs one frame of latency.
  static constexpr uint32_t kFramesInFlight = 3;

  // Textures, tilegen surfaces and ROM vertices stage through this. Holds the worst
  // cases together: a 10.7 MB texture sheet invalidation plus 1.5 MB of tilegen
  // layers. Running out costs a synchronous submission, not correctness.
  static constexpr VkDeviceSize kStagingRingBytesPerFrame = 16u * 1024u * 1024u;

  // Dynamic vertices stage separately so texture streaming cannot starve them.
  // Holds MAX_RAM_VERTS * sizeof(New3D::FVertex) = 17.2 MB.
  static constexpr VkDeviceSize kVertexStagingBytesPerFrame = 18u * 1024u * 1024u;

  // One LOS texel per Real3D priority layer, per frame in flight.
  static constexpr uint32_t kLosSlotsPerFrame = 4;
  static constexpr VkDeviceSize kLosSlotBytes = SceneTargets::kLosTexelBytes;

  // A plain function pointer so registering one allocates nothing.
  using FrameCompletionFn = void (*)(void *user, uint64_t token);

  Context() = default;
  ~Context();

  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;

  // quadRendering additionally requires VkPhysicalDeviceFeatures::geometryShader.
  // independentBlend is mandatory: the New3D layer scheme is expressed entirely
  // with per-attachment colour write masks.
  bool Init(SDL_Window *window, bool vsync, std::string *error);
  bool Init(SDL_Window *window, bool vsync, bool quadRendering, std::string *error);
  void Shutdown();

  // Returns false while minimized or when swapchain recreation is deferred; in
  // that case *error is left empty and the caller should simply skip the frame.
  bool BeginFrame(std::string *error);
  bool EndFrame(std::string *error);
  bool RecreateSwapchain(std::string *error);
  void NoteSuboptimal();

  void RequestSwapchainRecreation();

  const DeviceContext &Device() const { return m_deviceContext; }
  VkDevice Handle() const { return m_device; }
  uint32_t GraphicsQueueFamily() const;

  uint32_t FrameIndex() const { return m_currentFrame; }
  static constexpr uint32_t FramesInFlight() { return kFramesInFlight; }

  // VK_NULL_HANDLE outside an active frame.
  VkCommandBuffer CommandBuffer() const;
  bool FrameActive() const { return m_frameActive; }

  bool GeometryShaderAvailable() const { return m_geometryShaderEnabled; }
  bool QuadRenderingRequested() const { return m_quadRenderingRequested; }

  RingBuffer &UniformRing() { return m_uniformRing; }
  RingBuffer &StagingRing() { return m_stagingRing; }
  VkDeviceSize UniformOffsetAlignment() const { return m_uniformOffsetAlignment; }

  // Safe outside an active frame. forceOwnSubmission skips the frame's staging
  // ring and copies on its own submission: slow, but independent of the frame.
  bool RecordBufferUpload(VkBuffer destination, VkDeviceSize destinationOffset,
                          const void *source, VkDeviceSize bytes, std::string *error,
                          bool forceOwnSubmission = false);

  // As above, but staged through the dedicated dynamic vertex ring.
  bool RecordVertexUpload(VkBuffer destination, VkDeviceSize destinationOffset, const void *source,
                          VkDeviceSize bytes, std::string *error, bool forceOwnSubmission = false);

  bool RecordTextureBankUpload(TextureBank &bank, int level, int x, int y, int width, int height,
                               std::string *error);

  // Idles the device and records SceneTargets::RecordInitialTransitions(). Not
  // valid while a frame is active. No single caller knows all three sizes, so a
  // zero dimension means "leave this one as it is".
  bool EnsureSceneTargets(uint32_t width, uint32_t height, uint32_t sceneWidth,
                          uint32_t sceneHeight, uint32_t outputWidth, uint32_t outputHeight,
                          std::string *error);

  // True when the swapchain no longer matches the render targets, the display
  // server having resized the surface behind ResizeVideoScreen()'s back.
  // Presenting anyway rescales the frame, so the caller should re-lay out.
  bool SurfaceRelayoutNeeded() const;

  bool SceneTargetsValid() const { return m_sceneTargets.IsValid(); }
  SceneTargets &Targets() { return m_sceneTargets; }
  const SceneTargets &Targets() const { return m_sceneTargets; }
  PipelineCache &Pipelines() { return m_pipelines; }
  const TargetInfo &PipelineTargets() const { return m_targetInfo; }

  // Tells EndFrame() that SceneTargets::OutputImage() holds this frame's image.
  void MarkOutputImageWritten() { m_outputImageWritten = true; }

  VkBuffer LosReadbackBuffer() const { return m_losReadback.Handle(); }
  VkDeviceSize LosReadbackOffset(uint32_t frameIndex, uint32_t slot) const;
  // depthBits receives the raw floatBitsToUint(gl_FragDepth) value, noLosReturn
  // the flag in G.
  bool ReadLosSlot(uint32_t frameIndex, uint32_t slot, uint32_t *depthBits, uint32_t *noLosReturn,
                   std::string *error);

  // Allocated on first request, and released by EnsureSceneTargets() when the
  // output resolution changes.
  bool EnsureScreenshotReadback(std::string *error);
  // A caller holding a recorded-but-unwritten readback must discard it if this
  // has moved since recording: the buffer and its dimensions are both gone.
  uint64_t ScreenshotReadbackGeneration() const { return m_screenshotGeneration; }
  VkBuffer ScreenshotReadbackBuffer() const { return m_screenshotReadback.Handle(); }
  VkDeviceSize ScreenshotReadbackOffset(uint32_t frameIndex) const;
  // Valid after InvalidateScreenshotReadback().
  const void *ScreenshotReadbackPointer(uint32_t frameIndex) const;
  bool InvalidateScreenshotReadback(uint32_t frameIndex, std::string *error);

  // Runs once the frame being recorded completes. Fails only when the per-frame
  // slot table is full.
  bool AddFrameCompletion(FrameCompletionFn function, void *user, uint64_t token,
                          std::string *error);

  // A renderer must call this while being destroyed, or a later frame will invoke
  // a callback on a dead object.
  void CancelFrameCompletions(void *user);

  // Legacy CPU 2D path, superseded by drawing into SceneTargets::OutputImage()
  // plus MarkOutputImageWritten(). Nearest-neighbor scale into the drawable
  // rectangle; the surrounding image stays black.
  bool UploadRGBA(const uint32_t *pixels, uint32_t width, uint32_t height,
                  uint32_t x, uint32_t y, uint32_t outputWidth, uint32_t outputHeight,
                  std::string *error);

  const char *DeviceName() const;

  // Mirrors PrintGLInfo's "OpenGL information:" block.
  void LogDeviceInfo() const;

  uint32_t ApiVersion() const;
  VkFormat SwapchainFormat() const { return m_swapchainFormat; }
  VkExtent2D SwapchainExtent() const { return m_swapchainExtent; }

private:
  // One R3DUniforms block per draw, so this bounds a frame's state changes.
  static constexpr VkDeviceSize kUniformRingBytesPerFrame = 8u * 1024u * 1024u;
  static constexpr VkDeviceSize kOneShotStagingBytes = 8u * 1024u * 1024u;
  static constexpr uint32_t kMaxFrameCompletions = 16;

  struct QueueFamilies
  {
    int graphics = -1;
    int present = -1;

    bool Complete() const { return graphics >= 0 && present >= 0; }
  };

  struct SwapchainSupport
  {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
  };

  struct FrameCompletion
  {
    FrameCompletionFn function = nullptr;
    void *user = nullptr;
    uint64_t token = 0;
  };

  struct FrameCompletionList
  {
    std::array<FrameCompletion, kMaxFrameCompletions> entries{};
    uint32_t count = 0;
  };

  bool CreateInstance(std::string *error);
  bool CreateSurface(std::string *error);
  bool SelectPhysicalDevice(std::string *error);
  bool CreateDevice(std::string *error);
  bool CreateCommandResources(std::string *error);
  bool CreateSyncResources(std::string *error);
  bool CreatePresentSemaphores(std::string *error);
  void DestroyPresentSemaphores();
  bool CreateSwapchain(VkSwapchainKHR oldSwapchain, std::string *error);
  bool CreateUploadResources(std::string *error);
  bool CreateStreamingResources(std::string *error);
  void DestroyStreamingResources();
  void DestroyUploadResources();
  void DestroySceneResources();
  uint32_t FindMemoryTypeIndex(uint32_t typeBits, VkMemoryPropertyFlags required,
                               VkMemoryPropertyFlags preferred, bool *coherent) const;
  void DestroySwapchain();

  QueueFamilies FindQueueFamilies(VkPhysicalDevice device) const;
  bool SupportsRequiredDeviceExtensions(VkPhysicalDevice device) const;
  SwapchainSupport QuerySwapchainSupport(VkPhysicalDevice device) const;
  VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &formats) const;
  VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR> &modes) const;
  VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR &capabilities) const;
  void UpdateSwapchainCompositeSupport();
  bool RecordClearCommands(VkCommandBuffer commandBuffer, uint32_t imageIndex, std::string *error);
  bool RecordOutputComposite(std::string *error);
  bool RecordInitialSceneTransitions(std::string *error);
  void RecordTransferHazardBarrier();
  bool OneShotBufferUpload(VkBuffer destination, VkDeviceSize destinationOffset,
                           const void *source, VkDeviceSize bytes, std::string *error);
  void RunFrameCompletions(uint32_t frameIndex);
  bool SetError(std::string *error, const std::string &message) const;

  SDL_Window *m_window = nullptr;
  bool m_vsync = true;
  bool m_quadRenderingRequested = false;
  bool m_geometryShaderEnabled = false;
  bool m_loaderLoaded = false;
  bool m_recreateRequested = false;
  // Extent already rebuilt once in response to VK_SUBOPTIMAL_KHR.
  VkExtent2D m_suboptimalExtent{};
  bool m_suboptimalReported = false;
  bool m_stagingFallbackReported = false;
  bool m_frameActive = false;
  bool m_fatalError = false;
  bool m_outputImageWritten = false;
  bool m_canBlitToSwapchain = false;
  bool m_canCopyToSwapchain = false;

  VkInstance m_instance = VK_NULL_HANDLE;
  VkSurfaceKHR m_surface = VK_NULL_HANDLE;
  VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties m_deviceProperties{};
  VkDevice m_device = VK_NULL_HANDLE;
  VkQueue m_graphicsQueue = VK_NULL_HANDLE;
  VkQueue m_presentQueue = VK_NULL_HANDLE;
  QueueFamilies m_queueFamilies;
  DeviceContext m_deviceContext{};

  VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
  VkFormat m_swapchainFormat = VK_FORMAT_UNDEFINED;
  VkExtent2D m_swapchainExtent{};
  std::vector<VkImage> m_swapchainImages;
  std::vector<bool> m_swapchainImageInitialized;
  std::vector<VkFence> m_imagesInFlight;

  std::array<VkBuffer, kFramesInFlight> m_uploadBuffers{};
  std::array<VkDeviceMemory, kFramesInFlight> m_uploadMemory{};
  std::array<void *, kFramesInFlight> m_uploadMapped{};
  std::array<bool, kFramesInFlight> m_uploadCoherent{};
  std::array<VkDeviceSize, kFramesInFlight> m_uploadAllocationSize{};
  VkDeviceSize m_uploadSize = 0;

  RingBuffer m_uniformRing;
  RingBuffer m_stagingRing;
  RingBuffer m_vertexStaging;
  RingBuffer m_oneShotStaging;
  Buffer m_losReadback;
  Buffer m_screenshotReadback;
  VkDeviceSize m_losSliceBytes = 0;
  VkDeviceSize m_screenshotSliceBytes = 0;
  uint64_t m_screenshotGeneration = 0;
  VkDeviceSize m_uniformOffsetAlignment = 1;

  SceneTargets m_sceneTargets;
  PipelineCache m_pipelines;
  TargetInfo m_targetInfo{};

  std::array<FrameCompletionList, kFramesInFlight> m_frameCompletions{};

  VkCommandPool m_commandPool = VK_NULL_HANDLE;
  std::array<VkCommandBuffer, kFramesInFlight> m_commandBuffers{};
  // Dynamic vertex copies are submitted ahead of the frame: recording them into
  // the frame's own buffer does not order them against this hardware's vertex
  // fetch. The frame waits on m_uploadDone rather than blocking the CPU.
  std::array<VkCommandBuffer, kFramesInFlight> m_uploadCommandBuffers{};
  std::array<VkSemaphore, kFramesInFlight> m_uploadDone{};
  bool m_uploadRecorded = false;
  bool m_transferHazardOrdered = false;
  std::array<VkSemaphore, kFramesInFlight> m_imageAvailable{};
  // One per swapchain image, not per frame in flight: a binary semaphore must
  // not be re-signalled while an earlier present may still be waiting on it.
  std::vector<VkSemaphore> m_renderFinished;
  std::array<VkFence, kFramesInFlight> m_inFlight{};
  uint32_t m_currentFrame = 0;
  uint32_t m_currentImage = 0;
};

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
#endif // INCLUDED_VULKAN_CONTEXT_H
