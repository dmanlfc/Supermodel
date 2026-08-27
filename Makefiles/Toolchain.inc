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
# Toolchain.inc
#
# Toolchain selection, shared by every platform makefile.
#
# This file exists so that a cross compiling build system -- Buildroot, Yocto,
# OpenEmbedded, a hand rolled toolchain -- can drive the build entirely through
# the environment or the make command line, without patching any makefile.
#
# Each platform makefile sets TOOLCHAIN_DEFAULT_CC / _CXX / _LD to whatever is
# correct for a native build on that platform and then includes this file.
#

#
# Compilers for the *target* machine.
#
# $(origin VAR) is 'default' when the value is one make invented for itself
# (CC = cc, CXX = g++, LD = ld, AR = ar ...). Those are the only values we
# replace. An 'environment' or 'command line' origin is a deliberate choice by
# whoever started the build and has to survive, which is exactly what lets
#
#     export CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++
#     make
#
# work with no further arguments.
#
ifeq ($(origin CC),default)
	CC = $(TOOLCHAIN_DEFAULT_CC)
endif
ifeq ($(origin CXX),default)
	CXX = $(TOOLCHAIN_DEFAULT_CXX)
endif

#
# The link driver.
#
# LD here is a compiler front end -- something that knows where the C runtime,
# the startup files and the default library search paths live -- and not
# binutils ld. Cross build environments conventionally export LD as the raw
# linker (LD=aarch64-linux-gnu-ld), which cannot link a C++ program, so an
# inherited environment LD is deliberately ignored. Pass LD= on the make command
# line if you really do need to choose the link driver yourself.
#
ifneq ($(filter default environment,$(origin LD)),)
	LD = $(TOOLCHAIN_DEFAULT_LD)
endif

#
# Tools that build things which run on the *build* machine rather than on the
# target. Supermodel has exactly one of these: m68kmake, the Musashi 68K source
# generator, which is compiled and then executed during the build.
#
# CC_FOR_BUILD is the spelling used by Autotools and Buildroot; HOSTCC (kernel,
# Buildroot) and BUILD_CC (Yocto / OpenEmbedded) are accepted as synonyms, so
# the common cross environments are already handled by what they export.
#
ifeq ($(strip $(CC_FOR_BUILD)),)
	ifneq ($(strip $(HOSTCC)),)
		CC_FOR_BUILD := $(HOSTCC)
	else ifneq ($(strip $(BUILD_CC)),)
		CC_FOR_BUILD := $(BUILD_CC)
	endif
endif

#
# Nothing named a build machine compiler, so work out whether one is even
# needed: ask both compilers what they target and compare. Getting this wrong is
# unpleasant to diagnose -- the build fails deep inside the 68K step with
# "cannot execute binary file" -- so it is worth detecting rather than leaving
# to the person invoking make.
#
# When the two agree, or when the question cannot be answered because there is
# no native compiler to ask, fall back to the target compiler, which is both the
# historical behaviour and the right answer for every native build.
#
ifeq ($(strip $(CC_FOR_BUILD)),)
	TOOLCHAIN_BUILD_MACHINE := $(shell $(TOOLCHAIN_DEFAULT_CC) -dumpmachine 2>/dev/null)
	TOOLCHAIN_TARGET_MACHINE := $(shell $(CC) -dumpmachine 2>/dev/null)
	ifeq ($(strip $(TOOLCHAIN_TARGET_MACHINE)),$(strip $(TOOLCHAIN_BUILD_MACHINE)))
		CC_FOR_BUILD := $(CC)
	else ifneq ($(strip $(TOOLCHAIN_BUILD_MACHINE)),)
		CC_FOR_BUILD := $(TOOLCHAIN_DEFAULT_CC)
	else
		CC_FOR_BUILD := $(CC)
	endif
endif

#
# Flags for the build machine compiler. Deliberately *not* derived from CFLAGS:
# the target's -march, --sysroot, include paths and warning settings have no
# meaning for a program that has to run here.
#
CFLAGS_FOR_BUILD ?=
LDFLAGS_FOR_BUILD ?=

#
# pkg-config is how library flags are discovered. Cross build systems ship a
# wrapper that rewrites every path into the sysroot, so pointing this at that
# wrapper -- or just letting the environment's PKG_CONFIG through -- is all that
# is needed to keep the build machine's own libraries out of a cross build.
#
PKG_CONFIG ?= pkg-config
