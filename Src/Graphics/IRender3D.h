#ifndef INCLUDED_IRENDER3D_H
#define INCLUDED_IRENDER3D_H

#include <cstdint>
#include "Types.h"

/*
 * IRender3D:
 *
 * Interface (abstract base class) for Real3D rendering engine.
 */
class IRender3D
{
public:
  /*
   * PrepareFrame(void):
   *
   * Performs the CPU-only part of building a frame: walking the Real3D scene
   * database into the renderer's own structures. Called before the backend
   * acquires a frame, so an implementation must not touch the graphics API here.
   *
   * The scene data read is the snapshot taken at the end of the previous frame, so
   * this does not depend on the GPU having finished. Calling it is optional: a
   * renderer must produce the same frame whether or not it ran, which lets the
   * single-threaded path and skipped frames fall back to doing the work inside
   * RenderFrame().
   */
  virtual void PrepareFrame(void)
  {
  }

  virtual void RenderFrame(void) = 0;
  virtual void BeginFrame(void) = 0;
  virtual void EndFrame(void) = 0;
  virtual void UploadTextures(unsigned level, unsigned x, unsigned y, unsigned width, unsigned height) = 0;
  virtual void AttachMemory(const uint32_t *cullingRAMLoPtr, const uint32_t *cullingRAMHiPtr, const uint32_t *polyRAMPtr, const uint32_t *vromPtr, const uint16_t *textureRAMPtr) = 0;
  virtual void SetStepping(int stepping) = 0;
  virtual Result Init(unsigned xOffset, unsigned yOffset, unsigned xRes, unsigned yRes, unsigned totalXRes, unsigned totalYRes, unsigned aaTarget) = 0;
  virtual void SetSunClamp(bool enable) = 0;
  virtual void SetBlockCulling(bool enable) = 0;
  virtual float GetLosValue(int layer) = 0;

  virtual ~IRender3D()
  {
  }
};

#endif  // INCLUDED_IRENDER3D_H
