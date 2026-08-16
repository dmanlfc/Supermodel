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

#include "VulkanPostProcess.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>
#include <vector>

#include "OSD/Logger.h"

namespace Vulkan
{
namespace
{

// glDrawArrays(GL_TRIANGLE_STRIP, 0, 4) in SuperAA::Draw.
constexpr uint32_t kFullScreenQuadVertexCount = 4;

std::string VkError(const char *operation, VkResult result)
{
  std::ostringstream message;
  message << operation << " failed with VkResult " << static_cast<int>(result);
  return message.str();
}

} // anonymous namespace

PostProcess::PostProcess(Context &context, int supersampling, CRTcolor crtColors)
  : m_context(context), m_aa(std::max(1, supersampling)), m_crtColors(crtColors)
{
  // Declared here because this runs before the render targets exist, and they cannot work it
  // out alone: matching sizes rule out supersampling but say nothing about a colour transform.
  m_context.Targets().SetResolveIsIdentity(m_aa == 1 && m_crtColors == CRTcolor::None);
}

PostProcess::~PostProcess()
{
  const DeviceContext &device = m_context.Device();
  if (device.device == VK_NULL_HANDLE)
    return;

  // The descriptor set and sampler may still be referenced by frames in flight.
  const VkResult idle = vkDeviceWaitIdle(device.device);
  if (idle != VK_SUCCESS)
    ErrorLog("%s", VkError("vkDeviceWaitIdle(post process shutdown)", idle).c_str());

  m_descriptors.Destroy(device);
  m_set = VK_NULL_HANDLE;
  if (m_sampler != VK_NULL_HANDLE)
  {
    vkDestroySampler(device.device, m_sampler, nullptr);
    m_sampler = VK_NULL_HANDLE;
  }
}

bool PostProcess::SetError(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

int PostProcess::EffectiveSampleCount() const
{
  const SceneTargets &targets = m_context.Targets();
  if (!targets.IsValid() || targets.OutputWidth() == 0 || targets.OutputHeight() == 0)
    return 1;

  // The shader reads aa x aa source texels per destination pixel, so the source must be at
  // least aa times as large in both axes. Clamping to the real ratio keeps fetches in range.
  const int horizontal = static_cast<int>(targets.SceneWidth() / targets.OutputWidth());
  const int vertical = static_cast<int>(targets.SceneHeight() / targets.OutputHeight());
  const int ratio = std::max(1, std::min(horizontal, vertical));
  return std::max(1, std::min(m_aa, ratio));
}

Result PostProcess::Init(unsigned outputWidth, unsigned outputHeight)
{
  if (outputWidth == 0 || outputHeight == 0)
  {
    ErrorLog("Vulkan post processing was initialized with an empty output resolution.");
    return Result::FAIL;
  }

  // The 3D renderer has already set the attachment and scene sizes; the zeroes leave those
  // alone. Only the output resolution is this class's, and it is the unsupersampled one.
  std::string error;
  if (!m_context.EnsureSceneTargets(0, 0, 0, 0, outputWidth, outputHeight, &error))
  {
    ErrorLog("Unable to size the Vulkan output image: %s", error.c_str());
    return Result::FAIL;
  }

  // EnsureSceneTargets() rebuilds the pipeline cache whenever the render passes
  // change, so warm it again here rather than compiling inside the draw loop.
  if (!m_context.Pipelines().Prebuild(m_context.Device(), m_context.PipelineTargets(), &error))
  {
    ErrorLog("Unable to build the Vulkan post processing pipelines: %s", error.c_str());
    return Result::FAIL;
  }

  if (!Prepare(&error))
  {
    ErrorLog("Unable to create the Vulkan post processing descriptors: %s", error.c_str());
    return Result::FAIL;
  }

  m_initialized = true;
  return Result::OKAY;
}

bool PostProcess::Prepare(std::string *error)
{
  const DeviceContext &device = m_context.Device();
  if (!m_context.SceneTargetsValid())
    return SetError(error, "Vulkan post processing ran before the render targets existed");

  const VkDescriptorSetLayout layout = m_context.Pipelines().SetLayout(SetLayoutId::PostProcess);
  if (layout == VK_NULL_HANDLE)
    return SetError(error, "The Vulkan post processing descriptor set layout is unavailable");

  if (m_sampler == VK_NULL_HANDLE)
  {
    // The resolve only texelFetches, which ignores filtering, but the descriptor still needs
    // a sampler. GL's FBO texture used GL_LINEAR with GL_CLAMP_TO_EDGE.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    const VkResult result = vkCreateSampler(device.device, &samplerInfo, nullptr, &m_sampler);
    if (result != VK_SUCCESS)
    {
      m_sampler = VK_NULL_HANDLE;
      return SetError(error, VkError("vkCreateSampler(post process)", result));
    }
  }

  const VkImageView sceneColourView = m_context.Targets().SceneColourView();
  if (sceneColourView == VK_NULL_HANDLE)
    return SetError(error, "The Vulkan scene colour image is unavailable");

  if (m_set != VK_NULL_HANDLE && m_setLayout == layout && m_sceneColourView == sceneColourView)
    return true;

  // First use, or the targets were rebuilt. Either way EnsureSceneTargets() idled the device.
  m_descriptors.Destroy(device);
  m_set = VK_NULL_HANDLE;

  const std::vector<VkDescriptorPoolSize> sizes = {
    { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 },
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
  };
  if (!m_descriptors.Create(device, sizes, 1, error))
    return false;
  if (!m_descriptors.Allocate(layout, &m_set, error))
    return false;

  // The block is suballocated out of the shared uniform ring every frame and
  // selected with a dynamic offset, so the descriptor points at offset 0.
  VkDescriptorBufferInfo bufferInfo{};
  bufferInfo.buffer = m_context.UniformRing().Handle();
  bufferInfo.offset = 0;
  bufferInfo.range = sizeof(PostProcessUniforms);

  VkDescriptorImageInfo imageInfo{};
  imageInfo.sampler = m_sampler;
  imageInfo.imageView = sceneColourView;
  imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  std::array<VkWriteDescriptorSet, 2> writes{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = m_set;
  writes[0].dstBinding = kPostProcessBindingUniforms;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
  writes[0].pBufferInfo = &bufferInfo;
  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = m_set;
  writes[1].dstBinding = kPostProcessBindingTex1;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[1].pImageInfo = &imageInfo;

  vkUpdateDescriptorSets(device.device, static_cast<uint32_t>(writes.size()), writes.data(), 0,
                         nullptr);

  m_setLayout = layout;
  m_sceneColourView = sceneColourView;
  return true;
}

// True when the resolve shader would do nothing but copy scene colour to the output
// image texel for texel: no supersampling to average and no colour transform.
bool PostProcess::IsIdentityResolve() const
{
  if (m_aa != 1 || m_crtColors != CRTcolor::None)
    return false;
  const SceneTargets &targets = m_context.Targets();
  return targets.SceneWidth() == targets.OutputWidth() &&
         targets.SceneHeight() == targets.OutputHeight() &&
         targets.SceneColourFormat() == targets.OutputFormat();
}

void PostProcess::Draw()
{
  if (!m_initialized || !m_context.FrameActive() || !m_context.SceneTargetsValid())
    return;

  SceneTargets &targets = m_context.Targets();
  VkCommandBuffer commandBuffer = m_context.CommandBuffer();

  if (targets.OutputAliasesSceneColour())
  {
    // Scene colour is the output image, so the 2D and 3D stages already resolved into it.
    m_context.MarkOutputImageWritten();
    return;
  }

  if (IsIdentityResolve())
  {
    // A transfer instead of rasterising a full screen quad. The alpha the shader forces to
    // 1.0 is unobservable: the swapchain uses opaque composite alpha and Util::RGBA8 hands
    // the BMP writer only red, green and blue.
    targets.SceneColourImage().RecordTransition(commandBuffer,
                                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    targets.OutputImage().RecordTransition(commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageCopy region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.mipLevel = 0;
    region.srcSubresource.baseArrayLayer = 0;
    region.srcSubresource.layerCount = 1;
    region.srcOffset = VkOffset3D{ 0, 0, 0 };
    region.dstSubresource = region.srcSubresource;
    region.dstOffset = VkOffset3D{ 0, 0, 0 };
    region.extent = VkExtent3D{ targets.OutputWidth(), targets.OutputHeight(), 1 };
    vkCmdCopyImage(commandBuffer, targets.SceneColourImage().Handle(),
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, targets.OutputImage().Handle(),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Back to the layouts the rest of the frame expects: the crosshair draws over the
    // output image with a Load pass, and the next frame renders into scene colour.
    targets.OutputImage().RecordTransition(commandBuffer,
                                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    targets.RecordSceneColourForRendering(commandBuffer);

    m_context.MarkOutputImageWritten();
    return;
  }

  std::string error;
  if (!Prepare(&error))
  {
    if (!m_errorReported)
    {
      ErrorLog("Vulkan post processing is unavailable: %s", error.c_str());
      m_errorReported = true;
    }
    return;
  }

  PipelineCache &pipelines = m_context.Pipelines();

  VkPipeline pipeline = pipelines.Get(
      m_context.Device(), PipelineCache::MakePostProcessKey(m_context.PipelineTargets()), &error);
  if (pipeline == VK_NULL_HANDLE)
  {
    if (!m_errorReported)
    {
      ErrorLog("Unable to obtain the Vulkan post processing pipeline: %s", error.c_str());
      m_errorReported = true;
    }
    return;
  }

  // SuperAA baked these into the shader source; here they are a 16-byte uniform block.
  PostProcessUniforms uniforms{};
  uniforms.aa = EffectiveSampleCount();
  uniforms.crtColorMode = static_cast<int32_t>(m_crtColors);

  void *mapped = nullptr;
  VkDeviceSize offset = 0;
  if (!m_context.UniformRing().Suballocate(sizeof(PostProcessUniforms),
                                           m_context.UniformOffsetAlignment(), &mapped, &offset))
  {
    if (!m_errorReported)
    {
      ErrorLog("The Vulkan uniform ring buffer ran out of space for the post processing state.");
      m_errorReported = true;
    }
    return;
  }
  std::memcpy(mapped, &uniforms, sizeof(uniforms));

  // The scene colour image has just been rendered into by the 2D and 3D stages.
  targets.RecordSceneColourForSampling(commandBuffer);

  const VkClearColorValue unused{};
  // Discard, not Load: the draw covers every pixel of the render area, so nothing needs
  // reading back in and a tiled GPU need not fetch every tile purely to overwrite it. GL
  // never cleared the back buffer here either.
  targets.RecordBeginOutputPass(commandBuffer, SceneTargets::SimplePass::Discard, unused);

  // SuperAA::Draw's glViewport(0, 0, m_width, m_height) with depth, stencil, scissor and
  // blending disabled; the first three are in the pipeline key, the scissor covers the image.
  const VkViewport viewport = targets.MakeOutputViewport();
  vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
  const VkRect2D scissor = targets.MakeOutputScissor();
  vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  const uint32_t dynamicOffset = static_cast<uint32_t>(offset);
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipelines.LayoutFor(Program::PostProcess), kPostProcessDescriptorSet, 1,
                          &m_set, 1, &dynamicOffset);
  vkCmdDraw(commandBuffer, kFullScreenQuadVertexCount, 1, 0, 0);

  vkCmdEndRenderPass(commandBuffer);

  // Back to its steady-state layout for the next frame's 2D and 3D passes.
  targets.RecordSceneColourForRendering(commandBuffer);

  // The output image now holds this frame; Context::EndFrame() presents it.
  m_context.MarkOutputImageWritten();
}

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
