##
## Supermodel
## A Sega Model 3 Arcade Emulator.
## Copyright 2003-2025 The Supermodel Team
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

#
# Options.inc
#
# Build options.
#


###############################################################################
# Build Options
###############################################################################

#
# Verbose progression
#
VERBOSE =
ifneq ($(filter $(strip $(VERBOSE)),0 1),$(strip $(VERBOSE)))
	override VERBOSE =
endif
SILENT = @
ifeq ($(strip $(VERBOSE)),1)
	SILENT =
endif

#
# Enable extra debug logging and render state analyser for the legacy 3D engine
# (will slow down emulation!)
#
EXTRA_DEBUG =
ifneq ($(filter $(strip $(EXTRA_DEBUG)),0 1),$(strip $(EXTRA_DEBUG)))
	override EXTRA_DEBUG =
endif

#
# Include console-based debugger in emulator ('yes' or 'no')
#
ENABLE_DEBUGGER =
ifneq ($(filter $(strip $(ENABLE_DEBUGGER)),0 1),$(strip $(ENABLE_DEBUGGER)))
	override ENABLE_DEBUGGER =
endif

#
# Additional flags appended to the ones Supermodel constructs for itself.
#
# Rules.inc builds CFLAGS, CXXFLAGS and LDFLAGS from scratch, because they carry
# things the build cannot work without (-c, the generated include path, the
# output file). That means a CFLAGS or LDFLAGS exported by a distribution or by
# a cross build system would otherwise be silently thrown away -- and in Yocto's
# case, dropping LDFLAGS is a packaging QA failure.
#
# So anything the environment or command line supplied under those names is
# captured here and appended to the constructed flags instead of replacing them.
# EXTRA_CFLAGS / EXTRA_CXXFLAGS / EXTRA_LDFLAGS may also be set directly.
#
ifeq ($(filter undefined default,$(origin CFLAGS)),)
	EXTRA_CFLAGS := $(CFLAGS) $(EXTRA_CFLAGS)
endif
ifeq ($(filter undefined default,$(origin CXXFLAGS)),)
	EXTRA_CXXFLAGS := $(CXXFLAGS) $(EXTRA_CXXFLAGS)
endif
ifeq ($(filter undefined default,$(origin LDFLAGS)),)
	EXTRA_LDFLAGS := $(LDFLAGS) $(EXTRA_LDFLAGS)
endif

#
# Strip the linked binary. Packagers normally want this off, because the build
# system splits the debug information into its own package afterwards and cannot
# do that once the symbols are gone.
#
STRIP_BINARY = 1
ifneq ($(filter $(strip $(STRIP_BINARY)),0 1),$(strip $(STRIP_BINARY)))
	override STRIP_BINARY = 1
endif

#
# Version stamp. Normally derived from the git history. Packagers building from
# a release tarball have no .git directory and should pass their own, e.g.
# VERSION=0.3a-20260826, which is then compiled into the binary.
#
VERSION ?=
