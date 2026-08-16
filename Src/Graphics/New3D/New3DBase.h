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
* New3DBase.h
*
* Header file defining the CNew3DBase class: backend-neutral Real3D scene
* decoding. This holds everything that is required to walk the Real3D scene
* database and turn it into the renderer agnostic Node / Model / Mesh / FVertex
* structures. It deliberately contains no graphics API code so that it can be
* shared between the OpenGL and Vulkan renderers.
*/

#ifndef INCLUDED_NEW3DBASE_H
#define INCLUDED_NEW3DBASE_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "Types.h"
#include "Graphics/IRender3D.h"
#include "Model.h"
#include "Mat4.h"
#include "R3DData.h"
#include "Plane.h"
#include "Vec.h"
#include "PolyHeader.h"
#include "Util/NewConfig.h"

#define MAX_RAM_VERTS 300000
#define MAX_ROM_VERTS 1500000

#define BYTE_TO_FLOAT(B)	((2.0f * (B) + 1.0f) * (float)(1.0/255.0))

#define NEAR_PLANE 1e-3f

namespace New3D {

class CNew3DBase : public IRender3D
{
public:
	/*
	* SetStepping(stepping):
	*
	* Sets the Model 3 hardware stepping, which also determines the Real3D
	* functionality. The default is Step 1.0. This should be called prior to
	* any other emulation functions and after Init().
	*
	* Parameters:
	*   stepping  0x10 for Step 1.0, 0x15 for Step 1.5, 0x20 for Step 2.0, or
	*             0x21 for Step 2.1. Anything else defaults to 1.0.
	*/
	void SetStepping(int stepping);

	/*
	* SetSunClamp(bool enable);
	*
	* Sets or unsets the clamped light model
	*
	* Parameters:
	*		enable	Set clamp mode
	*/
	void SetSunClamp(bool enable);

	/*
	* SetBlockCulling(bool enable);
	*
	* When enabled, clears screen to white and disables 3D rendering
	*
	* Parameters:
	*		enable	Set block culling
	*/
	void SetBlockCulling(bool enable);

	/*
	* GetLosValue(int layer);
	*
	* Gets the line of sight value for the priority layer
	*
	* Parameters:
	*		layer	Priority layer to read from
	*/
	float GetLosValue(int layer);

protected:
	/*
	* CNew3DBase(config, gameName):
	*
	* Constructor. Initialises the backend neutral state.
	*
	* Parameters:
	*   config    Run-time configuration.
	*   gameName  Name of the game being emulated.
	*/
	CNew3DBase(const Util::Config::Node& config, const std::string& gameName);

	/*
	* Protected Members
	*/

	// Memory attachment (backend neutral part)
	void AttachMemoryPointers(const UINT32 *cullingRAMLoPtr,
		const UINT32 *cullingRAMHiPtr, const UINT32 *polyRAMPtr,
		const UINT32 *vromPtr, const UINT16 *textureRAMPtr);

	// Per-frame reset of the scene structures
	void ResetPerFrameState();

	// Resets the scene structures and walks the scene database into them. Pure CPU,
	// so it can run before the backend has acquired a frame.
	void PrepareScene();

	// A backend's RenderFrame() begins by calling its own PrepareFrame() when this
	// is false, so the frame comes out the same whether or not the work was hoisted,
	// then clears it. Checked rather than folded into a helper because each backend
	// adds its own CPU-only preparation on top of PrepareScene().
	bool ScenePrepared() const { return m_scenePrepared; }
	void ClearScenePrepared() { m_scenePrepared = false; }

	// Real3D address translation
	const UINT32 *TranslateCullingAddress(UINT32 addr);
	const UINT32 *TranslateModelAddress(UINT32 addr);

	// Matrix stack
	void MultMatrix(UINT32 matrixOffset, Mat4& mat);
	void InitMatrixStack(UINT32 matrixBaseAddr, Mat4& mat);
	void ResetMatrix(Mat4& mat) const;

	// Scene database traversal
	bool DrawModel(UINT32 modelAddr);
	void DescendCullingNode(UINT32 addr);
	void DescendPointerList(UINT32 addr);
	void DescendNodePtr(UINT32 nodeAddr);
	void RenderViewport(UINT32 addr);

	// building the scene
	int	GetTexFormat(int originalFormat, bool contour) const;
	void SetMeshValues(SortingMesh *currentMesh, PolyHeader &ph);
	void CacheModel(Model *m, const UINT32 *data);
	void CopyVertexData(const R3DPoly& r3dPoly, std::vector<FVertex>& vertexArray);
	void GetCoordinates(int width, int height, UINT16 uIn, UINT16 vIn, float uvScale, float& uOut, float& vOut) const;

	bool IsDynamicModel(UINT32 *data) const;				// check if the model has a colour palette
	bool IsVROMModel(UINT32 modelAddr) const;
	bool SkipLayer(int layer);
	void TranslateLosPosition(int inX, int inY, int& outX, int& outY) const;
	void CalcViewport(Viewport* vp);
	void TranslateTexture(unsigned& x, unsigned& y, int width, int height, int& page) const;

	/*
	* Data
	*/

	// Misc
	std::string m_gameName;
	int m_numPolyVerts;

	// GPU configuration
	std::atomic_bool m_sunClamp;
	std::atomic_bool m_blockCulling;
	bool m_noWhiteFlash;

	// Stepping
	int		m_step;
	int		m_offset;			// offset to subtract for words 3 and higher of culling nodes
	float	m_vertexFactor;		// fixed-point conversion factor for vertices
	float	m_textureNPFactor;	// fixed-point conversion factor for texture NP values

	// Memory (passed from outside)
	const UINT32	*m_cullingRAMLo;	// 4 MB
	const UINT32	*m_cullingRAMHi;	// 1 MB
	const UINT32	*m_polyRAM;			// 4 MB
	const UINT32	*m_vrom;			// 64 MB
	const UINT16	*m_textureRAM;		// 8 MB

	// Resolution and scaling factors (to support resolutions higher than 496x384) and offsets
	float		m_xRatio, m_yRatio;
	unsigned	m_xOffs, m_yOffs;
	unsigned	m_xRes, m_yRes;			// Model 3 display area within the New3D render target
	unsigned  	m_totalXRes, m_totalYRes;	// complete New3D render-target resolution
	bool		m_wideScreen;
	int			m_renderScale;
	unsigned	m_outputXOffs, m_outputYOffs;
	unsigned	m_outputXRes, m_outputYRes;	// destination area within the output surface

	// Real3D Base Matrix Pointer
	const float	*m_matrixBasePtr;
	UINT32 m_colorTableAddr = 0x400;		// address of color table in polygon RAM
	LODBlendTable* m_LODBlendTable;

	NodeAttributes	m_nodeAttribs;
	Mat4			m_modelMat;				// current modelview matrix

	struct LOS
	{
		float value[4] = { 0,0,0,0 };		// line of sight value for each priority layer
	} m_los[2];

	LOS* m_losFront = &m_los[0];			// we need to double buffer this because 3d works in separate thread
	LOS* m_losBack = &m_los[1];
	std::mutex m_losMutex;

	Vertex			m_prev[4];				// these are class variables because sega bass fishing starts meshes with shared vertices from the previous one
	UINT16			m_prevTexCoords[4][2];	// basically relying on undefined behavour

	// True between PrepareScene() and the RenderFrame() that consumes its result.
	bool m_scenePrepared = false;
	// m_blockCulling latched by PrepareScene(). The scene build and the draw that
	// follows it must agree on whether 3D is disabled, and with the two now up to a
	// frame apart, re-reading the atomic in between could disagree.
	bool m_blockCullingLatched = false;

	std::vector<Node>	 m_nodes;				// this represents the entire render frame
	// Nodes retired at the end of a frame, kept only so their models vectors can be
	// handed back to the next frame instead of being reallocated.
	std::vector<Node>	 m_nodePool;

	std::vector<FVertex> m_polyBufferRam;		// dynamic polys
	std::vector<FVertex> m_polyBufferRom;		// rom polys
	std::unordered_map<UINT32, std::shared_ptr<std::vector<Mesh>>> m_romMap;	// a hash table for all the ROM models. The meshes don't have model matrices or tex offsets yet

	struct
	{
		float bnlu;
		float bnlv;
		float bntu;
		float bntw;
		float bnru;
		float bnrv;
		float bnbu;
		float bnbw;
		float correction;
	} m_planes;
};

} // New3D

#endif  // INCLUDED_NEW3DBASE_H
