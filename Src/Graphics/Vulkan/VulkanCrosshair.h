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
 * VulkanCrosshair.h
 *
 * CCrosshair equivalent (Src/OSD/SDL/Crosshair.cpp): the light gun crosshairs,
 * drawn into SceneTargets::OutputImage() after post processing, as GL drew them
 * after SuperAA::Draw. Screenshots therefore include them.
 *
 * The vertex data never changes after BuildCrosshairVertices(), so the two buffers
 * are device-local and uploaded once at Init().
 */

#ifndef INCLUDED_VULKAN_CROSSHAIR_H
#define INCLUDED_VULKAN_CROSSHAIR_H

#ifdef SUPERMODEL_VULKAN

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "Graphics/New3D/Mat4.h"
#include "Graphics/Vulkan/VulkanContext.h"
#include "Graphics/Vulkan/VulkanPipelines.h"
#include "Graphics/Vulkan/VulkanResources.h"
#include "Supermodel.h"
#include "Util/NewConfig.h"

class CInputs;

namespace Vulkan
{

class Crosshair
{
public:
  Crosshair(Context &context, const Util::Config::Node &config);
  ~Crosshair();

  Crosshair(const Crosshair &) = delete;
  Crosshair &operator=(const Crosshair &) = delete;

  Result Init();

  // CCrosshair::Update equivalent: the gun inputs to read and the display
  // rectangle to draw into.
  void Update(uint32_t currentInputs, CInputs *inputs, unsigned xOffset, unsigned yOffset,
              unsigned xRes, unsigned yRes);

private:
  static constexpr uint32_t kMaxVerts = 1024;
  static constexpr uint32_t kPlayers = 2;

  struct BasicVertex
  {
    BasicVertex(float x, float y, float z) : x(x), y(y), z(z) {}
    BasicVertex(float x, float y) : x(x), y(y), z(0.0f) {}
    float x, y, z;
  };

  struct UVCoords
  {
    UVCoords(float x, float y) : x(x), y(y) {}
    float x, y;
  };

  void Destroy();
  void BuildCrosshairVertices();
  static void GunToViewCoords(float *x, float *y);

  bool LoadTextures(std::string *error);
  bool CreateBuffers(std::string *error);
  bool CreateDescriptors(std::string *error);
  bool RefreshDescriptors(std::string *error);

  void DrawCrosshair(New3D::Mat4 matrix, float x, float y, uint32_t player, unsigned xRes,
                     unsigned yRes);

  static bool SetError(std::string *error, const std::string &message);

  const Util::Config::Node &m_config;
  Context &m_context;

  bool m_initialized = false;
  bool m_isBitmapCrosshair = false;
  bool m_errorReported = false;

  const float m_base = 0.01f;
  const float m_height = 0.02f;
  const float m_dist = 0.004f;
  const float m_squareSize = 1.0f;
  const float m_standardDpi = 96.0f;
  const float m_scaleBitmap = 0.1f;
  float m_diagDpi = 0.0f;
  float m_hDpi = 0.0f;
  float m_vDpi = 0.0f;
  float m_dpiMultiplicator = 0.0f;

  std::vector<BasicVertex> m_verts;
  std::vector<UVCoords> m_uvCoord;

  VkSampler m_sampler = VK_NULL_HANDLE;
  std::array<Image, kPlayers> m_textures;
  Buffer m_vertexBuffer;
  Buffer m_uvBuffer;
  DescriptorAllocator m_descriptors;
  std::array<VkDescriptorSet, kPlayers> m_sets{};
  VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
};

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
#endif // INCLUDED_VULKAN_CROSSHAIR_H
