/**
** Supermodel
** A Sega Model 3 Arcade Emulator.
** Copyright 2011 Bart Trzynadlowski, Nik Henson
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
* New3D.h
*
* Header file defining the CNew3D class: OpenGL Real3D graphics engine.
*/

#ifndef INCLUDED_NEW3D_H
#define INCLUDED_NEW3D_H

#include <unordered_map>
#include <atomic>
#include "Graphics/GL.h"
#include "Types.h"
#include "Graphics/IRender3D.h"
#include "New3DBase.h"
#include "Model.h"
#include "Mat4.h"
#include "R3DShader.h"
#include "VBO.h"
#include "R3DData.h"
#include "Plane.h"
#include "Vec.h"
#include "R3DScrollFog.h"
#include "PolyHeader.h"
#include "R3DFrameBuffers.h"
#include <mutex>
#include "TextureBank.h"

namespace New3D {

class CNew3D final : public CNew3DBase
{
public:
	/*
	* RenderFrame(void):
	*
	* Renders the complete scene database. Must be called between BeginFrame() and
	* EndFrame(). This function traverses the scene database and builds up display
	* lists.
	*/
	void RenderFrame(void);

	/*
	* PrepareFrame(void):
	*
	* Builds the scene structure from the Real3D scene database. Pure CPU, so it may
	* run before the frame is acquired. Optional; RenderFrame() does it itself if
	* this was not called.
	*/
	void PrepareFrame(void);

	/*
	* BeginFrame(void):
	*
	* Prepare to render a new frame. Must be called once per frame prior to
	* drawing anything.
	*/
	void BeginFrame(void);

	/*
	* EndFrame(void):
	*
	* Signals the end of rendering for this frame. Must be called last during
	* the frame.
	*/
	void EndFrame(void);

	/*
	* UploadTextures(x, y, width, height):
	*
	* Signals that a portion of texture RAM has been updated.
	*
	* Parameters:
	*		x		X position within texture RAM.
	*		y		Y position within texture RAM.
	*		width	Width of texture data in texels.
	*		height	Height.
	*/
	void UploadTextures(unsigned level, unsigned x, unsigned y, unsigned width, unsigned height);

	/*
	* AttachMemory(cullingRAMLoPtr, cullingRAMHiPtr, polyRAMPtr, vromPtr,
	* 				textureRAMPtr):
	*
	* Attaches RAM and ROM areas. This must be done prior to any rendering
	* otherwise the program may crash with an access violation.
	*
	* Parameters:
	*		cullingRAMLoPtr		Pointer to low culling RAM (4 MB).
	*		cullingRAMHiPtr		Pointer to high culling RAM (1 MB).
	*		polyRAMPtr			Pointer to polygon RAM (4 MB).
	*		vromPtr				Pointer to video ROM (64 MB).
	*		textureRAMPtr		Pointer to texture RAM (8 MB).
	*/
	void AttachMemory(const UINT32 *cullingRAMLoPtr,
		const UINT32 *cullingRAMHiPtr, const UINT32 *polyRAMPtr,
		const UINT32 *vromPtr, const UINT16 *textureRAMPtr);

	/*
	* Init(xOffset, yOffset, xRes, yRes, totalXRes, totalYRes):
	*
	* One-time initialization of the context. Must be called before any other
	* members (meaning it should be called even before being attached to any
	* other objects that want to use it).
	*
	* External shader files are loaded according to configuration settings.
	*
	* Parameters:
	*		xOffset		X offset of the viewable area within OpenGL display
	*                  surface, in pixels.
	*		yOffset		Y offset.
	*		xRes		Horizontal resolution of the viewable area.
	*		yRes		Vertical resolution.
	*		totalXRes	Horizontal resolution of the complete display area.
	*		totalYRes	Vertical resolution.
	*
	* Returns:
	*		OKAY if successful, otherwise FAILED if a non-recoverable error
	*		occurred. Any allocated memory will not be freed until the
	*		destructor is called. Prints own error messages.
	*/
	Result Init(unsigned xOffset, unsigned yOffset, unsigned xRes, unsigned yRes, unsigned totalXRes, unsigned totalYRes, unsigned aaTarget);

	/*
	* CRender3D(config):
	* ~CRender3D(void):
	*
	* Constructor and destructor.
	*
	* Parameters:
	*   config  Run-time configuration.
	*/
	CNew3D(const Util::Config::Node &config, const std::string& gameName);
	~CNew3D(void);

private:
	/*
	* Private Members
	*/

	bool RenderScene(int priority, bool renderOverlay, Layer layer);		// returns if has overlay plane
	void DrawScrollFog();
	void DrawAmbientFog();
	void SetRenderStates();
	void DisableRenderStates();
	bool ProcessLos(int priority);

	/*
	* Data
	*/

	GLenum m_primType;

	TextureBank			m_textureBank[2];

	GLuint m_vao;
	VBO m_vbo;								// large VBO to hold our poly data, start of VBO is ROM data, ram polys follow
	R3DShader m_r3dShader;
	R3DScrollFog m_r3dScrollFog;
	R3DFrameBuffers m_r3dFrameBuffers;
	GLuint m_aaTarget;						// optional, maybe zero
	bool m_ready;
};

} // New3D

#endif  // INCLUDED_NEW3D_H
