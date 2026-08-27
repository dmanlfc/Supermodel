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

#
# Makefile
#
# Top level makefile. Works out which platform makefile to use and hands the
# build over to it, so that
#
#     make
#     make install DESTDIR=/tmp/stage PREFIX=/usr
#
# do the right thing from a fresh checkout. This matters most to packaging
# recipes -- Buildroot, Yocto, a distribution spec file -- which expect a
# makefile in the top level directory and would otherwise have to copy one
# there before they can build anything.
#
# The choice can be forced with SUPERMODEL_PLATFORM=UNIX|OSX|Win32, which is
# what a mingw-w64 cross build from Linux needs.
#
# Everything else -- build options, toolchain, install paths -- belongs to the
# platform makefile and is simply passed through. See Docs/CROSS-COMPILING.md.
#

UNAME_S := $(shell uname -s 2>/dev/null)

ifeq ($(UNAME_S),Darwin)
	SUPERMODEL_PLATFORM ?= OSX
else ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
	SUPERMODEL_PLATFORM ?= Win32
else ifeq ($(strip $(UNAME_S)),)
	# No uname at all: a Windows Command Prompt build
	SUPERMODEL_PLATFORM ?= Win32
else
	SUPERMODEL_PLATFORM ?= UNIX
endif

PLATFORM_MAKEFILE = Makefiles/Makefile.$(SUPERMODEL_PLATFORM)

#
# Targets forwarded to the platform makefile. Listed explicitly rather than
# caught with a `%' rule so that a mistyped goal is still reported as one.
#
FORWARDED_TARGETS = \
	all \
	clean \
	install \
	uninstall \
	version \
	set_version \
	print_message \
	release \
	pkg \
	vulkan-shaders

.PHONY: $(FORWARDED_TARGETS)
.DEFAULT_GOAL := all

#
# The `+' hands this make's job server to the sub-make, so -jN still works.
# .NOTPARALLEL applies to this makefile only: two goals named on one command
# line (`make clean all') must not run at the same time, while the sub-make
# itself remains free to build in parallel.
#
.NOTPARALLEL:

$(FORWARDED_TARGETS):
	+@$(MAKE) -f $(PLATFORM_MAKEFILE) $@
