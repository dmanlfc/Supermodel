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

#include "VulkanComposite.h"

#include <array>
#include <sstream>
#include <vector>

#include "OSD/Logger.h"

namespace Vulkan
{
namespace
{

// glDrawArrays(GL_TRIANGLE_STRIP, 0, 4) in both DrawBaseLayer and DrawAlphaLayer.
constexpr uint32_t kFullScreenQuadVertexCount = 4;

std::string VkError(const char *operation, VkResult result)
{
  std::ostringstream message;
  message << operation << " failed with VkResult " << static_cast<int>(result);
  return message.str();
}

} // anonymous namespace

Composite::~Composite()
{
  // Nothing can be released without a device; the owning renderer calls Destroy().
}

bool Composite::SetError(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

void Composite::Destroy(const DeviceContext &device)
{
  if (device.device != VK_NULL_HANDLE && m_sampler != VK_NULL_HANDLE)
    vkDestroySampler(device.device, m_sampler, nullptr);
  m_sampler = VK_NULL_HANDLE;

  m_descriptors.Destroy(device);
  m_baseSet = VK_NULL_HANDLE;
  m_transSet = VK_NULL_HANDLE;
  m_baseLayout = VK_NULL_HANDLE;
  m_transLayout = VK_NULL_HANDLE;
  m_colourView = VK_NULL_HANDLE;
  m_trans0View = VK_NULL_HANDLE;
  m_trans1View = VK_NULL_HANDLE;
  m_created = false;
}

bool Composite::Prepare(Context &context, std::string *error)
{
  const DeviceContext &device = context.Device();
  SceneTargets &targets = context.Targets();
  PipelineCache &pipelines = context.Pipelines();

  const VkDescriptorSetLayout baseLayout = pipelines.SetLayout(SetLayoutId::FrameBufferBase);
  const VkDescriptorSetLayout transLayout = pipelines.SetLayout(SetLayoutId::FrameBufferTrans);
  if (baseLayout == VK_NULL_HANDLE || transLayout == VK_NULL_HANDLE)
    return SetError(error, "The Vulkan framebuffer composite descriptor set layouts are unavailable");

  if (m_sampler == VK_NULL_HANDLE)
  {
    // The composite shaders only texelFetch(), which ignores filtering, but a sampler is
    // still required. R3DFrameBuffers::CreateTexture asked for GL_NEAREST/GL_CLAMP_TO_EDGE.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
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
      return SetError(error, VkError("vkCreateSampler(framebuffer composite)", result));
    }
  }

  const VkImageView colourView = targets.ColourView();
  const VkImageView trans0View = targets.Trans0View();
  const VkImageView trans1View = targets.Trans1View();
  if (colourView == VK_NULL_HANDLE || trans0View == VK_NULL_HANDLE || trans1View == VK_NULL_HANDLE)
    return SetError(error, "The Vulkan New3D scene colour attachments are unavailable");

  const bool upToDate = m_created && m_baseLayout == baseLayout && m_transLayout == transLayout &&
                        m_colourView == colourView && m_trans0View == trans0View &&
                        m_trans1View == trans1View;
  if (upToDate)
    return true;

  // First frame, or Context::EnsureSceneTargets() rebuilt the images and the pipeline cache.
  // That call idles the device, so no submitted work can still be reading these sets.
  m_descriptors.Destroy(device);
  m_baseSet = VK_NULL_HANDLE;
  m_transSet = VK_NULL_HANDLE;
  m_created = false;

  const std::vector<VkDescriptorPoolSize> sizes = {
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 },
  };
  if (!m_descriptors.Create(device, sizes, 2, error))
    return false;
  if (!m_descriptors.Allocate(baseLayout, &m_baseSet, error))
    return false;
  if (!m_descriptors.Allocate(transLayout, &m_transSet, error))
    return false;

  // The attachments are sampled in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  // which RecordInternal() transitions them into before the draws.
  std::array<VkDescriptorImageInfo, 3> images{};
  images[0].sampler = m_sampler;
  images[0].imageView = colourView;
  images[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  images[1].sampler = m_sampler;
  images[1].imageView = trans0View;
  images[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  images[2].sampler = m_sampler;
  images[2].imageView = trans1View;
  images[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  std::array<VkWriteDescriptorSet, 3> writes{};
  // Base: tex1 = the opaque colour attachment, GL texture unit 0.
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = m_baseSet;
  writes[0].dstBinding = kFrameBufferBindingTex1;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[0].pImageInfo = &images[0];
  // Trans: tex1 = layer 1 (unit 1), tex2 = layer 2 (unit 2).
  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = m_transSet;
  writes[1].dstBinding = kFrameBufferBindingTex1;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[1].pImageInfo = &images[1];
  writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[2].dstSet = m_transSet;
  writes[2].dstBinding = kFrameBufferBindingTex2;
  writes[2].descriptorCount = 1;
  writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[2].pImageInfo = &images[2];

  vkUpdateDescriptorSets(device.device, static_cast<uint32_t>(writes.size()), writes.data(), 0,
                         nullptr);

  m_baseLayout = baseLayout;
  m_transLayout = transLayout;
  m_colourView = colourView;
  m_trans0View = trans0View;
  m_trans1View = trans1View;
  m_created = true;
  return true;
}

bool Composite::Record(Context &context, const VkRect2D &scissor, std::string *error)
{
  if (!context.SceneTargetsValid())
    return SetError(error, "The Vulkan framebuffer composite ran before the render targets existed");

  // glViewport(0, 0, m_width, m_height): the entire destination. Only reached with RenderScale
  // off, where the attachments and sceneColour are the same size, so this is Draw()'s viewport.
  const VkViewport viewport = context.Targets().MakeSceneViewport();
  return RecordInternal(context, false, viewport, scissor, error);
}

bool Composite::RecordScaled(Context &context, int32_t x, int32_t y, uint32_t width,
                             uint32_t height, const VkRect2D &scissor, std::string *error)
{
  if (!context.SceneTargetsValid())
    return SetError(error, "The Vulkan framebuffer composite ran before the render targets existed");

  const SceneTargets &targets = context.Targets();
  // The rectangle is measured in the destination, which is the sceneColour image,
  // so the flip has to use the scene height and not the attachment height.
  const int32_t targetHeight = static_cast<int32_t>(targets.SceneHeight());

  // CNew3D passed the output rectangle in GL framebuffer coordinates, origin at the lower
  // left. MakeViewport() takes upper left and restores GL clip space with a negative height.
  const float top = static_cast<float>(targetHeight - (y + static_cast<int32_t>(height)));
  const VkViewport viewport = targets.MakeViewport(static_cast<float>(x), top,
                                                   static_cast<float>(width),
                                                   static_cast<float>(height));
  return RecordInternal(context, true, viewport, scissor, error);
}

bool Composite::RecordInternal(Context &context, bool scaled, const VkViewport &viewport,
                               const VkRect2D &scissor, std::string *error)
{
  if (!context.FrameActive())
    return SetError(error, "The Vulkan framebuffer composite ran outside an active frame");
  if (!Prepare(context, error))
    return false;

  VkCommandBuffer commandBuffer = context.CommandBuffer();
  SceneTargets &targets = context.Targets();
  PipelineCache &pipelines = context.Pipelines();
  const TargetInfo &targetInfo = context.PipelineTargets();

  const Program baseProgram = scaled ? Program::FrameBufferBaseScaled : Program::FrameBufferBase;
  const Program transProgram = scaled ? Program::FrameBufferTransScaled : Program::FrameBufferTrans;

  VkPipeline basePipeline =
      pipelines.Get(context.Device(), PipelineCache::MakeFrameBufferKey(targetInfo, baseProgram),
                    error);
  if (basePipeline == VK_NULL_HANDLE)
    return false;
  VkPipeline transPipeline =
      pipelines.Get(context.Device(), PipelineCache::MakeFrameBufferKey(targetInfo, transProgram),
                    error);
  if (transPipeline == VK_NULL_HANDLE)
    return false;

  const VkPipelineLayout baseLayout = pipelines.LayoutFor(baseProgram);
  const VkPipelineLayout transLayout = pipelines.LayoutFor(transProgram);

  // The three attachments were just rendered to; make them sampleable.
  targets.RecordSceneAttachmentsForSampling(commandBuffer);

  // VK_ATTACHMENT_LOAD_OP_LOAD: the 2D bottom layer is already here and the composite blends
  // over it, as it did over the GL back buffer or the SuperAA target.
  const VkClearColorValue unused{};
  targets.RecordBeginSceneColourPass(commandBuffer, SceneTargets::SimplePass::Load, unused);

  vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
  vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

  // glUniform2i(sourceSize, m_width, m_height): the size of the images being
  // sampled, so this stays the attachment size in both variants.
  R3DFrameBufferPushConstants push{};
  push.sourceSize[0] = static_cast<int32_t>(targets.Width());
  push.sourceSize[1] = static_cast<int32_t>(targets.Height());

  // DrawBaseLayer().
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, basePipeline);
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, baseLayout,
                          kFrameBufferDescriptorSet, 1, &m_baseSet, 0, nullptr);
  if (scaled)
    vkCmdPushConstants(commandBuffer, baseLayout, kR3DFrameBufferScaledPushConstantRange.stageFlags,
                       0, static_cast<uint32_t>(sizeof(push)), &push);
  vkCmdDraw(commandBuffer, kFullScreenQuadVertexCount, 1, 0, 0);

  // DrawAlphaLayer().
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, transPipeline);
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, transLayout,
                          kFrameBufferDescriptorSet, 1, &m_transSet, 0, nullptr);
  if (scaled)
    vkCmdPushConstants(commandBuffer, transLayout, kR3DFrameBufferScaledPushConstantRange.stageFlags,
                       0, static_cast<uint32_t>(sizeof(push)), &push);
  vkCmdDraw(commandBuffer, kFullScreenQuadVertexCount, 1, 0, 0);

  vkCmdEndRenderPass(commandBuffer);

  // Back to the steady-state layout so the next frame's scene pass can render into them.
  targets.RecordSceneAttachmentsForRendering(commandBuffer);
  return true;
}

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
