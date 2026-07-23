# WineForge

![WineForge](https://img.shields.io/badge/WineForge-0.6.0.1-blue)
![Wine](https://img.shields.io/badge/Wine-11.13-8a2be2)
![Platform](https://img.shields.io/badge/platform-macOS-lightgrey)
![License](https://img.shields.io/badge/license-LGPL--2.1--or--later-green)
![Downloads](https://img.shields.io/github/downloads/Alien4042x/WineForge/total)

WineForge is a macOS-focused Wine 11.13 source tree focused on improving Windows game compatibility through D3DMetal, Rosetta, and launcher compatibility.

It is built around one idea: keep the Wine side understandable while making the Apple Game Porting Toolkit / D3DMetal runtime path easier to test for Windows games on macOS.

This repository is not upstream Wine, not CrossOver, and not a complete runtime bundle. It is a prepared Wine source tree with a focused patch set.

Current build identity:

```text
wine-11.13 (WineForge 0.6.0.1)
```

## Credits

WineForge is based on Wine.

The macOS, winemac, Rosetta, and D3DMetal work in this tree builds upon publicly available work from the Wine project, CodeWeavers, CrossOver, and the Apple Game Porting Toolkit ecosystem. WineForge keeps that provenance visible where practical and does not claim those parts as original WineForge code.

Thank you to the Wine project, CodeWeavers, CrossOver developers, and everyone involved in the Apple Game Porting Toolkit ecosystem.

## Patch Provenance

WineForge keeps provenance visible in the source where practical. The current tree contains references to the following CrossOver / CodeWeavers patch IDs:

- `18947`
- `20186`
- `20760`
- `22131`
- `22434`
- `22435`
- `23015`
- `23427`
- `24945`
- `25719`
- `26456`
- `26470`

These references are kept intentionally so the origin of the low-level compatibility work remains visible. WineForge-local changes are marked in source and local patch files where practical.

## Build Notes

This repository is the patched Wine source tree. It does not include a complete packaged runtime, bottle manager, or release installer.

A local macOS build currently expects:

- Xcode Command Line Tools.
- Intel Homebrew under `/usr/local`.
- Homebrew `bison`.
- Homebrew `pkg-config`.
- `llvm-mingw` from https://github.com/mstorsjo/llvm-mingw/releases.
- A separate Apple Game Porting Toolkit / D3DMetal runtime source from https://developer.apple.com/games/game-porting-toolkit/.

The tested local configure shape is:

```sh
arch -x86_64 "$SOURCE/configure" \
  --build=x86_64-apple-darwin \
  --enable-archs=i386,x86_64 \
  --prefix="$INSTALL" \
  --disable-tests \
  --disable-winedbg \
  --disable-winemenubuilder \
  --without-alsa \
  --without-capi \
  --with-coreaudio \
  --with-cups \
  --without-dbus \
  --with-ffmpeg \
  --without-fontconfig \
  --with-freetype \
  --with-gettext \
  --without-gettextpo \
  --without-gphoto \
  --with-gnutls \
  --without-gssapi \
  --with-gstreamer \
  --without-inotify \
  --without-krb5 \
  --with-mingw \
  --without-netapi \
  --with-opencl \
  --without-opengl \
  --without-oss \
  --with-pcap \
  --without-pcsclite \
  --with-pthread \
  --without-pulse \
  --without-sane \
  --with-sdl \
  --without-udev \
  --without-usb \
  --without-v4l2 \
  --without-vulkan \
  --without-wayland \
  --without-x
```

Then build Wine normally:

```sh
make -j8
make install
```

The exact prefix, dependency paths, and runtime sync are intentionally left to the local builder or packaging layer.

## Runtime Notes

WineForge expects the D3DMetal runtime to be available from a local runtime directory. The active source patches route D3DMetal DLL and Unix library lookup through that local layout instead of relying on a system-wide install.

Wine's Mono and Gecko handling remains upstream Wine behavior. This source tree does not bundle Mono or Gecko unpacked directories.

## Graphics Backends

D3DMetal remains the primary D3D11 and D3D12 backend. WineForge also supports process-selectable [DXMT](https://github.com/3Shain/dxmt) for D3D10 and D3D11 applications and games. DXMT does not provide D3D12 support.

WineForge can route selected launcher processes to a separate [DXMT-CEF](https://github.com/Alien4042x/dxmt-cef) runtime while the game keeps the configured global graphics backend. DXMT-CEF is a focused DXMT variant for CEF-based launchers that require D3D11 rendering unavailable through D3DMetal. It is currently used by Rockstar Games Launcher and Social Club Helper.

DXMT and DXMT-CEF runtime binaries are maintained separately and are not bundled in this source repository.

### WineForge DXCompat

WineForge DXCompat (`WFDXCompat`) is an optional external compatibility
frontend placed in front of the process-selected D3DMetal backend. It fills
focused DirectX interfaces and resource behavior that D3DMetal does not yet
provide, while forwarding supported graphics behavior to the selected
backend.

The first implementation extends the D3D12 path with missing video-resource,
plane-copy, barrier, and resource-lifetime behavior. The project is structured
so additional focused frontend modules can be added as further D3DMetal
compatibility gaps are identified and verified. WFDXCompat activates only when
D3DMetal is the selected process backend; processes without the matching
external frontend keep their existing graphics path.

The external runtime is not bundled in this source repository. Its current
layout is:

```text
lib/wfdxcompat/
  x86_64-windows/
    d3d12.dll
    wfdxbackend-d3d12.dll
```

WineForge reads an explicit `WFDXCOMPAT_RUNTIME_DIR`, or derives the sibling
`lib/wfdxcompat` directory from `D3DMETAL_RUNTIME_DIR`. If the frontend is
absent, WineForge retains the normal D3DMetal D3D12 path.

As its frontend coverage grows, WFDXCompat may also become suitable for
Direct3D-based launcher surfaces that currently use the separate DXMT-CEF
runtime. DXMT-CEF remains the current launcher path while that coverage is
developed and validated.

## Running a Local Build

The examples below assume that the Wine build and the external graphics runtimes have already been installed. The packaging layer must make the matching PE DLL and Unix library directories available through `WINEDLLPATH`, `DYLD_LIBRARY_PATH`, or equivalent launcher setup.

Set the common paths first:

```sh
export WINE=/path/to/wineforge/bin/wine
export WINEPREFIX=/path/to/prefix
export D3DMETAL_RUNTIME_DIR=/path/to/lib/d3dmetal
export DXMT_RUNTIME_DIR=/path/to/lib/dxmt
export DXMT_CEF_RUNTIME_DIR=/path/to/lib/dxmt-cef
export WFDXCOMPAT_RUNTIME_DIR=/path/to/lib/wfdxcompat
```

Run with D3DMetal using the AMD / FidelityFX identity:

```sh
env GRAPHICS_BACKEND=d3dmetal \
    D3DMETAL_UPSCALER_PROFILE=amd \
    D3DM_VENDOR_ID=4098 \
    D3DM_DEVICE_ID=29631 \
    D3DM_DEVICE_DESCRIPTION="AMD Radeon RX 6800 XT" \
    WINEDLLOVERRIDES='atidxx64,amdxc64,amd_fidelityfx_upscaler_dx12,amd_fidelityfx_framegeneration_dx12=n,b;dxgi,d3d10,d3d10core,d3d11,d3d12=n,b' \
    "$WINE" /path/to/application.exe
```

Run with D3DMetal using the NVIDIA / MetalFX identity:

```sh
env GRAPHICS_BACKEND=d3dmetal \
    D3DMETAL_UPSCALER_PROFILE=nvidia \
    D3DM_VENDOR_ID=4318 \
    D3DM_DEVICE_ID=10370 \
    D3DM_DEVICE_DESCRIPTION="NVIDIA GeForce RTX 4080" \
    D3DM_ENABLE_METALFX=1 \
    WINEDLLOVERRIDES='dxgi,d3d10,d3d10core,d3d11,d3d12=n,b;nvapi,nvapi64,nvngx=b' \
    "$WINE" /path/to/application.exe
```

The NVIDIA example also requires the matching NVAPI/NVNGX PE files and registry setup supplied by the selected D3DMetal runtime package.

Run a D3D10 or D3D11 application entirely through DXMT:

```sh
env GRAPHICS_BACKEND=dxmt \
    WINEDLLOVERRIDES='dxgi,d3d10,d3d10core,d3d11,d3d12,winemetal=b' \
    "$WINE" /path/to/application.exe
```

Enable DXMT's NVIDIA extension path when a game requires it:

```sh
env GRAPHICS_BACKEND=dxmt \
    DXMT_ENABLE_NVEXT=1 \
    WINEDLLOVERRIDES='dxgi,d3d10,d3d10core,d3d11,d3d12,winemetal=b' \
    "$WINE" /path/to/game.exe
```

When `DXMT_CEF_RUNTIME_DIR` points to a complete runtime, WineForge automatically routes only its built-in exact-path Rockstar launcher processes through DXMT-CEF. The game continues to use the global `GRAPHICS_BACKEND`; no application list, registry value, or additional environment switch is required.

WFUSync remains independent of graphics selection and can be enabled for any of these commands with `WINEWFUSYNC=1`.

## Supported Launchers

Launcher compatibility is tested as part of the WineForge game-focused runtime path.

### Steam

Steam is supported for storefront, library, download, and game launch workflows. Steam overlay inside games is currently not supported in this tree.

### Epic Games Launcher

Epic Games Launcher is supported for storefront, download, install, and game launch workflows.

Known issue: opening the Friends/social panel can show a black surface in the Store UI. The launcher otherwise remains functional. This appears tied to Epic's accelerated browser surface; WineForge does not currently have a reliable launcher-side switch to disable that GPU acceleration path.

### Rockstar Games Launcher

Rockstar Games Launcher and Social Club are supported for installation, updates, sign-in, service startup, and game launch workflows. The standalone launcher and the Epic Games Launcher to Rockstar Games Launcher path have been validated with Red Dead Redemption 2.

Selected Rockstar CEF processes use the isolated [DXMT-CEF](https://github.com/Alien4042x/dxmt-cef) runtime, while the launched game keeps the configured global graphics backend.

## WFUSync

WFUSync is an opt-in WineForge userspace synchronization backend for macOS.

Enable it with:

```sh
WINEWFUSYNC=1
```

Disable it by unsetting the variable or setting:

```sh
WINEWFUSYNC=0
```

Unsupported waits and sensitive synchronization semantics remain on the Wine server fallback path. Probe and targeted validation have shown good results, but this is not claimed as a guaranteed per-game performance improvement.

## Status

This is an experimental WineForge source tree focused on local testing and reproducible macOS gaming builds.

Some games and launchers work, while others still need profiling or targeted compatibility work. Some compatibility patches currently prioritize functionality and testing while the implementation continues to evolve.

## License

WineForge is based on Wine and keeps Wine's license files in this repository.

Wine is free software under the GNU Lesser General Public License. See `LICENSE` and `LICENSE.OLD`.

## Upstream

- Wine: https://www.winehq.org/
- Wine source: https://gitlab.winehq.org/wine/wine
- Apple Game Porting Toolkit: https://developer.apple.com/games/game-porting-toolkit/
- llvm-mingw: https://github.com/mstorsjo/llvm-mingw/releases
