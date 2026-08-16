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
 * VulkanRenderers.h
 *
 * Umbrella header for the Vulkan renderer bundle, plus the screenshot recorder.
 *
 * Unlike GL's synchronous glReadPixels, screenshots are deferred: the readback is
 * recorded in a later frame and the BMP written from a frame completion callback.
 */

#ifndef INCLUDED_VULKAN_RENDERERS_H
#define INCLUDED_VULKAN_RENDERERS_H

#ifdef SUPERMODEL_VULKAN

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "Graphics/Vulkan/VulkanContext.h"
#include "Graphics/Vulkan/VulkanCrosshair.h"
#include "Graphics/Vulkan/VulkanNew3D.h"
#include "Graphics/Vulkan/VulkanPostProcess.h"
#include "Graphics/Vulkan/VulkanRender2D.h"

namespace Vulkan
{

class ScreenshotRecorder
{
public:
  explicit ScreenshotRecorder(Context &context);
  ~ScreenshotRecorder();

  ScreenshotRecorder(const ScreenshotRecorder &) = delete;
  ScreenshotRecorder &operator=(const ScreenshotRecorder &) = delete;

  // Safe to call outside a frame.
  void Request(const std::string &file);

  // Call once per frame, after every stage that writes the output image.
  void RecordPending();

  // Idles the device and writes any outstanding readback. Called on shutdown.
  void Drain();

private:
  struct Slot
  {
    std::string file;
    uint32_t width = 0;
    uint32_t height = 0;
    bool pending = false;
    // Context::ScreenshotReadbackGeneration() at record time. If it has moved by
    // completion the buffer was freed or resized, and reading it would over-read.
    uint64_t generation = 0;
  };

  static void Completion(void *user, uint64_t token);
  void Write(uint32_t frameIndex);

  Context &m_context;
  std::array<Slot, Context::kFramesInFlight> m_slots;
  std::vector<std::string> m_requests;
  bool m_errorReported = false;
};

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
#endif // INCLUDED_VULKAN_RENDERERS_H
