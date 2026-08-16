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

#include "New3D.h"
#include "Vec.h"
#include <cmath>
#include <algorithm>
#include <limits>
#include <cstring>
#include <unordered_map>
#include "R3DFloat.h"
#include "Util/BitCast.h"
#include "OSD/Logger.h"

namespace New3D {

CNew3D::CNew3D(const Util::Config::Node &config, const std::string& gameName) : 
	CNew3DBase(config, gameName),
	m_vao(0),
	m_r3dShader(config),
	m_r3dScrollFog(),
	m_r3dFrameBuffers(m_renderScale > 0),
	m_aaTarget(0),
	m_ready(false)
{
	m_primType		= GL_TRIANGLES;

	if (config["QuadRendering"].ValueAs<bool>()) {
		m_primType		= GL_LINES_ADJACENCY;
	}

	m_ready = m_r3dShader.LoadShader() && m_r3dFrameBuffers.IsReady() && m_r3dScrollFog.IsReady();
	glUseProgram(0);
	if (!m_ready) {
		return;
	}

	// setup up our vertex buffer memory

	glGenVertexArrays(1, &m_vao);
	glBindVertexArray(m_vao);
	m_vbo.Create(GL_ARRAY_BUFFER, GL_DYNAMIC_DRAW, sizeof(FVertex) * (MAX_RAM_VERTS + MAX_ROM_VERTS));
	m_vbo.Bind(true);

	glEnableVertexAttribArray(m_r3dShader.GetVertexAttribPos("inVertex"));
	glEnableVertexAttribArray(m_r3dShader.GetVertexAttribPos("inNormal"));
	glEnableVertexAttribArray(m_r3dShader.GetVertexAttribPos("inTexCoord"));
	glEnableVertexAttribArray(m_r3dShader.GetVertexAttribPos("inColour"));
	glEnableVertexAttribArray(m_r3dShader.GetVertexAttribPos("inFaceNormal"));
	glEnableVertexAttribArray(m_r3dShader.GetVertexAttribPos("inFixedShade"));
	glEnableVertexAttribArray(m_r3dShader.GetVertexAttribPos("inTextureNP"));

	// before draw, specify vertex and index arrays with their offsets, offsetof is maybe evil ..
	glVertexAttribPointer(m_r3dShader.GetVertexAttribPos("inVertex"), 4, GL_FLOAT, GL_FALSE, sizeof(FVertex), 0);
	glVertexAttribPointer(m_r3dShader.GetVertexAttribPos("inNormal"), 3, GL_FLOAT, GL_FALSE, sizeof(FVertex), (void*)offsetof(FVertex, base.normal));
	glVertexAttribPointer(m_r3dShader.GetVertexAttribPos("inTexCoord"), 2, GL_FLOAT, GL_FALSE, sizeof(FVertex), (void*)offsetof(FVertex, base.texcoords));
	glVertexAttribPointer(m_r3dShader.GetVertexAttribPos("inColour"), 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(FVertex), (void*)offsetof(FVertex, faceColour));
	glVertexAttribPointer(m_r3dShader.GetVertexAttribPos("inFaceNormal"), 3, GL_FLOAT, GL_FALSE, sizeof(FVertex), (void*)offsetof(FVertex, faceNormal));
	glVertexAttribPointer(m_r3dShader.GetVertexAttribPos("inFixedShade"), 1, GL_FLOAT, GL_FALSE, sizeof(FVertex), (void*)offsetof(FVertex, base.fixedShade));
	glVertexAttribPointer(m_r3dShader.GetVertexAttribPos("inTextureNP"), 1, GL_FLOAT, GL_FALSE, sizeof(FVertex), (void*)offsetof(FVertex, textureNP));

	glBindVertexArray(0);
	m_vbo.Bind(false);
}

CNew3D::~CNew3D()
{
	m_vbo.Destroy();
	if (m_vao) {
		glDeleteVertexArrays(1, &m_vao);
		m_vao = 0;
	}

	m_r3dShader.UnloadShader();
}

void CNew3D::AttachMemory(const UINT32 *cullingRAMLoPtr, const UINT32 *cullingRAMHiPtr, const UINT32 *polyRAMPtr, const UINT32 *vromPtr, const UINT16 *textureRAMPtr)
{
	AttachMemoryPointers(cullingRAMLoPtr, cullingRAMHiPtr, polyRAMPtr, vromPtr, textureRAMPtr);

	m_textureBank[0].AttachMemory(textureRAMPtr);
	m_textureBank[1].AttachMemory(textureRAMPtr + (2048*1024));
}

Result CNew3D::Init(unsigned xOffset, unsigned yOffset, unsigned xRes, unsigned yRes, unsigned totalXResParam, unsigned totalYResParam, unsigned aaTarget)
{
	if (!m_ready) {
		return Result::FAIL;
	}

	// Resolution and offset within physical display area
	if (m_wideScreen) {
		m_outputXOffs = 0;
		m_outputYOffs = 0;
		m_outputXRes = totalXResParam;
		m_outputYRes = totalYResParam;
	}
	else {
		m_outputXOffs = xOffset;
		m_outputYOffs = yOffset;
		m_outputXRes = xRes;
		m_outputYRes = yRes;
	}

	if (m_renderScale > 0) {
		const unsigned scale = static_cast<unsigned>(m_renderScale);
		m_xRes = 496u * scale;
		m_yRes = 384u * scale;
		m_totalYRes = m_yRes;

		if (m_wideScreen && m_outputYRes > 0) {
			const unsigned long long scaledWidth =
				static_cast<unsigned long long>(m_yRes) * m_outputXRes;
			m_totalXRes = static_cast<unsigned>((scaledWidth + m_outputYRes / 2u) / m_outputYRes);
		}
		else {
			m_totalXRes = m_xRes;
		}

		m_xOffs = m_totalXRes > m_xRes ? (m_totalXRes - m_xRes) / 2u : 0u;
		m_yOffs = 0;
	}
	else {
		m_xOffs = xOffset;
		m_yOffs = yOffset;
		m_xRes = xRes;
		m_yRes = yRes;
		m_totalXRes = totalXResParam;
		m_totalYRes = totalYResParam;
	}
	m_xRatio = m_xRes * (float)(1.0 / 496.0);
	m_yRatio = m_yRes * (float)(1.0 / 384.0);
	m_aaTarget = aaTarget;

	GLint maxTextureSize = 0;
	GLint maxRenderbufferSize = 0;
	GLint maxViewportDimensions[2] = { 0, 0 };
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
	glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRenderbufferSize);
	glGetIntegerv(GL_MAX_VIEWPORT_DIMS, maxViewportDimensions);
	const bool renderTargetExceedsLimits =
		m_totalXRes > static_cast<unsigned>(maxTextureSize) ||
		m_totalYRes > static_cast<unsigned>(maxTextureSize) ||
		m_totalXRes > static_cast<unsigned>(maxRenderbufferSize) ||
		m_totalYRes > static_cast<unsigned>(maxRenderbufferSize) ||
		m_totalXRes > static_cast<unsigned>(maxViewportDimensions[0]) ||
		m_totalYRes > static_cast<unsigned>(maxViewportDimensions[1]);
	const bool outputViewportExceedsLimits = m_renderScale > 0 &&
		(m_outputXRes > static_cast<unsigned>(maxViewportDimensions[0]) ||
		 m_outputYRes > static_cast<unsigned>(maxViewportDimensions[1]));
	if (renderTargetExceedsLimits || outputViewportExceedsLimits) {
		ErrorLog("New3D dimensions exceed the OpenGL limits: render target %ux%u, output viewport %ux%u; maximum texture %d, renderbuffer %d, viewport %dx%d.",
			m_totalXRes, m_totalYRes, m_outputXRes, m_outputYRes,
			maxTextureSize, maxRenderbufferSize, maxViewportDimensions[0], maxViewportDimensions[1]);
		return Result::FAIL;
	}

	m_r3dFrameBuffers.DestroyFBO();		// remove any old ones if created

	return m_r3dFrameBuffers.CreateFBO(m_totalXRes, m_totalYRes);
}

void CNew3D::UploadTextures(unsigned level, unsigned x, unsigned y, unsigned width, unsigned height)
{
	// handle case of entire sheet invalidation
	if (width == 2048 && height == 2048) {

		height = 1024;

		static constexpr int mipXBase[] = { 0, 1024, 1536, 1792, 1920, 1984, 2016, 2032, 2040, 2044, 2046, 2047 };
		static constexpr int mipYBase[] = { 0, 512, 768, 896, 960, 992, 1008, 1016, 1020, 1022, 1023 };

		for (int i = 0; i < m_textureBank[0].GetNumberOfLevels(); i++) {
			m_textureBank[0].UploadTextures(i, mipXBase[i], mipYBase[i], width, height);
			m_textureBank[1].UploadTextures(i, mipXBase[i], mipYBase[i], width, height);
			width = (width > 1) ? width / 2 : 1;
			height = (height > 1) ? height / 2 : 1;
		}

		return;
	}

	int page;
	TranslateTexture(x, y, width, height, page);

	m_textureBank[page].UploadTextures(level, x, y, width, height);
}

void CNew3D::DrawScrollFog()
{
	// this is my best guess at the logic based upon what games are doing
	//
	// ocean hunter		- every viewport has scroll fog values set. Must start with lowest priority layers as the higher ones sometimes are garbage
	// scud race		- first viewports in priority layer missing scroll values. The latter ones all contain valid scroll values.
	// daytona			- doesn't seem to use scroll fog at all. Will set scroll values for the first viewports, the end ones contain no scroll values. End credits have scroll fog, but constrained to the viewport
	// vf3				- first viewport only has it set. But set with highest select value ?? Rest of the viewports in priority layer contain a lower select value
	// sega bassfishing	- first viewport in priority 1 sets scroll value. The rest all contain the wrong value + a higher select value ..
	// spikeout final	- 2nd viewport in the priority layer has scroll values set, none of the others do. It also uses the highest select value

	// I think the basic logic is this: the real3d picks the highest scroll fog value, starting from the lowest priority layer. 
	// If it finds a value for priority layer 0 for example, it then bails out looking for any more.
	// Fogging seems to be constrained to whatever the viewport is that is set.
	// Scroll fog needs a density or start value to work, but these can come from another viewport if the fog colour is the same

	// For fullscreen viewports I am fairly certain the fogging aspect is drawn at the mixer level, not directly to the frame buffer, otherwise alpha can't work correctly.
	// For spotlight on fog, I think it must be drawn with the 3d h/w. Think only ECA uses this, maybe ocean hunter

	Node* nodePtr = nullptr;

	for (int i = 0; i < 4 && !nodePtr; i++) {
		for (auto &n : m_nodes) {
			if (n.viewport.priority == i) {
				if (n.viewport.scrollFog > 0.f) {

					// check to see if we have a higher scroll fog value
					if (nodePtr) {
						if (nodePtr->viewport.scrollFog < n.viewport.scrollFog) {
							nodePtr = &n;
						}

						continue;
					}

					nodePtr = &n;
				}
			}
		}
	}

	if (nodePtr) {

		// iterate nodes to see if any viewports with that fog colour actually set a fog density, start or spotlight on fog
		// if all of these are zero fogging is effectively disabled

		for (auto& n : m_nodes) {

			if (nodePtr->viewport.fogColour[0] == n.viewport.fogColour[0] &&
				nodePtr->viewport.fogColour[1] == n.viewport.fogColour[1] &&
				nodePtr->viewport.fogColour[2] == n.viewport.fogColour[2])
			{

				// spotlight on fog is disabled if colour = black
				bool spotOnFogEnabled = false;
				for (auto c : n.viewport.spotFogColor) {
					if (c > 0.0f) {
						spotOnFogEnabled = true;
						break;
					}
				}

				bool fog			= n.viewport.fogDensity > 0.0f || n.viewport.fogStart > 0.0f || n.viewport.scrollAtt > 0.0f;
				bool spotlightFog	= n.viewport.fogAttenuation > 0.0f && spotOnFogEnabled;

				if (fog || spotlightFog) {

					auto& vp = nodePtr->viewport;
					float rgba[4];
					
					rgba[0] = vp.fogColour[0];
					rgba[1] = vp.fogColour[1];
					rgba[2] = vp.fogColour[2];
					rgba[3] = vp.scrollFog;

					float fogAttentuation = (fog && spotlightFog) ? 0.0f : n.viewport.fogAttenuation;	// if regular fog is enabled h/w seems to only apply this and skip the spotlight on fog (ECA)

					glViewport(vp.x, vp.y, vp.width, vp.height);

					m_r3dScrollFog.DrawScrollFog(rgba, fogAttentuation, n.viewport.fogAmbient, n.viewport.spotFogColor, n.viewport.spotEllipse);
					break;
				}
			}
		}
	}
}

void CNew3D::DrawAmbientFog()
{
	// logic here is still not totally understood
	// some games are setting fog ambient which seems to darken the 2d background layer too when scroll fogging is not set
	// The logic is something like tileGenColour * fogAmbient
	// If fogAmbient = 1.0 it's a no-op. Lower values darken the image
	// Does this work with scroll fog? Well technically scroll fog already takes into account the fog ambient as it darkens the fog colour
	// Like scroll fog I am sure this works at the mixer level, rather than drawing into the frame buffer.

	// lemans24 every viewport will set ambient fog, scroll attentuation is sometimes set (for every viewport) for explosion effects from car exhaust. So has no effect on ambient fog
	// otherwise we'll make the ambient fog flash 
	// sega rally will set ambient fog to zero for every viewport in priority layers 1-3 with a fog density set. Disabled viewports with priority zero have ambient fog disabled (1.0). Don't think srally uses ambient fog
	// vf3 almost all viewports in all priority layers have ambient fog set (<1.0)
	// lost world is setting an ambient fog value every every viewport but has no density or fog start value set. Don't think lost world is using ambient fog

	// Let's pick the lowest fog ambient value from only the first priority layer
	// Check for fog density or a fog start value, otherwise the effect seems to be disabled (lost world)

	float fogAmbient = 1.0f;
	Node* nodePtr = nullptr;

	for (int i = 0; i < 4; i++) {

		bool hasPriority = false;

		for (auto& n : m_nodes) {

			auto& vp = n.viewport;

			if (vp.priority == i) {

				hasPriority = true;
				
				if (vp.scrollFog > 0.0f) {
					continue;
				}

				// if we don't have a fog density or fog start skip this viewport
				if (vp.fogDensity <= 0.0f && vp.fogStart <= 0.0f) {
					continue;
				}

				if (vp.fogAmbient < fogAmbient) {
					nodePtr = &n;
					fogAmbient = vp.fogAmbient;
				}
			}
		}

		if (nodePtr || hasPriority) {
			break;
		}

	}

	if (nodePtr) {
		auto& vp = nodePtr->viewport;
		float rgba[] = { 0.0f, 0.0f, 0.0f, 1.0f - fogAmbient };
		glViewport(vp.x, vp.y, vp.width, vp.height);
		m_r3dScrollFog.DrawScrollFog(rgba, 0.0f, 1.0f, vp.spotFogColor, vp.spotEllipse); // we assume spot light is not used
	}
}

bool CNew3D::RenderScene(int priority, bool renderOverlay, Layer layer)
{
	glActiveTexture(GL_TEXTURE0);
	m_textureBank[0].Bind();
	glActiveTexture(GL_TEXTURE1);
	m_textureBank[1].Bind();
	glActiveTexture(GL_TEXTURE0);

	bool hasOverlay = false;		// (high priority polys)

	for (auto &n : m_nodes) {

		if (n.viewport.priority != priority || n.models.empty()) {
			continue;
		}

		CalcViewport(&n.viewport);
		glViewport(n.viewport.x, n.viewport.y, n.viewport.width, n.viewport.height);

		m_r3dShader.SetViewportUniforms(&n.viewport);

		for (auto &m : n.models) {

			bool matrixLoaded = false;

			if (m.meshes->empty()) {
				continue;
			}

			for (auto &mesh : *m.meshes) {

				if (mesh.highPriority) {
					hasOverlay = true;
				}

				if (!mesh.Render(layer, m.alpha)) continue;
				if (mesh.highPriority != renderOverlay) continue;

				if (!matrixLoaded) {
					m_r3dShader.SetModelStates(&m);
					matrixLoaded = true;		// do this here to stop loading matrices we don't need. Ie when rendering non transparent etc
				}
#ifdef SUPERMODEL_GLES
				if (layer == Layer::colour) {
					m_r3dFrameBuffers.SetLosFlagWrite(!mesh.layered);
				}
#endif
				
				m_r3dShader.SetMeshUniforms(&mesh);
				glDrawArrays(m_primType, mesh.vboOffset, mesh.vertexCount);
			}
		}
	}

	return hasOverlay;
}

void CNew3D::SetRenderStates()
{
	m_vbo.Bind(true);
	glBindVertexArray(m_vao);

	m_r3dShader.SetShader(true);

	glDepthFunc		(GL_GEQUAL);
	glEnable		(GL_DEPTH_TEST);
	glDepthMask		(GL_TRUE);
	glActiveTexture	(GL_TEXTURE0);
	glDisable		(GL_CULL_FACE);					// we'll emulate this in the shader		

	glEnable		(GL_STENCIL_TEST);
	glStencilMask	(0xFF);

	glBlendFunc		(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable		(GL_BLEND);
}

void CNew3D::DisableRenderStates()
{
	m_vbo.Bind(false);
	glBindVertexArray(0);

	m_r3dShader.SetShader(false);

	glDisable(GL_STENCIL_TEST);
}

void CNew3D::PrepareFrame(void)
{
	PrepareScene();
}

void CNew3D::RenderFrame(void)
{
	if (!ScenePrepared()) {
		PrepareFrame();							// not hoisted this frame, so do it now
	}
	ClearScenePrepared();

	if (m_blockCullingLatched)					// block culling disables 3D rendering
	{
		if (m_aaTarget) {
			glBindFramebuffer(GL_FRAMEBUFFER, m_aaTarget);		// if we have an AA target draw to it instead of the default back buffer
		}

		// clear screen to white
		glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		
		if (m_aaTarget) {
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}

		return;
	}

	m_vbo.Bind(true);
	m_vbo.BufferSubData(MAX_ROM_VERTS*sizeof(FVertex), m_polyBufferRam.size()*sizeof(FVertex), m_polyBufferRam.data());	// upload all the dynamic data to GPU in one go

	if (!m_polyBufferRom.empty()) {

		// sync rom memory with vbo
		int romBytes	= (int)(m_polyBufferRom.size() * sizeof(FVertex));
		int vboBytes	= m_vbo.GetSize();
		int size		= romBytes - vboBytes;

		if (size) {
			//check we haven't blown up the memory buffers
			//we will lose rom models for 1 frame if this happens, not the end of the world, as probably won't ever happen anyway
			if (m_polyBufferRom.size() >= MAX_ROM_VERTS) {
				m_polyBufferRom.clear();
				m_romMap.clear();
				m_vbo.Reset();
			}
			else {
				m_vbo.AppendData(size, &m_polyBufferRom[vboBytes / sizeof(FVertex)]);
			}
		}
	}

	const bool restoreScissor = m_renderScale > 0 && glIsEnabled(GL_SCISSOR_TEST);
	if (restoreScissor) {
		glDisable(GL_SCISSOR_TEST);
	}
	m_r3dFrameBuffers.SetFBO(Layer::colour);		// colour will draw to all buffers. For regular opaque pixels the transparent layers will be essentially masked
#ifdef SUPERMODEL_GLES
	const GLfloat clearColour[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	glClearBufferfv(GL_COLOR, 0, clearColour);
	glClearBufferfv(GL_COLOR, 1, clearColour);
	glClearBufferfv(GL_COLOR, 2, clearColour);
#else
	glClear(GL_COLOR_BUFFER_BIT);
#endif

	DrawAmbientFog();
	DrawScrollFog();								// fog layer if applicable must be drawn here

	for (int pri = 0; pri <= 3; pri++) {

		if (SkipLayer(pri)) continue;

		for (int i = 0; i < 2; i++) {

			bool renderOverlay = (i == 1);

			SetRenderStates();

			m_r3dFrameBuffers.SetFBO(Layer::colour);

#ifdef SUPERMODEL_GLES
			glClearDepthf(0.0f);
#else
			glClearDepth(0.0);
#endif
			glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
#ifdef SUPERMODEL_GLES
			if (!renderOverlay) {
				m_r3dFrameBuffers.PrepareLos();
			}
#endif

			m_r3dShader.DiscardAlpha(true);
			m_r3dShader.SetLayer(Layer::colour);
			bool hasOverlay = RenderScene(pri, renderOverlay, Layer::colour);

			if (!renderOverlay) {
				ProcessLos(pri);
			}

			glDepthFunc(GL_GREATER);

			m_r3dShader.DiscardAlpha(false);

			m_r3dFrameBuffers.StoreDepth();
			m_r3dShader.SetLayer(Layer::trans1);
			m_r3dFrameBuffers.SetFBO(Layer::trans1);
			RenderScene(pri, renderOverlay, Layer::trans1);

			m_r3dFrameBuffers.RestoreDepth();
			m_r3dShader.SetLayer(Layer::trans2);
			m_r3dFrameBuffers.SetFBO(Layer::trans2);
			RenderScene(pri, renderOverlay, Layer::trans2);
						
			DisableRenderStates();

			if (!hasOverlay) break;								// no high priority polys						
		}
	}

	if (restoreScissor) {
		glEnable(GL_SCISSOR_TEST);
	}
	m_r3dFrameBuffers.SetFBO(Layer::none);

	if (m_aaTarget) {
		glBindFramebuffer(GL_FRAMEBUFFER, m_aaTarget);			// if we have an AA target draw to it instead of the default back buffer
	}

	if (m_renderScale > 0) {
		m_r3dFrameBuffers.DrawScaled(m_outputXOffs, m_outputYOffs, m_outputXRes, m_outputYRes);
	}
	else {
		m_r3dFrameBuffers.Draw();
	}

	if (m_aaTarget) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
}

void CNew3D::BeginFrame(void)
{
}

void CNew3D::EndFrame(void)
{
}

bool CNew3D::ProcessLos(int priority)
{
	for (const auto &n : m_nodes) {
		if (n.viewport.priority == priority) {
			if (n.viewport.losPosX || n.viewport.losPosY) {

				int losX, losY;
				TranslateLosPosition(n.viewport.losPosX, n.viewport.losPosY, losX, losY);

#ifdef SUPERMODEL_GLES
				float depth;
				bool noLosReturn;
				m_r3dFrameBuffers.ReadLos(losX, losY, depth, noLosReturn);
				GLubyte stencilVal = noLosReturn ? 0x80 : 0;
				float zVal = depth / NEAR_PLANE;
#else
				float range[2];
				glReadPixels(losX, losY, 1, 1, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, range);
				GLubyte stencilVal = Util::FloatAsInt32(range[1]);

				float zVal = range[0] / NEAR_PLANE;
#endif

				// apply our mask to stencil, because layered poly attributes use the lower bits
				stencilVal &= 0x80;

				// if the stencil val is zero that means we've hit sky or whatever, if it hits a 1 we've hit geometry
				// the real3d returns 1 in the top bit of the float if the line of sight test passes (ie doesn't hit geometry)

				auto zValP = reinterpret_cast<unsigned char*>(&zVal);	// this is legal in c++, casting to int technically isn't

				if (stencilVal == 0) {
					zValP[0] |= 1;		// set first bit to 1
				}
				else {
					zValP[0] &= 0xFE;	// set first bit to zero
				}

				m_losBack->value[priority] = zVal;

				return true;
			}
		}
	}

	return false;
}

} // New3D
