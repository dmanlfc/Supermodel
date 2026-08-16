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

#include "VulkanRenderTargets.h"

#include <cstring>
#include <sstream>
#include <vector>

#include "OSD/Logger.h"

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

// Steady-state layouts, see the header comment.
constexpr VkImageLayout kColourSteadyLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
constexpr VkImageLayout kDepthSteadyLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
constexpr VkImageLayout kDepthCopySteadyLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

constexpr VkImageAspectFlags kDepthStencilAspect =
    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

// The two depth/stencil formats GL_DEPTH32F_STENCIL8 can map onto, in
// preference order.
constexpr std::array<VkFormat, 2> kDepthStencilCandidates = {
  VK_FORMAT_D32_SFLOAT_S8_UINT,
  VK_FORMAT_D24_UNORM_S8_UINT,
};

const char *FormatName(VkFormat format)
{
  switch (format)
  {
  case VK_FORMAT_D32_SFLOAT_S8_UINT: return "VK_FORMAT_D32_SFLOAT_S8_UINT";
  case VK_FORMAT_D24_UNORM_S8_UINT: return "VK_FORMAT_D24_UNORM_S8_UINT";
  case VK_FORMAT_R8G8B8A8_UNORM: return "VK_FORMAT_R8G8B8A8_UNORM";
  case VK_FORMAT_R32G32_UINT: return "VK_FORMAT_R32G32_UINT";
  default: return "an unnamed VkFormat";
  }
}

bool FormatSupportsOptimalFeatures(const DeviceContext &device, VkFormat format,
                                   VkFormatFeatureFlags features)
{
  VkFormatProperties properties{};
  vkGetPhysicalDeviceFormatProperties(device.physicalDevice, format, &properties);
  return (properties.optimalTilingFeatures & features) == features;
}

// A colour attachment that is loaded or cleared but never changes layout across the pass.
// storeOp matters on a tile based GPU: STORE writes the attachment out of tile memory for
// every tile and LOAD reads it back, so the ops are per attachment per pass, not uniform.
VkAttachmentDescription MakeColourAttachment(VkFormat format, VkAttachmentLoadOp loadOp,
                                             VkAttachmentStoreOp storeOp)
{
  VkAttachmentDescription attachment{};
  attachment.format = format;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = loadOp;
  attachment.storeOp = storeOp;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = kColourSteadyLayout;
  attachment.finalLayout = kColourSteadyLayout;
  return attachment;
}

VkAttachmentDescription MakeDepthStencilAttachment(VkFormat format, VkAttachmentLoadOp loadOp,
                                                   VkAttachmentStoreOp storeOp)
{
  VkAttachmentDescription attachment{};
  attachment.format = format;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = loadOp;
  attachment.storeOp = storeOp;
  attachment.stencilLoadOp = loadOp;
  attachment.stencilStoreOp = storeOp;
  attachment.initialLayout = kDepthSteadyLayout;
  attachment.finalLayout = kDepthSteadyLayout;
  return attachment;
}

// Passes are recorded back to back onto the same images with the depth store/restore copies
// interleaved, so both directions must cover attachment access as well as transfers.
std::array<VkSubpassDependency, 2> MakeExternalDependencies(bool withDepth)
{
  const VkPipelineStageFlags colourStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  const VkPipelineStageFlags depthStages =
      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  const VkPipelineStageFlags attachmentStages = withDepth ? (colourStages | depthStages) : colourStages;

  const VkAccessFlags colourAccess =
      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  const VkAccessFlags depthAccess =
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  const VkAccessFlags attachmentAccess = withDepth ? (colourAccess | depthAccess) : colourAccess;

  std::array<VkSubpassDependency, 2> dependencies{};

  // VERTEX_INPUT is on the incoming edge because the per-frame vertex upload is a transfer
  // recorded into the same command buffer immediately before these passes; without it the
  // vertex fetch is unordered against the copy that writes the geometry it reads.
  dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[0].dstSubpass = 0;
  dependencies[0].srcStageMask = attachmentStages | VK_PIPELINE_STAGE_TRANSFER_BIT;
  dependencies[0].dstStageMask = attachmentStages | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
  dependencies[0].srcAccessMask = attachmentAccess | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  dependencies[0].dstAccessMask = attachmentAccess | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
  dependencies[0].dependencyFlags = 0;

  dependencies[1].srcSubpass = 0;
  dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[1].srcStageMask = attachmentStages;
  dependencies[1].dstStageMask = attachmentStages | VK_PIPELINE_STAGE_TRANSFER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dependencies[1].srcAccessMask = attachmentAccess;
  dependencies[1].dstAccessMask = attachmentAccess | VK_ACCESS_TRANSFER_READ_BIT |
                                  VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  dependencies[1].dependencyFlags = 0;

  return dependencies;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

SceneTargets::~SceneTargets()
{
  if (m_created)
    Destroy(m_deviceContext);
}

bool SceneTargets::SetError(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool SceneTargets::Create(const DeviceContext &device, uint32_t width, uint32_t height,
                          uint32_t sceneWidth, uint32_t sceneHeight, uint32_t outputWidth,
                          uint32_t outputHeight, std::string *error)
{
  if (m_created)
    Destroy(device);

  if (device.device == VK_NULL_HANDLE || device.physicalDevice == VK_NULL_HANDLE)
    return SetError(error, "SceneTargets::Create received an incomplete DeviceContext");
  if (width == 0 || height == 0)
    return SetError(error, "SceneTargets::Create received a zero-sized attachment resolution");
  if (sceneWidth == 0 || sceneHeight == 0)
    return SetError(error, "SceneTargets::Create received a zero-sized scene resolution");
  if (outputWidth == 0 || outputHeight == 0)
    return SetError(error, "SceneTargets::Create received a zero-sized output resolution");

  m_deviceContext = device;
  m_width = width;
  m_height = height;
  m_sceneWidth = sceneWidth;
  m_sceneHeight = sceneHeight;
  m_outputWidth = outputWidth;
  m_outputHeight = outputHeight;

  // Destroy() is safe on a partially built object, so every failure below unwinds through it.
  if (!CheckColourFormats(device, error) || !ChooseDepthStencilFormat(device, error) ||
      !CreateImages(device, error) || !CreateRenderPasses(device, error) ||
      !CreateFramebuffers(device, error))
  {
    Destroy(device);
    return false;
  }

  m_created = true;
  return true;
}

void SceneTargets::Destroy(const DeviceContext &device)
{
  if (device.device != VK_NULL_HANDLE)
  {
    for (VkFramebuffer framebuffer : m_sceneFramebuffers)
    {
      if (framebuffer != VK_NULL_HANDLE)
        vkDestroyFramebuffer(device.device, framebuffer, nullptr);
    }
    if (m_sceneColourFramebuffer != VK_NULL_HANDLE)
      vkDestroyFramebuffer(device.device, m_sceneColourFramebuffer, nullptr);
    if (m_outputFramebuffer != VK_NULL_HANDLE)
      vkDestroyFramebuffer(device.device, m_outputFramebuffer, nullptr);

    for (VkRenderPass &pass : m_scenePasses)
    {
      if (pass != VK_NULL_HANDLE)
        vkDestroyRenderPass(device.device, pass, nullptr);
      pass = VK_NULL_HANDLE;
    }
    for (VkRenderPass &pass : m_sceneColourPasses)
    {
      if (pass != VK_NULL_HANDLE)
        vkDestroyRenderPass(device.device, pass, nullptr);
      pass = VK_NULL_HANDLE;
    }
    for (VkRenderPass &pass : m_outputPasses)
    {
      if (pass != VK_NULL_HANDLE)
        vkDestroyRenderPass(device.device, pass, nullptr);
      pass = VK_NULL_HANDLE;
    }

    m_colour.Destroy(device);
    m_trans0.Destroy(device);
    m_trans1.Destroy(device);
    m_los.Destroy(device);
    m_depthStencil.Destroy(device);
    m_depthStencilCopy.Destroy(device);
    m_sceneColour.Destroy(device);
    m_output.Destroy(device);
  }

  m_sceneFramebuffers.fill(VK_NULL_HANDLE);
  m_sceneColourFramebuffer = VK_NULL_HANDLE;
  m_outputFramebuffer = VK_NULL_HANDLE;
  m_scenePasses.fill(VK_NULL_HANDLE);
  m_sceneColourPasses.fill(VK_NULL_HANDLE);
  m_outputPasses.fill(VK_NULL_HANDLE);

  m_outputAliasesSceneColour = false;
  m_created = false;
  m_width = 0;
  m_height = 0;
  m_sceneWidth = 0;
  m_sceneHeight = 0;
  m_outputWidth = 0;
  m_outputHeight = 0;
  m_depthStencilFormat = VK_FORMAT_UNDEFINED;
  m_deviceContext = DeviceContext{};
}

bool SceneTargets::Resize(const DeviceContext &device, uint32_t width, uint32_t height,
                          uint32_t sceneWidth, uint32_t sceneHeight, uint32_t outputWidth,
                          uint32_t outputHeight, std::string *error)
{
  Destroy(device);
  return Create(device, width, height, sceneWidth, sceneHeight, outputWidth, outputHeight, error);
}

// ---------------------------------------------------------------------------
// Formats
// ---------------------------------------------------------------------------

bool SceneTargets::CheckColourFormats(const DeviceContext &device, std::string *error)
{
  // Colour attachments 0..2 are sampled by the composite shaders as well as written.
  const VkFormatFeatureFlags colourFeatures =
      VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  if (!FormatSupportsOptimalFeatures(device, kColourFormat, colourFeatures))
  {
    std::ostringstream message;
    message << "This device cannot use " << FormatName(kColourFormat)
            << " as a sampled colour attachment, which the New3D framebuffers require";
    return SetError(error, message.str());
  }

  // The LOS attachment is read back through a buffer copy, never sampled.
  if (!FormatSupportsOptimalFeatures(device, kLosFormat, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
  {
    std::ostringstream message;
    message << "This device cannot use " << FormatName(kLosFormat)
            << " as a colour attachment, which the line-of-sight buffer requires";
    return SetError(error, message.str());
  }

  return true;
}

// The OpenGL renderers get a real 32 bit float depth buffer even where stencil cannot be
// packed beside one, since the GL driver may keep them in separate buffers. Render passes
// cannot express that; dynamic rendering can attach D32_SFLOAT and S8_UINT independently.
void SceneTargets::LogDepthPrecisionOptions(const DeviceContext &device)
{
  const bool depth32 = FormatSupportsOptimalFeatures(device, VK_FORMAT_D32_SFLOAT,
                                                     VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
  const bool stencil8 = FormatSupportsOptimalFeatures(device, VK_FORMAT_S8_UINT,
                                                      VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);

  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(device.physicalDevice, &properties);
  bool dynamicRendering = properties.apiVersion >= VK_API_VERSION_1_3;
  if (!dynamicRendering)
  {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device.physicalDevice, nullptr, &count, nullptr) ==
            VK_SUCCESS &&
        count > 0)
    {
      std::vector<VkExtensionProperties> extensions(count);
      if (vkEnumerateDeviceExtensionProperties(device.physicalDevice, nullptr, &count,
                                               extensions.data()) == VK_SUCCESS)
      {
        for (const VkExtensionProperties &extension : extensions)
        {
          if (std::strcmp(extension.extensionName, "VK_KHR_dynamic_rendering") == 0)
          {
            dynamicRendering = true;
            break;
          }
        }
      }
    }
  }

  InfoLog("Vulkan separate depth/stencil support on this device: D32_SFLOAT depth attachment %s, "
          "S8_UINT stencil attachment %s, dynamic rendering %s (device Vulkan %u.%u.%u).",
          depth32 ? "yes" : "no", stencil8 ? "yes" : "no", dynamicRendering ? "yes" : "no",
          VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
          VK_VERSION_PATCH(properties.apiVersion));
}

bool SceneTargets::ChooseDepthStencilFormat(const DeviceContext &device, std::string *error)
{
  for (VkFormat candidate : kDepthStencilCandidates)
  {
    if (FormatSupportsOptimalFeatures(device, candidate, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
    {
      m_depthStencilFormat = candidate;
      // The OpenGL renderers always get GL_DEPTH32F_STENCIL8, so the 24 bit integer
      // fallback is a real precision difference.
      if (candidate != kDepthStencilCandidates[0])
      {
        InfoLog("Vulkan is using %s for depth and stencil; %s is unavailable on this device, so "
                "depth precision differs from the OpenGL renderers.",
                FormatName(candidate), FormatName(kDepthStencilCandidates[0]));
        LogDepthPrecisionOptions(device);
      }
      else
        InfoLog("Vulkan is using %s for depth and stencil.", FormatName(candidate));
      return true;
    }
  }

  std::ostringstream message;
  message << "This device supports neither " << FormatName(kDepthStencilCandidates[0]) << " nor "
          << FormatName(kDepthStencilCandidates[1])
          << " as a depth/stencil attachment. New3D needs a combined depth and stencil buffer";
  return SetError(error, message.str());
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

bool SceneTargets::CreateImages(const DeviceContext &device, std::string *error)
{
  const VkImageUsageFlags colourUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

  if (!m_colour.Create(device, kColourFormat, m_width, m_height, 1, colourUsage,
                       VK_IMAGE_ASPECT_COLOR_BIT, error))
    return false;
  if (!m_trans0.Create(device, kColourFormat, m_width, m_height, 1, colourUsage,
                       VK_IMAGE_ASPECT_COLOR_BIT, error))
    return false;
  if (!m_trans1.Create(device, kColourFormat, m_width, m_height, 1, colourUsage,
                       VK_IMAGE_ASPECT_COLOR_BIT, error))
    return false;

  // The LOS attachment only ever leaves the GPU through vkCmdCopyImageToBuffer.
  if (!m_los.Create(device, kLosFormat, m_width, m_height, 1,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, error))
    return false;

  // TRANSFER_SRC/DST on the pair below is what makes the GL depth blit equivalent possible.
  if (!m_depthStencil.Create(device, m_depthStencilFormat, m_width, m_height, 1,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             kDepthStencilAspect, error))
    return false;
  if (!m_depthStencilCopy.Create(device, m_depthStencilFormat, m_width, m_height, 1,
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 kDepthStencilAspect, error))
    return false;

  // Where the 2D layers, the New3D composite and the scroll fog end up, at the supersampled
  // display resolution rather than the smaller attachment one: 2D is laid out in display space.
  if (!m_sceneColour.Create(device, kColourFormat, m_sceneWidth, m_sceneHeight, 1, colourUsage,
                            VK_IMAGE_ASPECT_COLOR_BIT, error))
    return false;

  // The final image, at output resolution: post process, crosshair, screenshot. With an
  // identity resolve and matching sizes it holds nothing scene colour does not, so it is
  // skipped and OutputImage() hands out scene colour instead.
  m_outputAliasesSceneColour =
      m_resolveIsIdentity && m_sceneWidth == m_outputWidth && m_sceneHeight == m_outputHeight;

  if (!m_outputAliasesSceneColour &&
      !m_output.Create(device, kColourFormat, m_outputWidth, m_outputHeight, 1,
                       colourUsage | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, error))
    return false;

  return true;
}

Image *SceneTargets::ColourAttachment(uint32_t attachment)
{
  switch (attachment)
  {
  case kR3DAttachmentColour: return &m_colour;
  case kR3DAttachmentTrans0: return &m_trans0;
  case kR3DAttachmentTrans1: return &m_trans1;
  case kR3DAttachmentLos: return &m_los;
  default: return nullptr;
  }
}

VkImageView SceneTargets::ColourView() const { return m_colour.View(); }
VkImageView SceneTargets::Trans0View() const { return m_trans0.View(); }
VkImageView SceneTargets::Trans1View() const { return m_trans1.View(); }
VkImageView SceneTargets::LosView() const { return m_los.View(); }
VkImageView SceneTargets::DepthStencilView() const { return m_depthStencil.View(); }
VkImageView SceneTargets::DepthStencilCopyView() const { return m_depthStencilCopy.View(); }
VkImageView SceneTargets::SceneColourView() const { return m_sceneColour.View(); }
VkImageView SceneTargets::OutputView() const { return OutputImage().View(); }

// ---------------------------------------------------------------------------
// Render passes
// ---------------------------------------------------------------------------

bool SceneTargets::CreateRenderPasses(const DeviceContext &device, std::string *error)
{
  for (uint32_t i = 0; i < static_cast<uint32_t>(ScenePass::Count); ++i)
  {
    if (!CreateSceneRenderPass(device, static_cast<ScenePass>(i), error))
      return false;
  }

  for (uint32_t i = 0; i < static_cast<uint32_t>(SimplePass::Count); ++i)
  {
    const SimplePass pass = static_cast<SimplePass>(i);
    if (!CreateSimpleRenderPass(device, kColourFormat, pass, &m_sceneColourPasses[i], error))
      return false;
    if (!CreateSimpleRenderPass(device, kColourFormat, pass, &m_outputPasses[i], error))
      return false;
  }

  return true;
}

bool SceneTargets::CreateSceneRenderPass(const DeviceContext &device, ScenePass pass,
                                         std::string *error)
{
  // GL's three load behaviours, see the header comment.
  VkAttachmentLoadOp colourLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  VkAttachmentLoadOp losLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  VkAttachmentLoadOp depthLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  VkAttachmentStoreOp losStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkAttachmentStoreOp depthStoreOp = VK_ATTACHMENT_STORE_OP_STORE;

  bool losUnused = false;

  switch (pass)
  {
  case ScenePass::ClearAllNoLos:
    losUnused = true;
    [[fallthrough]];
  case ScenePass::ClearAll:
    colourLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    losLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    break;
  case ScenePass::BeginLayerNoLos:
    losUnused = true;
    [[fallthrough]];
  case ScenePass::BeginLayer:
    // glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT) plus PrepareLos(). ProcessLos and
    // RecordStoreDepth both run straight after this pass, so LOS and depth are stored.
    losLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    break;
  case ScenePass::TransFirst:
  case ScenePass::TransSecond:
    // Each translucency pass writes one colour attachment; the others are VK_ATTACHMENT_UNUSED
    // and never enter tile memory. LOS is masked off by MakeR3DKey and already sampled. Depth
    // results are discarded: RecordRestoreDepth overwrites the image, then BeginLayer clears it.
    losLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    losStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    break;
  case ScenePass::Count:
  default:
    return SetError(error, "SceneTargets::CreateSceneRenderPass received an invalid ScenePass");
  }

  const VkAttachmentStoreOp keep = VK_ATTACHMENT_STORE_OP_STORE;
  std::array<VkAttachmentDescription, kSceneAttachmentCount> attachments{};
  attachments[kR3DAttachmentColour] = MakeColourAttachment(kColourFormat, colourLoadOp, keep);
  attachments[kR3DAttachmentTrans0] = MakeColourAttachment(kColourFormat, colourLoadOp, keep);
  attachments[kR3DAttachmentTrans1] = MakeColourAttachment(kColourFormat, colourLoadOp, keep);
  attachments[kR3DAttachmentLos] = MakeColourAttachment(kLosFormat, losLoadOp, losStoreOp);
  attachments[kSceneAttachmentDepthStencil] =
      MakeDepthStencilAttachment(m_depthStencilFormat, depthLoadOp, depthStoreOp);

  // Unused attachments get no load and no store, and their contents are preserved. The
  // fragment shader still declares all four outputs; writes to an unused location are
  // discarded, so no shader variants are needed and the blend state is unchanged.
  const uint32_t liveColour = (pass == ScenePass::TransFirst)    ? kR3DAttachmentTrans0
                              : (pass == ScenePass::TransSecond) ? kR3DAttachmentTrans1
                                                                 : UINT32_MAX;

  std::array<VkAttachmentReference, kSceneColourAttachmentCount> colourReferences{};
  for (uint32_t i = 0; i < kSceneColourAttachmentCount; ++i)
  {
    const bool unused = (liveColour != UINT32_MAX && i != liveColour) ||
                        (losUnused && i == kR3DAttachmentLos);
    colourReferences[i].attachment = unused ? VK_ATTACHMENT_UNUSED : i;
    colourReferences[i].layout = unused ? VK_IMAGE_LAYOUT_UNDEFINED : kColourSteadyLayout;
    if (unused)
    {
      attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
  }

  VkAttachmentReference depthReference{};
  depthReference.attachment = kSceneAttachmentDepthStencil;
  depthReference.layout = kDepthSteadyLayout;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = static_cast<uint32_t>(colourReferences.size());
  subpass.pColorAttachments = colourReferences.data();
  subpass.pDepthStencilAttachment = &depthReference;

  const std::array<VkSubpassDependency, 2> dependencies = MakeExternalDependencies(true);

  VkRenderPassCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  createInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
  createInfo.pAttachments = attachments.data();
  createInfo.subpassCount = 1;
  createInfo.pSubpasses = &subpass;
  createInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
  createInfo.pDependencies = dependencies.data();

  VkRenderPass handle = VK_NULL_HANDLE;
  const VkResult result = vkCreateRenderPass(device.device, &createInfo, nullptr, &handle);
  if (result != VK_SUCCESS)
    return SetError(error, VkError("vkCreateRenderPass for the New3D scene pass", result));

  m_scenePasses[static_cast<size_t>(pass)] = handle;
  return true;
}

bool SceneTargets::CreateSimpleRenderPass(const DeviceContext &device, VkFormat format,
                                          SimplePass pass, VkRenderPass *out, std::string *error)
{
  const VkAttachmentLoadOp loadOp = (pass == SimplePass::Clear) ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                    : (pass == SimplePass::Discard)
                                        ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                                        : VK_ATTACHMENT_LOAD_OP_LOAD;

  const VkAttachmentDescription attachment =
      MakeColourAttachment(format, loadOp, VK_ATTACHMENT_STORE_OP_STORE);

  VkAttachmentReference colourReference{};
  colourReference.attachment = 0;
  colourReference.layout = kColourSteadyLayout;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colourReference;

  const std::array<VkSubpassDependency, 2> dependencies = MakeExternalDependencies(false);

  VkRenderPassCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  createInfo.attachmentCount = 1;
  createInfo.pAttachments = &attachment;
  createInfo.subpassCount = 1;
  createInfo.pSubpasses = &subpass;
  createInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
  createInfo.pDependencies = dependencies.data();

  const VkResult result = vkCreateRenderPass(device.device, &createInfo, nullptr, out);
  if (result != VK_SUCCESS)
    return SetError(error, VkError("vkCreateRenderPass for a single-attachment pass", result));
  return true;
}

bool SceneTargets::CreateFramebuffers(const DeviceContext &device, std::string *error)
{
  const std::array<VkImageView, kSceneAttachmentCount> sceneViews = {
    m_colour.View(), m_trans0.View(), m_trans1.View(), m_los.View(), m_depthStencil.View(),
  };

  VkFramebufferCreateInfo sceneInfo{};
  sceneInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  // Any of the three scene passes would do; they are render-pass compatible.
  sceneInfo.attachmentCount = static_cast<uint32_t>(sceneViews.size());
  sceneInfo.pAttachments = sceneViews.data();
  sceneInfo.width = m_width;
  sceneInfo.height = m_height;
  sceneInfo.layers = 1;

  // One framebuffer per pass variant. Sharing one is only legal across compatible render
  // passes, and these variants differ in which attachments are VK_ATTACHMENT_UNUSED.
  VkResult result = VK_SUCCESS;
  for (size_t i = 0; i < m_sceneFramebuffers.size(); ++i)
  {
    sceneInfo.renderPass = m_scenePasses[i];
    if (sceneInfo.renderPass == VK_NULL_HANDLE)
      continue;
    result = vkCreateFramebuffer(device.device, &sceneInfo, nullptr, &m_sceneFramebuffers[i]);
    if (result != VK_SUCCESS)
      return SetError(error, VkError("vkCreateFramebuffer for the New3D scene pass", result));
  }

  const VkImageView sceneColourView = m_sceneColour.View();
  VkFramebufferCreateInfo sceneColourInfo{};
  sceneColourInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  sceneColourInfo.renderPass = m_sceneColourPasses[static_cast<size_t>(SimplePass::Clear)];
  sceneColourInfo.attachmentCount = 1;
  sceneColourInfo.pAttachments = &sceneColourView;
  sceneColourInfo.width = m_sceneWidth;
  sceneColourInfo.height = m_sceneHeight;
  sceneColourInfo.layers = 1;

  result = vkCreateFramebuffer(device.device, &sceneColourInfo, nullptr, &m_sceneColourFramebuffer);
  if (result != VK_SUCCESS)
    return SetError(error, VkError("vkCreateFramebuffer for the intermediate scene colour", result));

  const VkImageView outputView = OutputImage().View();
  VkFramebufferCreateInfo outputInfo{};
  outputInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  outputInfo.renderPass = m_outputPasses[static_cast<size_t>(SimplePass::Clear)];
  outputInfo.attachmentCount = 1;
  outputInfo.pAttachments = &outputView;
  outputInfo.width = m_outputAliasesSceneColour ? m_sceneWidth : m_outputWidth;
  outputInfo.height = m_outputAliasesSceneColour ? m_sceneHeight : m_outputHeight;
  outputInfo.layers = 1;

  result = vkCreateFramebuffer(device.device, &outputInfo, nullptr, &m_outputFramebuffer);
  if (result != VK_SUCCESS)
    return SetError(error, VkError("vkCreateFramebuffer for the output image", result));

  return true;
}

VkRenderPass SceneTargets::ScenePassHandle(ScenePass pass) const
{
  const size_t index = static_cast<size_t>(pass);
  return index < m_scenePasses.size() ? m_scenePasses[index] : VK_NULL_HANDLE;
}

VkRenderPass SceneTargets::SceneColourPassHandle(SimplePass pass) const
{
  const size_t index = static_cast<size_t>(pass);
  return index < m_sceneColourPasses.size() ? m_sceneColourPasses[index] : VK_NULL_HANDLE;
}

VkRenderPass SceneTargets::OutputPassHandle(SimplePass pass) const
{
  const size_t index = static_cast<size_t>(pass);
  return index < m_outputPasses.size() ? m_outputPasses[index] : VK_NULL_HANDLE;
}

std::array<VkClearValue, SceneTargets::kSceneAttachmentCount> SceneTargets::SceneClearValues()
{
  std::array<VkClearValue, kSceneAttachmentCount> values{};

  // GL cleared the three colour attachments to transparent black.
  for (uint32_t i = kR3DAttachmentColour; i <= kR3DAttachmentTrans1; ++i)
  {
    values[i].color.float32[0] = 0.0f;
    values[i].color.float32[1] = 0.0f;
    values[i].color.float32[2] = 0.0f;
    values[i].color.float32[3] = 0.0f;
  }

  // PrepareLos()'s glClearBufferuiv with { 0, 0, 0, 0 }.
  values[kR3DAttachmentLos].color.uint32[0] = 0;
  values[kR3DAttachmentLos].color.uint32[1] = 0;
  values[kR3DAttachmentLos].color.uint32[2] = 0;
  values[kR3DAttachmentLos].color.uint32[3] = 0;

  // Reversed-Z: glClearDepth(0.0) and a zero stencil.
  values[kSceneAttachmentDepthStencil].depthStencil.depth = kReversedZClearDepth;
  values[kSceneAttachmentDepthStencil].depthStencil.stencil = 0;

  return values;
}

void SceneTargets::RecordBeginScenePass(VkCommandBuffer commandBuffer, ScenePass pass) const
{
  const std::array<VkClearValue, kSceneAttachmentCount> clearValues = SceneClearValues();

  VkRenderPassBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  beginInfo.renderPass = ScenePassHandle(pass);
  beginInfo.framebuffer = SceneFramebuffer(pass);
  beginInfo.renderArea.offset = VkOffset2D{ 0, 0 };
  beginInfo.renderArea.extent = VkExtent2D{ m_width, m_height };
  // Ignored where loadOp is LOAD, so one array serves all three variants.
  beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
  beginInfo.pClearValues = clearValues.data();

  vkCmdBeginRenderPass(commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
}

void SceneTargets::RecordBeginSceneColourPass(VkCommandBuffer commandBuffer, SimplePass pass,
                                              const VkClearColorValue &clearColour,
                                              const VkRect2D *area) const
{
  VkClearValue clearValue{};
  clearValue.color = clearColour;

  VkRenderPassBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  beginInfo.renderPass = SceneColourPassHandle(pass);
  beginInfo.framebuffer = m_sceneColourFramebuffer;
  beginInfo.renderArea.offset = VkOffset2D{ 0, 0 };
  // The scene size, so a clearing pass covers the whole composition image.
  beginInfo.renderArea.extent = VkExtent2D{ m_sceneWidth, m_sceneHeight };
  if (area != nullptr)
    beginInfo.renderArea = *area;
  beginInfo.clearValueCount = 1;
  beginInfo.pClearValues = &clearValue;

  vkCmdBeginRenderPass(commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
}

void SceneTargets::RecordBeginOutputPass(VkCommandBuffer commandBuffer, SimplePass pass,
                                         const VkClearColorValue &clearColour) const
{
  VkClearValue clearValue{};
  clearValue.color = clearColour;

  VkRenderPassBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  beginInfo.renderPass = OutputPassHandle(pass);
  beginInfo.framebuffer = m_outputFramebuffer;
  beginInfo.renderArea.offset = VkOffset2D{ 0, 0 };
  beginInfo.renderArea.extent = VkExtent2D{ m_outputWidth, m_outputHeight };
  beginInfo.clearValueCount = 1;
  beginInfo.pClearValues = &clearValue;

  vkCmdBeginRenderPass(commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
}

// ---------------------------------------------------------------------------
// Viewport helpers
// ---------------------------------------------------------------------------

VkViewport SceneTargets::MakeViewport() const
{
  return Vulkan::MakeViewport(0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height));
}

VkViewport SceneTargets::MakeViewport(float x, float y, float width, float height) const
{
  return Vulkan::MakeViewport(x, y, width, height);
}

VkViewport SceneTargets::MakeSceneViewport() const
{
  return Vulkan::MakeViewport(0.0f, 0.0f, static_cast<float>(m_sceneWidth),
                             static_cast<float>(m_sceneHeight));
}

VkViewport SceneTargets::MakeOutputViewport() const
{
  return Vulkan::MakeViewport(0.0f, 0.0f, static_cast<float>(m_outputWidth),
                             static_cast<float>(m_outputHeight));
}

VkRect2D SceneTargets::MakeScissor() const
{
  return MakeScissor(0, 0, m_width, m_height);
}

VkRect2D SceneTargets::MakeScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) const
{
  // The negative-height viewport does not affect the scissor: always framebuffer
  // coordinates with the origin at the top left.
  VkRect2D scissor{};
  scissor.offset = VkOffset2D{ x, y };
  scissor.extent = VkExtent2D{ width, height };
  return scissor;
}

VkRect2D SceneTargets::MakeSceneScissor() const
{
  return MakeScissor(0, 0, m_sceneWidth, m_sceneHeight);
}

VkRect2D SceneTargets::MakeOutputScissor() const
{
  return MakeScissor(0, 0, m_outputWidth, m_outputHeight);
}

// ---------------------------------------------------------------------------
// Layout management
// ---------------------------------------------------------------------------

void SceneTargets::RecordInitialTransitions(VkCommandBuffer commandBuffer)
{
  m_colour.RecordTransition(commandBuffer, kColourSteadyLayout);
  m_trans0.RecordTransition(commandBuffer, kColourSteadyLayout);
  m_trans1.RecordTransition(commandBuffer, kColourSteadyLayout);
  m_los.RecordTransition(commandBuffer, kColourSteadyLayout);
  m_sceneColour.RecordTransition(commandBuffer, kColourSteadyLayout);
  OutputImage().RecordTransition(commandBuffer, kColourSteadyLayout);
  m_depthStencil.RecordTransition(commandBuffer, kDepthSteadyLayout);
  m_depthStencilCopy.RecordTransition(commandBuffer, kDepthCopySteadyLayout);
}

void SceneTargets::RecordSceneAttachmentsForSampling(VkCommandBuffer commandBuffer)
{
  m_colour.RecordTransition(commandBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  m_trans0.RecordTransition(commandBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  m_trans1.RecordTransition(commandBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void SceneTargets::RecordSceneAttachmentsForRendering(VkCommandBuffer commandBuffer)
{
  m_colour.RecordTransition(commandBuffer, kColourSteadyLayout);
  m_trans0.RecordTransition(commandBuffer, kColourSteadyLayout);
  m_trans1.RecordTransition(commandBuffer, kColourSteadyLayout);
}

void SceneTargets::RecordSceneColourForSampling(VkCommandBuffer commandBuffer)
{
  m_sceneColour.RecordTransition(commandBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void SceneTargets::RecordSceneColourForRendering(VkCommandBuffer commandBuffer)
{
  m_sceneColour.RecordTransition(commandBuffer, kColourSteadyLayout);
}

// ---------------------------------------------------------------------------
// Copies
// ---------------------------------------------------------------------------

void SceneTargets::RecordDepthCopy(VkCommandBuffer commandBuffer, Image &source, Image &destination,
                                   VkImageLayout sourceSteady, VkImageLayout destinationSteady)
{
  source.RecordTransition(commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  destination.RecordTransition(commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  // A copy, not a blit: vkCmdBlitImage is not permitted for depth/stencil formats, and GL's
  // GL_NEAREST blit over identical rectangles was a straight copy in all but name.
  VkImageCopy region{};
  region.srcSubresource.aspectMask = kDepthStencilAspect;
  region.srcSubresource.mipLevel = 0;
  region.srcSubresource.baseArrayLayer = 0;
  region.srcSubresource.layerCount = 1;
  region.srcOffset = VkOffset3D{ 0, 0, 0 };
  region.dstSubresource = region.srcSubresource;
  region.dstOffset = VkOffset3D{ 0, 0, 0 };
  region.extent = VkExtent3D{ m_width, m_height, 1 };

  vkCmdCopyImage(commandBuffer, source.Handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 destination.Handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  source.RecordTransition(commandBuffer, sourceSteady);
  destination.RecordTransition(commandBuffer, destinationSteady);
}

void SceneTargets::RecordStoreDepth(VkCommandBuffer commandBuffer)
{
  RecordDepthCopy(commandBuffer, m_depthStencil, m_depthStencilCopy, kDepthSteadyLayout,
                  kDepthCopySteadyLayout);
}

void SceneTargets::RecordRestoreDepth(VkCommandBuffer commandBuffer)
{
  RecordDepthCopy(commandBuffer, m_depthStencilCopy, m_depthStencil, kDepthCopySteadyLayout,
                  kDepthSteadyLayout);
}

void SceneTargets::RecordLosCopyToBuffer(VkCommandBuffer commandBuffer, int32_t x, int32_t y,
                                        VkBuffer destination, VkDeviceSize bufferOffset)
{
  m_los.RecordTransition(commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

  VkBufferImageCopy region{};
  region.bufferOffset = bufferOffset;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageOffset = VkOffset3D{ x, y, 0 };
  region.imageExtent = VkExtent3D{ 1, 1, 1 };

  vkCmdCopyImageToBuffer(commandBuffer, m_los.Handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         destination, 1, &region);

  m_los.RecordTransition(commandBuffer, kColourSteadyLayout);
}

VkDeviceSize SceneTargets::ScreenshotBufferSize() const
{
  // kColourFormat is four bytes per texel and the copy is tightly packed.
  return static_cast<VkDeviceSize>(m_outputWidth) * static_cast<VkDeviceSize>(m_outputHeight) * 4u;
}

void SceneTargets::RecordOutputCopyToBuffer(VkCommandBuffer commandBuffer, VkBuffer destination,
                                            VkDeviceSize bufferOffset)
{
  OutputImage().RecordTransition(commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

  VkBufferImageCopy region{};
  region.bufferOffset = bufferOffset;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageOffset = VkOffset3D{ 0, 0, 0 };
  region.imageExtent = VkExtent3D{ m_outputWidth, m_outputHeight, 1 };

  vkCmdCopyImageToBuffer(commandBuffer, OutputImage().Handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         destination, 1, &region);

  OutputImage().RecordTransition(commandBuffer, kColourSteadyLayout);
}

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
