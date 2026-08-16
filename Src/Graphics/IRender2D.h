#ifndef INCLUDED_IRENDER2D_H
#define INCLUDED_IRENDER2D_H

#include <cstdint>
#include <memory>

struct TileGenBuffer;

// Backend-neutral interface used by the Model 3 tile generator. Backend-specific
// initialization remains on each implementation because OpenGL and Vulkan own
// fundamentally different output resources.
class IRender2D
{
public:
  virtual ~IRender2D() = default;

  virtual void BeginFrame() = 0;
  virtual void PreRenderFrame() = 0;
  virtual void RenderFrameBottom() = 0;
  virtual void RenderFrameTop() = 0;
  virtual void EndFrame() = 0;

  virtual void WriteVRAM(unsigned addr, uint32_t data) = 0;
  virtual void AttachRegisters(const uint32_t *regPtr) = 0;
  virtual void AttachPalette(const uint32_t *palPtr[2]) = 0;
  virtual void AttachVRAM(const uint8_t *vramPtr) = 0;
  virtual void AttachDrawBuffers(std::shared_ptr<TileGenBuffer> bottom,
                                 std::shared_ptr<TileGenBuffer> top) = 0;
};

#endif // INCLUDED_IRENDER2D_H
