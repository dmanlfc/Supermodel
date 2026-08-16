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

#include "VulkanRenderers.h"

#include <utility>

#include "OSD/Logger.h"
#include "Util/BMPFile.h"

namespace Vulkan
{

ScreenshotRecorder::ScreenshotRecorder(Context &context)
  : m_context(context)
{
}

ScreenshotRecorder::~ScreenshotRecorder()
{
  Drain();
}

void ScreenshotRecorder::Request(const std::string &file)
{
  m_requests.push_back(file);
}

void ScreenshotRecorder::Completion(void *user, uint64_t token)
{
  ScreenshotRecorder *self = static_cast<ScreenshotRecorder *>(user);
  if (self == nullptr)
    return;
  self->Write(static_cast<uint32_t>(token));
}

void ScreenshotRecorder::Write(uint32_t frameIndex)
{
  if (frameIndex >= Context::kFramesInFlight)
    return;

  Slot &slot = m_slots[frameIndex];
  if (!slot.pending)
    return;
  slot.pending = false;

  // A resolution change since this readback was recorded freed the buffer and invalidated
  // the dimensions it was sized for; reading now would over-read the replacement allocation.
  if (slot.generation != m_context.ScreenshotReadbackGeneration())
  {
    ErrorLog("Skipped the Vulkan screenshot '%s': the output resolution changed before it could be saved.",
             slot.file.c_str());
    return;
  }

  std::string error;
  if (!m_context.InvalidateScreenshotReadback(frameIndex, &error))
  {
    ErrorLog("Unable to read back the Vulkan screenshot: %s", error.c_str());
    return;
  }

  const void *pixels = m_context.ScreenshotReadbackPointer(frameIndex);
  if (pixels == nullptr)
  {
    ErrorLog("The Vulkan screenshot readback buffer is unavailable.");
    return;
  }

  // Row 0 is the top here, whereas glReadPixels returned the bottom row first, so
  // SaveFrameBuffer's vertical flip is not requested.
  if (Util::WriteSurfaceToBMP<Util::RGBA8>(slot.file, static_cast<const uint8_t *>(pixels),
                                           static_cast<int32_t>(slot.width),
                                           static_cast<int32_t>(slot.height), false))
    ErrorLog("Unable to write the screenshot to '%s'.", slot.file.c_str());
}

void ScreenshotRecorder::RecordPending()
{
  if (m_requests.empty())
    return;
  if (!m_context.FrameActive() || !m_context.SceneTargetsValid())
    return;

  const uint32_t frameIndex = m_context.FrameIndex();
  Slot &slot = m_slots[frameIndex];
  if (slot.pending)
    return;   // this frame index still owes a write; try again next frame

  std::string error;
  if (!m_context.EnsureScreenshotReadback(&error))
  {
    if (!m_errorReported)
    {
      ErrorLog("Unable to allocate the Vulkan screenshot readback buffer: %s", error.c_str());
      m_errorReported = true;
    }
    m_requests.clear();
    return;
  }

  SceneTargets &targets = m_context.Targets();
  slot.file = std::move(m_requests.front());
  slot.width = targets.OutputWidth();
  slot.height = targets.OutputHeight();
  slot.pending = true;
  slot.generation = m_context.ScreenshotReadbackGeneration();
  m_requests.erase(m_requests.begin());

  // Recorded after post processing and the crosshairs, where the OpenGL path read the back
  // buffer, so the image matches what the user saw.
  targets.RecordOutputCopyToBuffer(m_context.CommandBuffer(),
                                   m_context.ScreenshotReadbackBuffer(),
                                   m_context.ScreenshotReadbackOffset(frameIndex));

  if (!m_context.AddFrameCompletion(&Completion, this, frameIndex, &error))
  {
    // The copy is recorded either way. Without a completion the slot stays pending so
    // Drain() writes it at shutdown.
    ErrorLog("Unable to schedule the Vulkan screenshot readback: %s", error.c_str());
  }
}

void ScreenshotRecorder::Drain()
{
  if (m_context.Handle() != VK_NULL_HANDLE)
  {
    bool anyPending = false;
    for (const Slot &slot : m_slots)
      anyPending = anyPending || slot.pending;

    if (anyPending)
    {
      // The recorded copies may not have completed, and their frame completions only run
      // from Context::BeginFrame(), which will not happen again.
      const VkResult idle = vkDeviceWaitIdle(m_context.Handle());
      if (idle != VK_SUCCESS)
        ErrorLog("Unable to wait for the device before writing a pending screenshot: VkResult %d.",
                 static_cast<int>(idle));
      else
      {
        for (uint32_t frameIndex = 0; frameIndex < Context::kFramesInFlight; ++frameIndex)
          Write(frameIndex);
      }
    }
    m_context.CancelFrameCompletions(this);
  }

  for (Slot &slot : m_slots)
  {
    slot.pending = false;
    slot.file.clear();
  }

  if (!m_requests.empty())
  {
    InfoLog("%zu screenshot request(s) were dropped because Supermodel is shutting down.",
            m_requests.size());
    m_requests.clear();
  }
}

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
