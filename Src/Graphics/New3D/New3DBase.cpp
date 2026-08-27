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

#include "New3DBase.h"
#include "Vec.h"
#include <cmath>
#include <algorithm>
#include <limits>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include "R3DFloat.h"
#include "Util/BitCast.h"

namespace New3D {

CNew3DBase::CNew3DBase(const Util::Config::Node &config, const std::string& gameName) :
	m_gameName(gameName),
	m_blockCulling(false),
	m_cullingRAMLo(nullptr),
	m_cullingRAMHi(nullptr),
	m_polyRAM(nullptr),
	m_vrom(nullptr),
	m_textureRAM(nullptr),
	m_xRatio(0),
	m_yRatio(0),
	m_xOffs(0),
	m_yOffs(0),
	m_xRes(0),
	m_yRes(0),
	m_totalXRes(0),
	m_totalYRes(0),
	m_wideScreen(config["WideScreen"].ValueAs<bool>()),
	m_renderScale(config["RenderScale"].ValueAs<int>()),
	m_outputXOffs(0),
	m_outputYOffs(0),
	m_outputXRes(0),
	m_outputYRes(0),
	m_matrixBasePtr(nullptr),
	m_LODBlendTable(nullptr),
	m_prev{{}},
	m_prevTexCoords{{}}
{
	m_sunClamp		= true;
	m_numPolyVerts	= 3;

	if (config["QuadRendering"].ValueAs<bool>()) {
		m_numPolyVerts	= 4;
	}

	m_noWhiteFlash = config["NoWhiteFlash"].ValueAs<bool>();
}

void CNew3DBase::AttachMemoryPointers(const UINT32 *cullingRAMLoPtr, const UINT32 *cullingRAMHiPtr, const UINT32 *polyRAMPtr, const UINT32 *vromPtr, const UINT16 *textureRAMPtr)
{
	m_cullingRAMLo	= cullingRAMLoPtr;
	m_cullingRAMHi	= cullingRAMHiPtr;
	m_polyRAM		= polyRAMPtr;
	m_vrom			= vromPtr;
	m_textureRAM	= textureRAMPtr;
}

// per-frame reset of the scene structures, must run before the scene is rebuilt
void CNew3DBase::ResetPerFrameState()
{
	{
		std::lock_guard<std::mutex> guard(m_losMutex);
		std::swap(m_losBack, m_losFront);
		for (int i = 0; i < 4; i++) {
			m_losBack->value[i] = 0;
		}
	}

	// release any resources from last frame
	m_polyBufferRam.clear();		// clear dynamic model memory buffer

	// Retire the nodes into the pool rather than destroying them. Moving a Node hands
	// over its models vector, so the per-viewport storage survives to be reused. Left
	// to clear() the vectors are freed and re-reserved every frame, and at 2048 models
	// each that is large enough for the allocator to take it from the system and give
	// it straight back again.
	for (auto& n : m_nodes) {
		n.models.clear();			// retains the capacity reserve() paid for
		m_nodePool.emplace_back(std::move(n));
	}
	m_nodes.clear();				// memory will grow during the object life time, that's fine, no need to shrink to fit

	m_modelMat.Release();			// would hope we wouldn't need this but no harm in checking
	m_nodeAttribs.Reset();
}

// Walks the scene database into the Node / Model / Mesh / FVertex structures. No
// graphics API calls, so this is valid outside an acquired backend frame.
void CNew3DBase::PrepareScene()
{
	ResetPerFrameState();

	m_blockCullingLatched = m_blockCulling && !m_noWhiteFlash;
	m_scenePrepared = true;

	if (m_blockCullingLatched) {
		return;					// block culling disables 3D rendering, nothing to build
	}

	RenderViewport(0x800000);	// build model structure
}

void CNew3DBase::SetStepping(int stepping)
{
	m_step = stepping;

	if ((m_step != 0x10) && (m_step != 0x15) && (m_step != 0x20) && (m_step != 0x21)) {
		m_step = 0x10;
	}

	if (m_step > 0x10) {
		m_offset = 0;							// culling nodes are 10 words
		m_vertexFactor = (1.0f / 2048.0f);		// vertices are in 13.11 format
		m_textureNPFactor = (1.0f / 16384.0f);	// texture NP values are in 10.14 format
	}
	else {
		m_offset = 2;							// 8 words
		m_vertexFactor = (1.0f / 128.0f);		// 17.7
		m_textureNPFactor = (1.0f / 4096.0f);	// 12.12
	}
}

bool CNew3DBase::SkipLayer(int layer)
{
	for (const auto &n : m_nodes) {
		if (n.viewport.priority == layer) {
			if (!n.models.empty()) {
				return false;
			}
		}
	}

	return true;
}

/******************************************************************************
Real3D Address Translation

Functions that interpret word-granular Real3D addresses and return pointers.
******************************************************************************/

// Translates 24-bit culling RAM addresses
const UINT32* CNew3DBase::TranslateCullingAddress(UINT32 addr)
{
	addr &= 0x00FFFFFF;	// caller should have done this already

	if ((addr >= 0x800000) && (addr < 0x840000)) {
		return &m_cullingRAMHi[addr & 0x3FFFF];
	}
	else if (addr < 0x100000) {
		return &m_cullingRAMLo[addr];
	}

	return nullptr;
}

// Translates model references
const UINT32* CNew3DBase::TranslateModelAddress(UINT32 modelAddr)
{
	modelAddr &= 0x00FFFFFF;	// caller should have done this already

	if (modelAddr < 0x100000) {
		return &m_polyRAM[modelAddr];
	}
	else {
		return &m_vrom[modelAddr];
	}
}

bool CNew3DBase::DrawModel(UINT32 modelAddr)
{
	bool cached = false;

	const UINT32* const modelAddress = TranslateModelAddress(modelAddr);

	// create a new model to push onto the vector
	m_nodes.back().models.emplace_back();

	// get the last model in the array
	Model* const m = &m_nodes.back().models.back();

	if (IsVROMModel(modelAddr)) {

		// try to find meshes in the rom cache
		const auto cacheEntry = m_romMap.find(modelAddr);

		if (cacheEntry != m_romMap.end() && cacheEntry->second) {
			// The cache is only populated for models that were not dynamic when first
			// seen, and VROM cannot have changed since, so the answer is already known.
			// Asking IsDynamicModel() again would walk the model's whole polygon chain
			// for every reference to it, on every frame, to reach the same conclusion.
			m->meshes = cacheEntry->second;
			m->dynamic = false;
			cached = true;
		}
		else if (!IsDynamicModel((UINT32*)modelAddress)) {
			m->meshes = std::make_shared<std::vector<Mesh>>();
			m_romMap[modelAddr] = m->meshes;		// store meshes in our rom map here
			m->dynamic = false;
		}
		else {
			m->meshes = std::make_shared<std::vector<Mesh>>();
		}
	}
	else {
		m->meshes = std::make_shared<std::vector<Mesh>>();
	}

	// copy current model matrix
	for (int i = 0; i < 16; i++) {
		m->modelMat[i] = m_modelMat.currentMatrix[i];
	}

	// update texture offsets
	m->textureOffsetX	= m_nodeAttribs.currentTexOffsetX;
	m->textureOffsetY	= m_nodeAttribs.currentTexOffsetY;
	m->page				= m_nodeAttribs.currentPage;
	m->scale			= m_nodeAttribs.currentModelScale;
	m->alpha			= m_nodeAttribs.currentModelAlpha;

	if (!cached) {
		CacheModel(m, modelAddress);
	}

	return true;
}

/*
	0x00:   x------- -------- -------- --------	Is UF ref
			-x------ -------- -------- --------	Is 3D model
			--x----- -------- -------- --------	Is point
			---x---- -------- -------- --------	Is point ref
			----x--- -------- -------- --------	Is animation
			-----x-- -------- -------- --------	Is billboard
			------x- -------- -------- --------	Child is billboard
			-------x -------- -------- --------	Extra child pointer needed
			-------- xxxxx--- -------- --------	Spare (unknown if used)
			-------- -----xxx xxxxxx-- --------	Node ID
			-------- -------- ------x- --------	Discard 1
			-------- -------- -------x --------	Discard 2

			-------- -------- -------- x-------	Reset matrix
			-------- -------- -------- -x------	Use child pointer
			-------- -------- -------- --x-----	Use sibling pointer
			-------- -------- -------- ---x----	No matrix
			-------- -------- -------- ----x---	Indirect child
			-------- -------- -------- -----x--	Valid color table
			-------- -------- -------- ------xx	Node type(0 = viewport, 1 = root node, 2 = culling node)

	0x01, 0x02 only present on Step 1.5+

	0x01:   xxxxxxxx xxxxxxxx xxxxxxxx xxxxxx--	Model scale (float32) last 2 bits are control words
			-------- -------- -------- ------x- Disable culling
			-------- -------- -------- -------x	Valid model scale

	0x02 :	-------- -------- x------- --------	Texture replace
			-------- -------- -x------ --------	Switch bank
			-------- -------- --xxxxxx x-------	X offset
			-------- -------- -------- -xxxxxxx	Y offset

	0x03 :	xxxxxxxx xxxxx--- -------- --------	Color table address 1
			-------- -----xxx xxxx---- --------	LOD table pointer
			-------- -------- ----xxxx xxxxxxxx	Node matrix

	0x04:   Translation X coordinate
	0x05:   Translation Y coordinate
	0x06:   Translation Z coordinate

	0x07:   xxxx---- -------- -------- -------- Color table address 2
			-----x-- -------- -------- -------- Sibling table
			------x- -------- -------- -------- Point
			-------x -------- -------- -------- Leaf node
			-------- xxxxxxxx xxxxxxxx xxxxxxxx Child pointer

	0x08:   xxxxxxx- -------- -------- -------- Color table address 3
			-------x -------- -------- -------- Null sibling
			-------- xxxxxxxx xxxxxxxx xxxxxxxx Sibling pointer

	0x09:   xxxxxxxx xxxxxxxx -------- -------- Blend radius
			-------- -------- xxxxxxxx xxxxxxxx Culling radius
*/

void CNew3DBase::DescendCullingNode(UINT32 addr)
{
	enum class NodeType { undefined = -1, viewport = 0, rootNode = 1, cullingNode = 2 };

	const UINT32	*node, *lodPtr;
	UINT32			matrixOffset, child1Ptr, sibling2Ptr;
	UINT16			uCullRadius;
	float			fCullRadius;
	UINT16			uBlendRadius;
	float			fBlendRadius;
	UINT8			lodTablePointer;
	NodeType		nodeType;
	bool			resetMatrix;

	if (m_nodeAttribs.StackLimit()) {
		return;
	}

	node = TranslateCullingAddress(addr);

	if (NULL == node) {
		return;
	}

	// Extract known fields
	nodeType		= (NodeType)(node[0x00] & 3);
	child1Ptr		= node[0x07 - m_offset] & 0x7FFFFFF;	// mask colour table bits
	sibling2Ptr		= node[0x08 - m_offset] & 0x1FFFFFF;	// mask colour table bits
	matrixOffset	= node[0x03 - m_offset] & 0xFFF;
	resetMatrix		= (node[0x0] & 0x80) > 0;
	lodTablePointer = (node[0x03 - m_offset] >> 12) & 0x7F;

	// check our node type
	if (nodeType == NodeType::viewport) {
		return;												// viewport nodes aren't rendered
	}

	// node discard
	if ((0x300 & node[0]) == 0x300) {						// This allows to set node transparency. If both bits are set object is completely invisible.
		return;
	}

	// parse siblings 
	if ((node[0x00] & 0x07) != 0x06) {						// colour table seems to indicate no siblings
		if (!(sibling2Ptr & 0x1000000) && sibling2Ptr) {
			DescendCullingNode(sibling2Ptr);				// no need to mask bit, would already be zero
		}
	}

	if ((node[0x00] & 0x04)) {
		m_colorTableAddr = ((node[0x03 - m_offset] >> 19) << 0) | ((node[0x07 - m_offset] >> 28) << 13) | ((node[0x08 - m_offset] >> 25) << 17);
		m_colorTableAddr &= 0x000FFFFF; // clamp to 4MB (in words) range
	}

	m_nodeAttribs.Push();	// save current attribs

	if (!m_offset) {		// Step 1.5+

		if (node[0x01] & 1)
			m_nodeAttribs.currentModelScale = Util::Uint32AsFloat(node[0x01] & ~3);	// mask out control bits

		if (node[0x01] & 2)
			m_nodeAttribs.currentDisableCulling = true;

		// apply texture offsets, else retain current ones
		if ((node[0x02] & 0x8000))	{
			int tx = 32 * ((node[0x02] >> 7) & 0x3F);
			int ty = 32 * (node[0x02] & 0x1F);
			m_nodeAttribs.currentTexOffsetX	= tx;
			m_nodeAttribs.currentTexOffsetY = ty;
			m_nodeAttribs.currentPage = (node[0x02] & 0x4000) >> 14;
		}
	}

	// Apply matrix and translation
	m_modelMat.PushMatrix();

	// apply translation vector
	if (node[0x00] & 0x10) {
		float centroid_x = Util::Uint32AsFloat(node[0x04 - m_offset]);
		float centroid_y = Util::Uint32AsFloat(node[0x05 - m_offset]);
		float centroid_z = Util::Uint32AsFloat(node[0x06 - m_offset]);
		m_modelMat.Translate(centroid_x, centroid_y, centroid_z);
	}
	// multiply matrix, if specified
	else if (matrixOffset) {
		MultMatrix(matrixOffset,m_modelMat);
	}

	if (resetMatrix) {
		ResetMatrix(m_modelMat);
	}

	float& x = m_modelMat.currentMatrix[12];
	float& y = m_modelMat.currentMatrix[13];
	float& z = m_modelMat.currentMatrix[14];

	uCullRadius = node[9 - m_offset] & 0xFFFF;
	fCullRadius = R3DFloat::GetFloat16(uCullRadius) * m_nodeAttribs.currentModelScale;;

	uBlendRadius = node[9 - m_offset] >> 16;
	fBlendRadius = R3DFloat::GetFloat16(uBlendRadius) * m_nodeAttribs.currentModelScale;;

	bool outsideFrustum = false;
	if ((z * m_planes.bnlu - x * m_planes.bnlv * m_planes.correction) > fCullRadius ||
		(z * m_planes.bntu + y * m_planes.bntw) > fCullRadius ||
		(z * m_planes.bnru - x * m_planes.bnrv * m_planes.correction) > fCullRadius ||
		(z * m_planes.bnbu + y * m_planes.bnbw) > fCullRadius)
	{
		outsideFrustum = true;
	}

	float LODscale = m_nodeAttribs.currentDisableCulling ? std::numeric_limits<float>::max() : (fBlendRadius / std::hypot(x, y, z));
	const LOD * const lod = m_LODBlendTable->table[lodTablePointer].lod;

	LODscale = std::clamp(LODscale, 0.0f, std::numeric_limits<float>::max());

	if (m_nodeAttribs.currentDisableCulling || (!outsideFrustum && LODscale >= lod[3].deleteSize)) {

		// Descend down first link
		if ((node[0x00] & 0x08))	// 4-element LOD table
		{
			lodPtr = TranslateCullingAddress(child1Ptr);

			if (nullptr != lodPtr)
			{
				int modelLOD;
				for (modelLOD = 0; modelLOD < 3; modelLOD++)
				{
					if (LODscale >= lod[modelLOD].deleteSize && lodPtr[modelLOD] & 0x1000000)
						break;
				}

				float tempAlpha = m_nodeAttribs.currentModelAlpha;

				float nodeAlpha = lod[modelLOD].blendFactor * (LODscale - lod[modelLOD].deleteSize);
				nodeAlpha = std::clamp(nodeAlpha, 0.0f, 1.0f);
				if (nodeAlpha > (float)(31.0 / 32.0))		// shader discards pixels below 1/32 alpha
					nodeAlpha = 1.0f;
				else if (nodeAlpha < (float)(1.0 / 32.0))
					nodeAlpha = 0.0f;
				m_nodeAttribs.currentModelAlpha *= nodeAlpha;	// alpha of each node multiples by the alpha of its parent
				
				if ((node[0x03 - m_offset] & 0x20000000)) {
					DescendCullingNode(lodPtr[modelLOD] & 0xFFFFFF);

					if (nodeAlpha < 1.0f && modelLOD != 3)
					{
						m_nodeAttribs.currentModelAlpha = (1.0f - nodeAlpha) * tempAlpha;
						DescendCullingNode(lodPtr[modelLOD+1] & 0xFFFFFF);
					}
				}
				else {
					DrawModel(lodPtr[modelLOD] & 0xFFFFFF);

					if (nodeAlpha < 1.0f && modelLOD != 3)
					{
						m_nodeAttribs.currentModelAlpha = (1.0f - nodeAlpha) * tempAlpha;
						DrawModel(lodPtr[modelLOD + 1] & 0xFFFFFF);
					}
				}
			}
		}
		else {

			float nodeAlpha = lod[3].blendFactor * (LODscale - lod[3].deleteSize);
			nodeAlpha = std::clamp(nodeAlpha, 0.0f, 1.0f);
			m_nodeAttribs.currentModelAlpha *= nodeAlpha;	// alpha of each node multiples by the alpha of its parent

			DescendNodePtr(child1Ptr);
		}

	}

	m_modelMat.PopMatrix();

	// Restore old texture offsets
	m_nodeAttribs.Pop();
}

void CNew3DBase::DescendNodePtr(UINT32 nodeAddr)
{
	// Ignore null links
	if ((nodeAddr & 0x00FFFFFF) == 0) {
		return;
	}

	switch ((nodeAddr >> 24) & 0x5)		// pointer type encoded in upper 8 bits
	{
	case 0x00:
		DescendCullingNode(nodeAddr & 0xFFFFFF);
		break;
	case 0x01:
		DrawModel(nodeAddr & 0xFFFFFF);
		break;
	case 0x04:
		DescendPointerList(nodeAddr & 0xFFFFFF);
		break;
	default:
		break;
	}
}

void CNew3DBase::DescendPointerList(UINT32 addr)
{
	const UINT32* const list = TranslateCullingAddress(addr);

	if (nullptr == list) {
		return;
	}

	int index = 0;

	while (true) {

		if (list[index] & 0x01000000) {
			break;	// empty list
		}

		UINT32 nodeAddr = list[index] & 0x00FFFFFF;	// clear upper 8 bits to ensure this is processed as a culling node

		DescendCullingNode(nodeAddr);

		if (list[index] & 0x02000000) {
			break;	// list end
		}

		index++;
	}
}


/******************************************************************************
Matrix Stack
******************************************************************************/

// Macro to generate column-major (OpenGL) index from y,x subscripts
#define CMINDEX(y,x)	(x*4+y)

/*
* MultMatrix():
*
* Multiplies the matrix stack by the specified Real3D matrix. The matrix
* index is a 12-bit number specifying a matrix number relative to the base.
* The base matrix MUST be set up before calling this function.
*/
void CNew3DBase::MultMatrix(UINT32 matrixOffset, Mat4& mat)
{
	float		m[4*4];
	const float	*src = &m_matrixBasePtr[matrixOffset * 12];

	if (m_matrixBasePtr == NULL)	// LA Machineguns
		return;

	m[CMINDEX(0, 0)] = src[3];
	m[CMINDEX(0, 1)] = src[4];
	m[CMINDEX(0, 2)] = src[5];
	m[CMINDEX(0, 3)] = src[0];
	m[CMINDEX(1, 0)] = src[6];
	m[CMINDEX(1, 1)] = src[7];
	m[CMINDEX(1, 2)] = src[8];
	m[CMINDEX(1, 3)] = src[1];
	m[CMINDEX(2, 0)] = src[9];
	m[CMINDEX(2, 1)] = src[10];
	m[CMINDEX(2, 2)] = src[11];
	m[CMINDEX(2, 3)] = src[2];
	m[CMINDEX(3, 0)] = 0.0;
	m[CMINDEX(3, 1)] = 0.0;
	m[CMINDEX(3, 2)] = 0.0;
	m[CMINDEX(3, 3)] = 1.0;

	mat.MultMatrix(m);
}

/*
* InitMatrixStack():
*
* Initializes the modelview (model space -> view space) matrix stack and
* Real3D coordinate system. These are the last transforms to be applied (and
* the first to be defined on the stack) before projection.
*
* Model 3 games tend to define the following unusual base matrix:
*
*		0	0	-1	0
*		1	0	0	0
*		0	-1	0	0
*		0	0	0	1
*
* When this is multiplied by a column vector, the output is:
*
*		-Z
*		X
*		-Y
*		1
*
* My theory is that the Real3D GPU accepts vectors in Z,X,Y order. The games
* store everything as X,Y,Z and perform the translation at the end. The Real3D
* also has Y and Z coordinates opposite of the OpenGL convention. This
* function inserts a compensating matrix to undo these things.
*
* NOTE: This function assumes we are in GL_MODELVIEW matrix mode.
*/

void CNew3DBase::InitMatrixStack(UINT32 matrixBaseAddr, Mat4& mat)
{
	float m[4 * 4];

	// This matrix converts vectors back from the weird Model 3 Z,X,Y ordering
	// and also into OpenGL viewspace (-Y,-Z)
	m[CMINDEX(0, 0)] = 0.0;	m[CMINDEX(0, 1)] = 1.0;	m[CMINDEX(0, 2)] = 0.0;	m[CMINDEX(0, 3)] = 0.0;
	m[CMINDEX(1, 0)] = 0.0;	m[CMINDEX(1, 1)] = 0.0;	m[CMINDEX(1, 2)] =-1.0; m[CMINDEX(1, 3)] = 0.0;
	m[CMINDEX(2, 0)] =-1.0; m[CMINDEX(2, 1)] = 0.0;	m[CMINDEX(2, 2)] = 0.0;	m[CMINDEX(2, 3)] = 0.0;
	m[CMINDEX(3, 0)] = 0.0;	m[CMINDEX(3, 1)] = 0.0;	m[CMINDEX(3, 2)] = 0.0;	m[CMINDEX(3, 3)] = 1.0;

	mat.LoadMatrix(m);

	// Set matrix base address and apply matrix #0 (coordinate system matrix)
	m_matrixBasePtr = (float *)TranslateCullingAddress(matrixBaseAddr);
	MultMatrix(0, mat);
}

// what this does is to set the rotation back to zero, whilst keeping the position and scale of the current matrix
void CNew3DBase::ResetMatrix(Mat4& mat) const
{
	float m[16];
	memcpy(m, mat.currentMatrix, 16 * 4);

	// transpose the top 3x3 of the matrix (this effectively inverts the rotation). When we multiply our new matrix it'll effectively cancel out the rotations.
	std::swap(m[1], m[4]);
	std::swap(m[2], m[8]);
	std::swap(m[6], m[9]);

	// set position to zero
	m[12] = 0;
	m[13] = 0;
	m[14] = 0;
	m[15] = 1;

	// normalise columns, this removes the scaling, otherwise we'll apply it twice
	float s1 = std::sqrt((m[0] * m[0]) + (m[1] * m[1]) + (m[2] * m[2]));
	float s2 = std::sqrt((m[4] * m[4]) + (m[5] * m[5]) + (m[6] * m[6]));
	float s3 = std::sqrt((m[8] * m[8]) + (m[9] * m[9]) + (m[10] * m[10]));

	m[0] /= s1;		m[4] /= s2;		m[8] /= s3;
	m[1] /= s1;		m[5] /= s2;		m[9] /= s3;
	m[2] /= s1;		m[6] /= s2;		m[10] /= s3;

	mat.MultMatrix(m);
}

// Draws viewports of the given priority
void CNew3DBase::RenderViewport(UINT32 addr)
{
	static const float	color[8][3] =
	{											// RGB1 color translation
		{ 0.0f, 0.0f, 0.0f },	// off
		{ 0.0f, 0.0f, 1.0f },	// blue
		{ 0.0f, 1.0f, 0.0f },	// green
		{ 0.0f, 1.0f, 1.0f },	// cyan
		{ 1.0f, 0.0f, 0.0f }, 	// red
		{ 1.0f, 0.0f, 1.0f },	// purple
		{ 1.0f, 1.0f, 0.0f },	// yellow
		{ 1.0f, 1.0f, 1.0f }	// white
	};

	if ((addr & 0x00FFFFFF) == 0) {
		return;
	}

	// Translate address and obtain pointer
	const uint32_t * const vpnode = TranslateCullingAddress(addr);

	if (nullptr == vpnode) {
		return;
	}

	bool vpDisabled = vpnode[0] & 0x20;						// only if viewport enabled

	{
		// create node object, reusing one retired by ResetPerFrameState when possible
		if (m_nodePool.empty()) {
			m_nodes.emplace_back(Node());
			m_nodes.back().models.reserve(2048);			// create space for models
		}
		else {
			m_nodes.emplace_back(std::move(m_nodePool.back()));
			m_nodePool.pop_back();
			m_nodes.back().viewport = Viewport();			// as if freshly constructed
		}

		// get pointer to its viewport
		Viewport* vp = &m_nodes.back().viewport;

		vp->priority	= (vpnode[0] >> 3) & 0x3;
		vp->select		= (vpnode[0] >> 8) & 0x3;
		vp->number		= (vpnode[0] >> 10);

		// Fetch viewport parameters
		vp->vpX			= ((vpnode[0x1A] & 0xFFFF) * (float)(1.0 / 16.0)) ;		// viewport X (12.4 fixed point)
		vp->vpY			= ((vpnode[0x1A] >> 16) * (float)(1.0 / 16.0));			// viewport Y (12.4)
		vp->vpWidth		= ((vpnode[0x14] & 0xFFFF) * (float)(1.0 / 4.0));		// width (14.2)
		vp->vpHeight	= ((vpnode[0x14] >> 16) * (float)(1.0 / 4.0));			// height (14.2)

		uint32_t matrixBase = vpnode[0x16] & 0xFFFFFF;							// matrix base address

		m_LODBlendTable = (LODBlendTable*)TranslateCullingAddress(vpnode[0x17] & 0xFFFFFF);

		float cv = Util::Uint32AsFloat(vpnode[0x8]);	// 1/(left-right)
		float cw = Util::Uint32AsFloat(vpnode[0x9]);	// 1/(top-bottom)
		float io = Util::Uint32AsFloat(vpnode[0xa]);	// top / bottom (ratio) - ish
		float jo = Util::Uint32AsFloat(vpnode[0xb]);	// left / right (ratio)

		// clipping plane normals
		m_planes.bnlu = Util::Uint32AsFloat(vpnode[0xc]);
		m_planes.bnlv = Util::Uint32AsFloat(vpnode[0xd]);
		m_planes.bntu = Util::Uint32AsFloat(vpnode[0xe]);
		m_planes.bntw = Util::Uint32AsFloat(vpnode[0xf]);
		m_planes.bnru = Util::Uint32AsFloat(vpnode[0x10]);
		m_planes.bnrv = Util::Uint32AsFloat(vpnode[0x11]);
		m_planes.bnbu = Util::Uint32AsFloat(vpnode[0x12]);
		m_planes.bnbw = Util::Uint32AsFloat(vpnode[0x13]);
		m_planes.correction = 1.0f;		// might get changed by the calc viewport method

		vp->angle_left		= (0.0f - jo) / cv;
		vp->angle_right		= (1.0f - jo) / cv;
		vp->angle_bottom	= -(1.0f - io) / cw;
		vp->angle_top		= -(0.0f - io) / cw;

		vp->cota = Util::Uint32AsFloat(vpnode[0x3]);

		CalcViewport(vp);

		// Lighting (note that sun vector points toward sun -- away from vertex)
		vp->lightingParams[0] = Util::Uint32AsFloat(vpnode[0x05]);							// sun X
		vp->lightingParams[1] = -Util::Uint32AsFloat(vpnode[0x06]);							// sun Y (- to convert to ogl cordinate system)
		vp->lightingParams[2] = -Util::Uint32AsFloat(vpnode[0x04]);							// sun Z (- to convert to ogl cordinate system)
		vp->lightingParams[3] = std::max(0.f, std::min(Util::Uint32AsFloat(vpnode[0x07]), 1.0f));	// sun intensity (clamp to 0-1)
		vp->lightingParams[4] = (float)((vpnode[0x24] >> 8) & 0xFF) * (float)(1.0 / 255.0);	// ambient intensity
		vp->lightingParams[5] = 0.0f;	// reserved

		vp->sunClamp = m_sunClamp;
		vp->intensityClamp = (m_step == 0x10);		// just step 1.0 ?
		vp->hardwareStep = m_step;

		// Spotlight
		int spotColorIdx = (vpnode[0x20] >> 11) & 7;									// spotlight color index
		int spotFogColorIdx = (vpnode[0x20] >> 8) & 7;									// spotlight on fog color index
		vp->spotEllipse[0] = (float)(INT16)(vpnode[0x1E] & 0xFFFF) * (float)(1.0 / 8.0);// spotlight X position (13.3 fixed point)
		vp->spotEllipse[1] = (float)(INT16)(vpnode[0x1D] & 0xFFFF) * (float)(1.0 / 8.0);// spotlight Y
		vp->spotEllipse[2] = (float)((vpnode[0x1E] >> 16) & 0xFFFF);					// spotlight X size (16-bit)
		vp->spotEllipse[3] = (float)((vpnode[0x1D] >> 16) & 0xFFFF);					// spotlight Y size

		vp->spotRange[0] = 1.0f / Util::Uint32AsFloat(vpnode[0x21]);					// spotlight start
		vp->spotRange[1] = Util::Uint32AsFloat(vpnode[0x1F]);							// spotlight extent

		vp->spotColor[0] = color[spotColorIdx][0];										// spotlight color
		vp->spotColor[1] = color[spotColorIdx][1];
		vp->spotColor[2] = color[spotColorIdx][2];

		vp->spotFogColor[0] = color[spotFogColorIdx][0];								// spotlight color on fog
		vp->spotFogColor[1] = color[spotFogColorIdx][1];
		vp->spotFogColor[2] = color[spotFogColorIdx][2];

		// spotlight is specified in terms of physical resolution
		vp->spotEllipse[1] = 384.0f - vp->spotEllipse[1];								// flip Y position

		// Avoid division by zero
		vp->spotEllipse[2] = std::max(1.0f, vp->spotEllipse[2]);
		vp->spotEllipse[3] = std::max(1.0f, vp->spotEllipse[3]);

		vp->spotEllipse[2] = std::roundf(2047.0f / vp->spotEllipse[2]);
		vp->spotEllipse[3] = std::roundf(2047.0f / vp->spotEllipse[3]);

		// Scale the spotlight to the OpenGL viewport
		vp->spotEllipse[0] = vp->spotEllipse[0] * m_xRatio + (float)m_xOffs;
		vp->spotEllipse[1] = vp->spotEllipse[1] * m_yRatio + (float)m_yOffs;
		vp->spotEllipse[2] *= m_xRatio;
		vp->spotEllipse[3] *= m_yRatio;

		// Line of sight position
		vp->losPosX = (int)(((vpnode[0x1c] & 0xFFFF) / 16.0f) + 0.5f);					// x position
		vp->losPosY = (int)(((vpnode[0x1c] >> 16) / 16.0f) + 0.5f);						// y position 0 starts from the top

		// Fog
		vp->fogColour[0]	= (float)((vpnode[0x22] >> 16) & 0xFF) * (float)(1.0 / 255.0);	// fog color R
		vp->fogColour[1]	= (float)((vpnode[0x22] >> 8) & 0xFF) * (float)(1.0 / 255.0);	// fog color G
		vp->fogColour[2]	= (float)((vpnode[0x22] >> 0) & 0xFF) * (float)(1.0 / 255.0);	// fog color B
		vp->fogDensity		= std::abs(Util::Uint32AsFloat(vpnode[0x23]));					// fog density	- ocean hunter uses negative values, but looks the same
		vp->fogStart		= (float)(INT16)(vpnode[0x25] & 0xFFFF) * (float)(1.0 / 255.0);	// fog start
		vp->fogAttenuation	= (float)((vpnode[0x24] >> 16) & 0xFF) * (float)(1.0 / 255.0);	// fog attenuation
		vp->fogAmbient		= (float)((vpnode[0x25] >> 16) & 0xFF) * (float)(1.0 / 255.0);	// fog ambient

		// Avoid Infinite and NaN values for Star Wars Trilogy
		if (std::isinf(vp->fogDensity) || std::isnan(vp->fogDensity)) {
			vp->fogDensity = 0;
		}

		vp->scrollFog = (float)(vpnode[0x20] & 0xFF) * (float)(1.0 / 255.0);				// scroll fog
		vp->scrollAtt = (float)(vpnode[0x24] & 0xFF) * (float)(1.0 / 255.0);				// scroll attenuation

		// Clear texture offsets before proceeding
		m_nodeAttribs.Reset();

		// Set up coordinate system and base matrix
		InitMatrixStack(matrixBase, m_modelMat);

		// Descend down the node link. Need to start with a culling node because that defines our culling radius.
		if (!vpDisabled) {
			auto childptr = vpnode[0x02];
			if (((childptr >> 24) & 0x5) == 0) {
				DescendNodePtr(vpnode[0x02]);
			}
		}
	}
	
	// render next viewport
	if (vpnode[0x01] != 0x01000000) {
		RenderViewport(vpnode[0x01]);
	}
}

void CNew3DBase::CopyVertexData(const R3DPoly& r3dPoly, std::vector<FVertex>& vertexArray)
{
	// both lemans 24 and dirt devils are rendering some totally transparent polys as the first object in each viewport
	// in dirt devils it's parallel to the camera so is completely invisible, but breaks our depth calculation
	// in lemans 24 its a sort of diamond shape, but never leaves a hole in the transparent geometry so must be being skipped by the h/w
	if (r3dPoly.faceColour[3] == 0) {
		return;
	}

	if (m_numPolyVerts==4) {
		if (r3dPoly.number == 4) {
			vertexArray.emplace_back(r3dPoly, 0);		// construct directly inside container without copy
			vertexArray.emplace_back(r3dPoly, 1);
			vertexArray.emplace_back(r3dPoly, 2);
			vertexArray.emplace_back(r3dPoly, 3);

			// check for identical points (ie forced triangle) and replace with average point
			// if we don't do this our quad code falls apart
			FVertex* v = (&vertexArray.back()) - 3;

			for (int i = 0; i < 4; i++) {

				int next1 = (i + 1) % 4;
				int next2 = (i + 2) % 4;

				if (FVertex::Equal(v[i], v[next1])) {
					FVertex::Average(v[next1], v[next2], v[next1]);
					break;
				}
			}
		}
		else {
			vertexArray.emplace_back(r3dPoly, 0);	
			vertexArray.emplace_back(r3dPoly, 1);
			vertexArray.emplace_back(r3dPoly, 2);
			vertexArray.emplace_back(r3dPoly, 0, 2);	// last point is an average of 0 and 2
		}
	}
	else {
		vertexArray.emplace_back(r3dPoly, 0);
		vertexArray.emplace_back(r3dPoly, 1);
		vertexArray.emplace_back(r3dPoly, 2);

		if (r3dPoly.number == 4) {
			vertexArray.emplace_back(r3dPoly, 0);
			vertexArray.emplace_back(r3dPoly, 2);
			vertexArray.emplace_back(r3dPoly, 3);
		}
	}
}

void CNew3DBase::GetCoordinates(int width, int height, UINT16 uIn, UINT16 vIn, float uvScale, float& uOut, float& vOut) const
{
	uOut = (uIn * uvScale) / width;
	vOut = (vIn * uvScale) / height;
}

int CNew3DBase::GetTexFormat(int originalFormat, bool contour) const
{
	if (!contour) {
		return originalFormat;	// the same
	}

	switch (originalFormat)
	{
	case 1:
	case 2:
	case 3:
	case 4:
		return originalFormat + 7;		// these formats are identical to 1-4, except they lose the 4 bit alpha part when contour is enabled
	default:
		return originalFormat;
	}
}

void CNew3DBase::SetMeshValues(SortingMesh *currentMesh, PolyHeader &ph)
{
	//copy attributes
	currentMesh->textured		= ph.TexEnabled();
	currentMesh->alphaTest		= ph.AlphaTest();
	currentMesh->textureAlpha	= ph.TextureAlpha();
	currentMesh->polyAlpha		= ph.PolyAlpha();
	currentMesh->lighting		= ph.LightEnabled();
	currentMesh->fixedShading	= ph.FixedShading() && !ph.SmoothShading();
	currentMesh->highPriority	= ph.HighPriority();
	currentMesh->transLSelect	= ph.TranslucencyPatternSelect();
	currentMesh->layered		= ph.Layered();
	currentMesh->specular		= ph.SpecularEnabled();
	currentMesh->shininess		= ph.Shininess();
	currentMesh->specularValue	= ph.SpecularValue();
	currentMesh->fogIntensity	= ph.LightModifier();
	currentMesh->translatorMap	= ph.TranslatorMap();
	currentMesh->noLosReturn	= ph.NoLosReturn();
	currentMesh->smoothShading	= ph.SmoothShading();

	if (currentMesh->textured) {

		currentMesh->format = GetTexFormat(ph.TexFormat(), ph.AlphaTest());

		if (currentMesh->format == 7) {
			currentMesh->alphaTest = false;	// alpha test is a 1 bit test, this format needs a lower threshold, since it has 16 levels of transparency
		}

		currentMesh->x				= ph.X();
		currentMesh->y				= ph.Y();
		currentMesh->width			= ph.TexWidth();
		currentMesh->height			= ph.TexHeight();
		currentMesh->microTexture	= ph.MicroTexture();
		currentMesh->inverted		= ph.TranslatorMapOffset() == 2;
		currentMesh->page			= ph.Page();

		{
			bool smoothU = ph.TexSmoothU();
			bool smoothV = ph.TexSmoothV();

			if (ph.AlphaTest()) {
				smoothU = false;	// smooth wrap makes no sense for alpha tested polys with pixel dilate
				smoothV = false;
			}

			if (ph.TexUMirror()) {
				if (smoothU)	currentMesh->wrapModeU = Mesh::TexWrapMode::mirror;
				else			currentMesh->wrapModeU = Mesh::TexWrapMode::mirrorClamp;
			}
			else {
				if (smoothU)	currentMesh->wrapModeU = Mesh::TexWrapMode::repeat;
				else			currentMesh->wrapModeU = Mesh::TexWrapMode::repeatClamp;
			}

			if (ph.TexVMirror()) {
				if (smoothV)	currentMesh->wrapModeV = Mesh::TexWrapMode::mirror;
				else			currentMesh->wrapModeV = Mesh::TexWrapMode::mirrorClamp;
			}
			else {
				if (smoothV)	currentMesh->wrapModeV = Mesh::TexWrapMode::repeat;
				else			currentMesh->wrapModeV = Mesh::TexWrapMode::repeatClamp;
			}
		}

		if (currentMesh->microTexture) {

			currentMesh->microTextureID = ph.MicroTextureID();
			currentMesh->microTextureMinLOD = (float)ph.MicroTextureMinLOD();
		}
	}
}

void CNew3DBase::CacheModel(Model *m, const UINT32 *data)
{
	if (data == nullptr)
		return;

	UINT16			texCoords[4][2];
	PolyHeader		ph;
	UINT64			lastHash	= -1;
	SortingMesh*	currentMesh = nullptr;
	
	std::unordered_map<UINT64, SortingMesh> sMap;

	ph = data; 
	int numTriangles = ph.NumTrianglesTotal();

	// Cache all polygons
	do {

		R3DPoly		p;					// current polygon
		float		uvScale;

		if (ph.header[6] == 0) {
			break;
		}

		// create a hash value based on poly attributes -todo add more attributes
		auto hash = ph.Hash();

		if (hash != lastHash) {

			// One hash of the key instead of two or three. Inserts exactly the same node
			// as count() plus insert() did, so the bucket layout, and with it the order
			// the meshes come back out in below, is unchanged.
			const auto entry = sMap.try_emplace(hash);

			currentMesh = &entry.first->second;

			if (entry.second) {

				//make space for our vertices
				currentMesh->verts.reserve(numTriangles * 3);

				//set mesh values
				SetMeshValues(currentMesh, ph);
			}
		}

		// Obtain basic polygon parameters
		p.number	= ph.NumVerts();
		uvScale		= ph.UVScale();

		ph.FaceNormal(p.faceNormal);

		// Fetch reused vertices according to bitfield, then new verts
		int j = 0;
		for (int i = 0; i < 4; i++)		// up to 4 reused vertices
		{
			if (ph.SharedVertex(i))
			{
				p.v[j] = m_prev[i];

				texCoords[j][0] = m_prevTexCoords[i][0];
				texCoords[j][1] = m_prevTexCoords[i][1];

				//check if we need to recalc tex coords - will only happen if tex tiles are different + sharing vertices
				if (hash != lastHash) {
					if (currentMesh->textured) {
						GetCoordinates(currentMesh->width, currentMesh->height, texCoords[j][0], texCoords[j][1], uvScale, p.v[j].texcoords[0], p.v[j].texcoords[1]);
					}
				}

				j++;
			}
		}

		lastHash = hash;

		// copy face attributes

		if (!ph.PolyColor()) {
			int colorIdx = ph.ColorIndex();
			p.faceColour[2] = (m_polyRAM[m_colorTableAddr + colorIdx] & 0xFF);
			p.faceColour[1] = ((m_polyRAM[m_colorTableAddr + colorIdx] >> 8) & 0xFF);
			p.faceColour[0] = ((m_polyRAM[m_colorTableAddr + colorIdx] >> 16) & 0xFF);
		}
		else {
			p.faceColour[0] = ((ph.header[4] >> 24));
			p.faceColour[1] = ((ph.header[4] >> 16) & 0xFF);
			p.faceColour[2] = ((ph.header[4] >> 8) & 0xFF);
		}

		p.faceColour[3] = ph.Transparency();

		if (ph.Discard1() && !ph.Discard2()) {
			p.faceColour[3] /= 2;
		}

		// if we have flat shading, we can't re-use normals from shared vertices
		for (int i = 0; i < p.number && !ph.SmoothShading(); i++) {
			p.v[i].normal[0] = p.faceNormal[0];
			p.v[i].normal[1] = p.faceNormal[1];
			p.v[i].normal[2] = p.faceNormal[2];
		}

		p.textureNP = ph.TextureNP() * m_textureNPFactor;

		UINT32* vData = ph.StartOfData();	// vertex data starts here

		// remaining vertices are new and defined here
		for (; j < p.number; j++)	
		{
			// Fetch vertices
			UINT32 ix = vData[0];
			UINT32 iy = vData[1];
			UINT32 iz = vData[2];
			UINT32 it = vData[3];

			// Decode vertices
			p.v[j].pos[0] = (((INT32)ix) >> 8) * m_vertexFactor;
			p.v[j].pos[1] = (((INT32)iy) >> 8) * m_vertexFactor;
			p.v[j].pos[2] = (((INT32)iz) >> 8) * m_vertexFactor;
			p.v[j].pos[3] = 1.0f;

			// Per vertex normals
			if (ph.SmoothShading()) {
				p.v[j].normal[0] = BYTE_TO_FLOAT((INT8)(ix & 0xFF));
				p.v[j].normal[1] = BYTE_TO_FLOAT((INT8)(iy & 0xFF));
				p.v[j].normal[2] = BYTE_TO_FLOAT((INT8)(iz & 0xFF));
			}

			if (ph.FixedShading() && !ph.SmoothShading()) {			// fixed shading seems to be disabled if actual normals are set

				//==========
				float shade;
				//==========

				// If specular is enabled fixed shading values are treated as unsigned
				if (ph.SpecularEnabled()) {
					shade = (ix & 0xFF) * (float)(1.0 / 255.0);
				}
				else {
					shade = BYTE_TO_FLOAT((INT8)(ix & 0xFF));
				}
				
				p.v[j].fixedShade = shade;
			}

			float texU = 0;
			float texV = 0;

			// tex coords
			if (currentMesh->textured) {
				GetCoordinates(currentMesh->width, currentMesh->height, (UINT16)(it >> 16), (UINT16)(it & 0xFFFF), uvScale, texU, texV);
			}

			p.v[j].texcoords[0] = texU;
			p.v[j].texcoords[1] = texV;

			//cache un-normalised tex coordinates
			texCoords[j][0] = (UINT16)(it >> 16);
			texCoords[j][1] = (UINT16)(it & 0xFFFF);

			vData += 4;
		}

		// check if we need to double up vertices for two sided lighting
		if (ph.DoubleSided() && !ph.Discard()) {

			R3DPoly tempP = p;

			// flip normals
			V3::inverse(tempP.faceNormal);

			for (int i2 = 0; i2 < tempP.number; i2++) {
				V3::inverse(tempP.v[i2].normal);
			}

			CopyVertexData(tempP, currentMesh->verts);
		}

		// Copy this polygon into the model buffer
		if (!ph.Discard()) {
			CopyVertexData(p, currentMesh->verts);
		}
		
		// Copy current vertices into previous vertex array
		for (int i = 0; i < 4; i++) {
			m_prev[i] = p.v[i];
			m_prevTexCoords[i][0] = texCoords[i][0];
			m_prevTexCoords[i][1] = texCoords[i][1];
		}

	} while (ph.NextPoly());

	//sorted the data, now copy to main data structures

	// we know how many meshes we have to reserve appropriate space
	m->meshes->reserve(sMap.size());

	for (auto& it : sMap) {

		if (m->dynamic) {

			// calculate VBO values for current mesh
			it.second.vboOffset		= (int)m_polyBufferRam.size() + MAX_ROM_VERTS;
			it.second.vertexCount	= (int)it.second.verts.size();

			// copy poly data to main buffer
			m_polyBufferRam.insert(m_polyBufferRam.end(), it.second.verts.begin(), it.second.verts.end());
		}
		else {
			// calculate VBO values for current mesh
			it.second.vboOffset		= (int)m_polyBufferRom.size();
			it.second.vertexCount	= (int)it.second.verts.size();

			// copy poly data to main buffer
			m_polyBufferRom.insert(m_polyBufferRom.end(), it.second.verts.begin(), it.second.verts.end());
		}

		//copy the temp mesh into the model structure
		//this will lose the associated vertex data, which is now copied to the main buffer anyway
		m->meshes->push_back(it.second);
	}
}

bool CNew3DBase::IsDynamicModel(UINT32 *data) const
{
	if (data == nullptr) {
		return false;
	}

	PolyHeader p(data);

	do {

		if ((p.header[1] & 2) == 0) {		// model has rgb colour palette 
			return true;
		}

	} while (p.NextPoly());

	return false;
}

bool CNew3DBase::IsVROMModel(UINT32 modelAddr) const
{
	return modelAddr >= 0x100000;
}

void CNew3DBase::CalcViewport(Viewport* vp)
{
	float l = vp->angle_left;	// we need to calc the shape of the projection frustum for culling
	float r = vp->angle_right;
	float t = vp->angle_top;
	float b = vp->angle_bottom;

	vp->projectionMatrix.LoadIdentity();	// reset matrix

	if (m_wideScreen && (vp->vpX == 0) && (vp->vpWidth >= 495) && (vp->vpY == 0) && (vp->vpHeight >= 383)) {

		/*
		 * Compute aspect ratio correction factor. "Window" refers to the full GL
		 * viewport (i.e., totalXRes x totalYRes). "Viewable area" is the effective
		 * Model 3 screen (xRes x yRes). In non-wide-screen, non-stretch mode, this
		 * is intended to replicate the 496x384 display and may in general be 
		 * smaller than the window. The rest of the window appears to have a
		 * border, which is created by a scissor box.
		 *
		 * In wide-screen mode, we want to expand the frustum horizontally to fill
		 * the window. We want the aspect ratio to be correct. To accomplish this,
		 * the viewable area is set *the same* as in non-wide-screen mode (e.g.,
		 * often smaller than the window) but glScissor() is set by the OSD layer's
		 * screen setup code to reveal the entire window.
		 *
		 * In stretch mode, the window and viewable area are both set the same,
		 * which means there will be no aspect ratio correction and the display
		 * will stretch to fill the entire window while keeping the view frustum
		 * the same as a 496x384 Model 3 display. The display will be distorted.
		 */
		float windowAR = (float)m_totalXRes / (float)m_totalYRes;
		float viewableAreaAR = (float)m_xRes / (float)m_yRes;
		
		// Will expand horizontal frustum planes only in non-stretch mode (wide-
		// screen and non-wide-screen modes have identical resolution parameters
		// and only their scissor box differs)
		float correction = windowAR / viewableAreaAR;
		m_planes.correction = 1.0f / correction;

		vp->x		= 0;
		vp->y		= m_yOffs + (int)((384.0f - (vp->vpY + vp->vpHeight))*m_yRatio);
		vp->width	= m_totalXRes;
		vp->height = (int)(vp->vpHeight*m_yRatio);

		vp->projectionMatrix.FrustumRZ(l*correction, r*correction, b, t, NEAR_PLANE);
	}
	else {

		vp->x		= m_xOffs + (int)(vp->vpX*m_xRatio);
		vp->y		= m_yOffs + (int)((384.0f - (vp->vpY + vp->vpHeight))*m_yRatio);
		vp->width	= (int)(vp->vpWidth*m_xRatio);
		vp->height	= (int)(vp->vpHeight*m_yRatio);

		vp->projectionMatrix.FrustumRZ(l, r, b, t, NEAR_PLANE);
	}
}

void CNew3DBase::TranslateTexture(unsigned& x, unsigned& y, int width, int height, int& page) const
{
	page = y / 1024;

	// remove page from y coordinate
	y -= (page * 1024);
}

void CNew3DBase::SetSunClamp(bool enable)
{
	m_sunClamp = enable;
}

void CNew3DBase::SetBlockCulling(bool enable)
{
	m_blockCulling = enable;
}

float CNew3DBase::GetLosValue(int layer)
{
	// we always write to the 'back' buffer, and the software reads from the front
	// then they get swapped
	std::lock_guard<std::mutex> guard(m_losMutex);
	return m_losFront->value[layer];
}

void CNew3DBase::TranslateLosPosition(int inX, int inY, int& outX, int& outY) const
{
	// remap real3d 496x384 to our new viewport
	inY = 384 - inY;

	outX = m_xOffs + int(inX * m_xRatio);
	outY = m_yOffs + int(inY * m_yRatio);
	if (m_renderScale > 0) {
		outX = std::clamp(outX, 0, static_cast<int>(m_totalXRes) - 1);
		outY = std::clamp(outY, 0, static_cast<int>(m_totalYRes) - 1);
	}
}

} // New3D
