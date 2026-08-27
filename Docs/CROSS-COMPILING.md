# Cross compiling Supermodel

Supermodel's UNIX makefile is written so that a cross build needs no patching:
point the toolchain variables at your cross compiler, point `PKG_CONFIG` at your
sysroot, and build. This document explains the variables, then gives worked
examples for a plain toolchain, Buildroot and Yocto/OpenEmbedded.

```sh
export CC=aarch64-linux-gnu-gcc
export CXX=aarch64-linux-gnu-g++
export PKG_CONFIG=aarch64-linux-gnu-pkg-config
make -j$(nproc)
make install DESTDIR=/path/to/rootfs PREFIX=/usr
```

## The one thing that makes cross compiling special

Supermodel does not just compile source files. It also builds and **runs** a
program during the build: `m68kmake`, the Musashi 68K code generator, which
writes the 68K interpreter's source into the object directory before the rest of
the build can proceed.

That program has to run on the machine doing the build, not on the target. It is
therefore compiled with `CC_FOR_BUILD` rather than `CC`, and with none of the
target's flags. Get this wrong and the build fails deep inside the 68K step with
`cannot execute binary file`, which is not an obvious diagnosis.

You should not have to think about it. If `CC_FOR_BUILD` is not set, Supermodel
looks at `HOSTCC` (Buildroot, kernel) and `BUILD_CC` (Yocto/OpenEmbedded), and
failing those it asks both compilers what they target (`-dumpmachine`) and
compares. When they differ it is a cross build and the platform's native
compiler is used for the generator; when they agree, or when there is no native
compiler to ask, it falls back to `CC` exactly as before. So

```sh
export CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++
make
```

is enough. Set `CC_FOR_BUILD` explicitly if you want a specific build machine
compiler, or if the detection cannot see one.

## Variables

### Toolchain

| Variable | Default | Notes |
|---|---|---|
| `CC` | `gcc` | Target C compiler. An environment or command line value always wins. |
| `CXX` | `g++` | Target C++ compiler. |
| `LD` | `$(CC)` | The **link driver**, i.e. a compiler front end -- not binutils `ld`. An inherited environment `LD` is deliberately ignored, because cross environments conventionally export it as the raw linker, which cannot link a C++ program. Pass it on the make command line if you really need to change it. |
| `CC_FOR_BUILD` | autodetected | Compiler for programs that run on the build machine. Taken from `HOSTCC` or `BUILD_CC` if either is set, otherwise detected by comparing `-dumpmachine` output, falling back to `$(CC)`. |
| `CFLAGS_FOR_BUILD` | empty | Flags for the above. Deliberately not derived from `CFLAGS`. |
| `LDFLAGS_FOR_BUILD` | empty | Link flags for the above. |
| `PKG_CONFIG` | `pkg-config` | Use your toolchain's sysroot aware wrapper here. |

### Flags

`CFLAGS`, `CXXFLAGS` and `LDFLAGS` are constructed by the build -- they carry
`-c`, the generated include path and the output file -- so they cannot simply be
replaced. Anything you supply under those names from the environment or the
command line is **appended** to what the build constructs, which is what lets a
distribution's hardening and optimisation flags through and keeps Yocto's
packaging QA happy. `EXTRA_CFLAGS`, `EXTRA_CXXFLAGS` and `EXTRA_LDFLAGS` do the
same thing explicitly.

| Variable | Default | Notes |
|---|---|---|
| `ARCH` | empty | Architecture tuning flags, e.g. `-mcpu=cortex-a76`. |
| `OPT` | `-O3` | Optimisation level. |
| `STRIP_BINARY` | `1` | `0` leaves the symbols in place so the build system can split the debug information into its own package. Packagers normally want `0`. |
| `VERSION` | from git | Set it when building from a tarball, e.g. `VERSION=0.3a-20260826`, otherwise there is no git history to read and the binary is stamped with nothing. |

### Libraries

Each of these is discovered through `pkg-config` where possible and can be
overridden outright. Override one of them rather than patching the link line
when a target spells a library differently -- no `libGLU`, a split `libOpenGL`,
`zlib-ng`, and so on.

| Variable | Default |
|---|---|
| `SDL2_CFLAGS`, `SDL2_LIBS` | `pkg-config sdl2`, falling back to `$(SDL2_CONFIG)` |
| `SDL2_CONFIG` | `sdl2-config` (fallback only -- see below) |
| `SDL2_NET_LIBS` | `pkg-config SDL2_net`, falling back to `-lSDL2_net` |
| `GL_LIBS` | `-lGL -lGLU` |
| `GLES_LIBS` | `-lGLESv2` |
| `VULKAN_LIBS` | `-lvulkan` |
| `ZLIB_LIBS` | `pkg-config zlib`, falling back to `-lz` |
| `SYSTEM_LIBS` | `-lm -lstdc++ -lpthread` |

**Prefer `pkg-config` over `sdl2-config`.** `sdl2-config` bakes in the prefix it
was configured with and knows nothing about a sysroot, so in a cross build it
returns the build machine's `-I/usr/include` and `-L/usr/lib`. That either fails
to link or, worse, links successfully against the wrong libraries. Supermodel
uses `sdl2-config` only when no `sdl2.pc` can be found.

### Build options

`GLES=1`, `VULKAN=1`, `WAYLAND=1`, `EVDEV=0|1`, `DEBUG=1`, `ENABLE_DEBUGGER=1`
and `VERBOSE=1` work exactly as they do for a native build. Two notes for cross
builds:

- `EVDEV` is autodetected through `$(PKG_CONFIG)`, so it correctly looks in the
  sysroot rather than on the build machine. Set `EVDEV=1` to turn a missing
  `libudev` into a build error instead of a silent downgrade, or `EVDEV=0` to
  leave the evdev light gun support out.
- `VULKAN=1` needs a **build machine** shader compiler (`glslang`,
  `glslangValidator` or `glslc`); SPIR-V is architecture independent, so the
  target needs no shader compiler at run time. Point `GLSLANG` or `GLSLC` at a
  specific binary if the wrong one is picked up.

### Install paths

The GNU names, with the GNU defaults:

| Variable | Default |
|---|---|
| `DESTDIR` | empty |
| `PREFIX` | `/usr/local` |
| `BINDIR` | `$(PREFIX)/bin` |
| `DATADIR` | `$(PREFIX)/share/supermodel` |
| `CONFIGDIR` | `$(DATADIR)/Config` |
| `ASSETSDIR` | `$(DATADIR)/Assets` |

`make install` puts the binary in `BINDIR`, `Games.xml`, `Music.xml` and
`Supermodel.ini` in `CONFIGDIR`, and the crosshair bitmaps in `ASSETSDIR`.

Supermodel finds those at run time through its path overrides, so a frontend
should point them at the installed locations with `-config-path` and
`-assets-path`, with the `SUPERMODEL_CONFIG_PATH` and `SUPERMODEL_ASSETS_PATH`
environment variables, or with `ConfigPath` and `AssetsPath` in `Supermodel.ini`.

## Example: a plain aarch64 toolchain (Raspberry Pi 5)

```sh
export CC=aarch64-linux-gnu-gcc
export CXX=aarch64-linux-gnu-g++
export PKG_CONFIG_SYSROOT_DIR=/path/to/sysroot
export PKG_CONFIG_LIBDIR=/path/to/sysroot/usr/lib/pkgconfig:/path/to/sysroot/usr/share/pkgconfig

make -j$(nproc) GLES=1 VULKAN=1 ARCH="-mcpu=cortex-a76" STRIP_BINARY=0
make install DESTDIR=/path/to/sysroot PREFIX=/usr
```

`-mcpu=cortex-a76` is the Pi 5. Use `-mcpu=cortex-a72` for a Pi 4. `GLES=1`
avoids desktop OpenGL, which is the right choice on most Arm SBCs.

## Example: Buildroot

With the changes above, the package makefile no longer needs to rewrite the
build system with `sed`, and no `cross-compile.patch` is required.

```make
################################################################################
#
# supermodel
#
################################################################################

SUPERMODEL_VERSION = <commit>
SUPERMODEL_SITE = $(call github,trzy,Supermodel,$(SUPERMODEL_VERSION))
SUPERMODEL_DEPENDENCIES = sdl2 sdl2_net zlib
SUPERMODEL_LICENSE = GPL-3.0+
SUPERMODEL_LICENSE_FILES = Docs/LICENSE.txt

ifeq ($(BR2_PACKAGE_BATOCERA_GLES3),y)
SUPERMODEL_DEPENDENCIES += libgles
SUPERMODEL_CONF_OPTS += GLES=1
else
SUPERMODEL_DEPENDENCIES += libglew libglu
endif

ifeq ($(BR2_PACKAGE_WAYLAND),y)
SUPERMODEL_DEPENDENCIES += wayland
SUPERMODEL_CONF_OPTS += WAYLAND=1
endif

ifeq ($(BR2_PACKAGE_BATOCERA_VULKAN),y)
SUPERMODEL_DEPENDENCIES += vulkan-headers vulkan-loader
SUPERMODEL_CONF_OPTS += VULKAN=1
endif

define SUPERMODEL_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) $(MAKE) -C $(@D) \
		ARCH="$(BR2_TARGET_OPTIMIZATION)" \
		STRIP_BINARY=0 \
		VERSION="$(SUPERMODEL_VERSION)" \
		$(SUPERMODEL_CONF_OPTS)
endef

define SUPERMODEL_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) install \
		DESTDIR="$(TARGET_DIR)" PREFIX=/usr
endef

$(eval $(generic-package))
```

`TARGET_CONFIGURE_OPTS` exports `CC`, `CXX` and `PKG_CONFIG`, all of which are
honoured. It also exports `LD` as the raw linker, which is ignored on purpose --
see the toolchain table above. Nothing needs to name a build machine compiler:
Buildroot exports `HOSTCC`, and the detection described earlier would find one
anyway.

`VULKAN=1` additionally needs `host-glslang` (or another SPIR-V compiler) in
`SUPERMODEL_DEPENDENCIES`.

## Example: Yocto / OpenEmbedded

```bitbake
SUMMARY = "Sega Model 3 arcade emulator"
LICENSE = "GPL-3.0-or-later"
LIC_FILES_CHKSUM = "file://Docs/LICENSE.txt;md5=<sum>"

SRC_URI = "git://github.com/trzy/Supermodel.git;branch=master;protocol=https"
SRCREV = "<commit>"
S = "${WORKDIR}/git"

DEPENDS = "libsdl2 libsdl2-net zlib virtual/libgles2"
inherit pkgconfig

EXTRA_OEMAKE = "\
    GLES=1 \
    STRIP_BINARY=0 \
    VERSION='${PV}' \
"

do_compile() {
    oe_runmake
}

do_install() {
    oe_runmake install DESTDIR=${D} PREFIX=${prefix}
}

FILES:${PN} += "${datadir}/supermodel"
```

`oe_runmake` passes `CC`, `CXX`, `CFLAGS`, `CXXFLAGS` and `LDFLAGS` on the make
command line. Supermodel appends the flag variables to the ones it constructs
rather than being overwritten by them, so the sysroot, hardening and
`-fdebug-prefix-map` flags all reach the compiler and the `ldflags` QA check
passes. `STRIP_BINARY=0` is what keeps `-s` off the link line so that Yocto can
split the debug information itself.

## Troubleshooting

**`obj/m68kmake.exe: cannot execute binary file`** -- the generator was built for
the target, meaning the cross build was not detected and no build machine
compiler could be found. Set `CC_FOR_BUILD` explicitly.

**The link picks up the build machine's SDL2** -- `sdl2-config` was used instead
of `pkg-config`. Set `PKG_CONFIG` to your sysroot aware wrapper, or set
`SDL2_CFLAGS` and `SDL2_LIBS` directly.

**`cannot find -lstdc++` or a similar link failure** -- something set `LD` to a
raw `ld`. It is ignored when it arrives from the environment, but a command line
`LD=` wins; drop it and let it default to `$(CXX)` or `$(CC)`.

**Yocto `ldflags` QA error** -- `LDFLAGS` is not reaching the link. Check that
you are using `oe_runmake` rather than a bare `make`.

**The GUI cannot find Games.xml or the crosshairs** -- nothing points at the
installed data. Set `ConfigPath` and `AssetsPath` in `Supermodel.ini`, or pass
`-config-path` and `-assets-path`.
