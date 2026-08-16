##
## Supermodel
## A Sega Model 3 Arcade Emulator.
## Copyright 2003-2026 The Supermodel Team
##
## This file is part of Supermodel.
##
## Supermodel is free software: you can redistribute it and/or modify it under
## the terms of the GNU General Public License as published by the Free
## Software Foundation, either version 3 of the License, or (at your option)
## any later version.
##
## Supermodel is distributed in the hope that it will be useful, but WITHOUT
## ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
## FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
## more details.
##
## You should have received a copy of the GNU General Public License along
## with Supermodel.  If not, see <http://www.gnu.org/licenses/>.
##
###############################################################################
# Vulkan Shaders
#
# Every shader in Src/Graphics/Vulkan/Shaders/ is compiled to SPIR-V for
# vulkan1.1 and embedded into a generated header as a uint32_t array, so the
# renderer needs no shader files at run time and no GLSL compiler on the target
# machine.
#
# Include this after Makefiles/Rules.inc, which is what defines $(OBJ_DIR), and
# only when the Vulkan backend is being built. Nothing here depends on a
# particular GL flavour, so it combines freely with desktop OpenGL or GLES.
#
# The including makefile may set:
#   VULKAN_SHADER_GENERATOR  name recorded in the generated headers
#   GLSLANG / GLSLC          explicit path to a shader compiler
###############################################################################

VULKAN_SHADER_GENERATOR ?= Makefiles/VulkanShaders.inc

VULKAN_SHADER_DIR = Src/Graphics/Vulkan/Shaders

#
# Shaders built once. The stage is taken from the file extension, and the
# generated header is $(OBJ_DIR)/<source name>.spv.h. R3DShaderUniforms.glsl and
# R3DShaderCommon.glsl are deliberately absent: they are #included by the
# shaders below rather than compiled on their own.
#
VULKAN_SHADER_SRCS = \
	R3DTriangle.vert \
	R3DTriangle.frag \
	R3DQuad.vert \
	R3DQuad.geom \
	R3DQuad.frag \
	FullScreenQuad.vert \
	FullScreenQuadUV.vert \
	R3DFrameBufferBase.frag \
	R3DFrameBufferTrans.frag \
	R3DFrameBufferBaseScaled.frag \
	R3DFrameBufferTransScaled.frag \
	ScrollFog.frag \
	PostProcess.frag \
	Tilegen.vert \
	Crosshair.vert \
	Crosshair.frag

#
# Tilegen.frag is built once per UpscaleMode, mirroring the GL renderer which
# prepended "#define UPSCALEMODE n" and recompiled. 0 nearest, 1 biquintic,
# 2 bilinear, 3 bicubic.
#
VULKAN_TILEGEN_UPSCALE_MODES = 0 1 2 3

VULKAN_SHADER_HEADERS = \
	$(foreach shader,$(VULKAN_SHADER_SRCS),$(OBJ_DIR)/$(shader).spv.h) \
	$(foreach mode,$(VULKAN_TILEGEN_UPSCALE_MODES),$(OBJ_DIR)/Tilegen.upscale$(mode).frag.spv.h)

#
# Convenience umbrella header that pulls in every generated module.
#
VULKAN_SHADER_INDEX = $(OBJ_DIR)/VulkanShaderSPIRV.h

#
# Shader compiler discovery. glslang is preferred because it is the reference
# front end; glslc (shaderc, shipped with the Vulkan SDK) is the fallback. Both
# understand -I, -D and GL_GOOGLE_include_directive.
#
# glslang 15 renamed the binary from glslangValidator to glslang and installs
# the old name only as a compatibility symlink, which some distributions and
# the official release archives omit. Accept either name, newest first, so the
# build works with both current and older toolchains. GLSLANG and GLSLC may
# also be set on the make command line to point at a specific binary.
#
GLSLANG ?=
GLSLC ?=
VULKAN_GLSLANG := $(strip $(GLSLANG))
ifeq ($(VULKAN_GLSLANG),)
	VULKAN_GLSLANG := $(shell command -v glslang 2>/dev/null || command -v glslangValidator 2>/dev/null)
endif
VULKAN_GLSLC := $(strip $(GLSLC))
ifeq ($(VULKAN_GLSLC),)
	VULKAN_GLSLC := $(shell command -v glslc 2>/dev/null)
endif

#
# Compile one shader and wrap it in a header.
#
# $(1) - extra -D flags for this variant
#
# Warnings are errors. glslc has -Werror; glslangValidator does not, so the
# diagnostics are captured and the build is failed if any of them mention a
# warning. That keeps the behaviour identical whichever compiler is present.
#
# The word extraction uses od in native byte order, which is what a SPIR-V
# module written by the compiler on this host already is.
#
define VULKAN_COMPILE_SHADER
	$(info Compiling shader       : $< -> $@)
	$(SILENT)set -e; \
	spv='$(@:.h=)'; \
	log='$@.log'; \
	symbol=`basename '$@' .spv.h | tr '.-' '__'`; \
	if [ -n "$(VULKAN_GLSLANG)" ]; then \
	  set -- $(VULKAN_GLSLANG) -V --target-env vulkan1.1 -I$(VULKAN_SHADER_DIR) $(1) -o "$$spv" '$<'; \
	elif [ -n "$(VULKAN_GLSLC)" ]; then \
	  set -- $(VULKAN_GLSLC) --target-env=vulkan1.1 -Werror -I$(VULKAN_SHADER_DIR) $(1) -o "$$spv" '$<'; \
	else \
	  echo "" 1>&2; \
	  echo "*** No Vulkan shader compiler found." 1>&2; \
	  echo "*** The Vulkan backend needs glslang, glslangValidator or glslc on PATH to" 1>&2; \
	  echo "*** turn $(VULKAN_SHADER_DIR)/*.{vert,geom,frag} into SPIR-V." 1>&2; \
	  echo "***" 1>&2; \
	  echo "***   Debian/Ubuntu : sudo apt install glslang-tools" 1>&2; \
	  echo "***   Fedora        : sudo dnf install glslang" 1>&2; \
	  echo "***   Arch          : sudo pacman -S glslang" 1>&2; \
	  echo "***   Homebrew      : brew install glslang" 1>&2; \
	  echo "***   Or install the LunarG Vulkan SDK, which provides both." 1>&2; \
	  echo "***" 1>&2; \
	  echo "*** Build without VULKAN=1 to skip the Vulkan backend entirely." 1>&2; \
	  echo "" 1>&2; \
	  exit 1; \
	fi; \
	if ! "$$@" > "$$log" 2>&1; then \
	  echo "*** Shader compilation failed: $<" 1>&2; \
	  cat "$$log" 1>&2; \
	  rm -f "$$log" "$$spv"; \
	  exit 1; \
	fi; \
	if grep -i -q 'warning' "$$log"; then \
	  echo "*** Shader warnings are errors: $<" 1>&2; \
	  cat "$$log" 1>&2; \
	  rm -f "$$log" "$$spv"; \
	  exit 1; \
	fi; \
	rm -f "$$log"; \
	{ \
	  echo "// Generated by $(VULKAN_SHADER_GENERATOR). Do not edit."; \
	  echo "// Source: $< $(1)"; \
	  echo "#pragma once"; \
	  echo "#include <cstdint>"; \
	  echo "inline constexpr uint32_t $$symbol[] = {"; \
	  od -An -v -tx4 "$$spv" | tr -s ' ' '\n' | sed -e '/^$$/d' -e 's/^/0x/' -e 's/$$/u,/'; \
	  echo "};"; \
	  echo "inline constexpr uint32_t $${symbol}WordCount = sizeof($$symbol) / sizeof(uint32_t);"; \
	} > '$@'; \
	rm -f "$$spv"
endef

#
# Shared #include-only sources. Listing all of them as prerequisites of every
# shader is conservative but keeps the dependency information correct without
# parsing #include lines.
#
VULKAN_SHADER_INCLUDES = $(wildcard $(VULKAN_SHADER_DIR)/*.glsl)

$(OBJ_DIR)/%.vert.spv.h: $(VULKAN_SHADER_DIR)/%.vert $(VULKAN_SHADER_INCLUDES) | $(OBJ_DIR)
	$(call VULKAN_COMPILE_SHADER,)

$(OBJ_DIR)/%.geom.spv.h: $(VULKAN_SHADER_DIR)/%.geom $(VULKAN_SHADER_INCLUDES) | $(OBJ_DIR)
	$(call VULKAN_COMPILE_SHADER,)

$(OBJ_DIR)/%.frag.spv.h: $(VULKAN_SHADER_DIR)/%.frag $(VULKAN_SHADER_INCLUDES) | $(OBJ_DIR)
	$(call VULKAN_COMPILE_SHADER,)

#
# One rule per UpscaleMode. Each writes Tilegen.upscale<n>.frag.spv.h from the
# single Tilegen.frag source. Declared explicitly so that the pattern rules above
# never match these outputs.
#
define VULKAN_TILEGEN_RULE
$$(OBJ_DIR)/Tilegen.upscale$(1).frag.spv.h: $$(VULKAN_SHADER_DIR)/Tilegen.frag $$(VULKAN_SHADER_INCLUDES) | $$(OBJ_DIR)
	$$(call VULKAN_COMPILE_SHADER,-DUPSCALEMODE=$(1))
endef

$(foreach mode,$(VULKAN_TILEGEN_UPSCALE_MODES),$(eval $(call VULKAN_TILEGEN_RULE,$(mode))))

$(VULKAN_SHADER_INDEX): $(VULKAN_SHADER_HEADERS) | $(OBJ_DIR)
	$(info Generating shader index: $@)
	$(SILENT){ \
	  echo "// Generated by $(VULKAN_SHADER_GENERATOR). Do not edit."; \
	  echo "#pragma once"; \
	  for header in $(notdir $(VULKAN_SHADER_HEADERS)); do \
	    echo "#include \"$$header\""; \
	  done; \
	} > $@

#
# The generated headers are prerequisites of the Vulkan translation units, and
# $(OBJ_DIR) has to be on their include path. Main.cpp is included because it
# constructs the renderer bundle and therefore includes VulkanPipelines.h through
# VulkanRenderers.h.
#
VULKAN_OBJ_FILES = \
	$(OBJ_DIR)/VulkanContext.o \
	$(OBJ_DIR)/VulkanRenderers.o \
	$(OBJ_DIR)/VulkanResources.o \
	$(OBJ_DIR)/VulkanTextureBank.o \
	$(OBJ_DIR)/VulkanRenderTargets.o \
	$(OBJ_DIR)/VulkanPipelines.o \
	$(OBJ_DIR)/VulkanNew3D.o \
	$(OBJ_DIR)/VulkanComposite.o \
	$(OBJ_DIR)/VulkanRender2D.o \
	$(OBJ_DIR)/VulkanPostProcess.o \
	$(OBJ_DIR)/VulkanCrosshair.o \
	$(OBJ_DIR)/Main.o

$(VULKAN_OBJ_FILES): $(VULKAN_SHADER_INDEX) $(VULKAN_SHADER_HEADERS)
$(VULKAN_OBJ_FILES): CXXFLAGS += -I$(OBJ_DIR)

.PHONY: vulkan-shaders
vulkan-shaders: $(VULKAN_SHADER_INDEX)
