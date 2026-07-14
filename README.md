# WineForge

![WineForge](https://img.shields.io/badge/WineForge-0.6.0.0-blue)
![Wine](https://img.shields.io/badge/Wine-11.12-8a2be2)
![Platform](https://img.shields.io/badge/platform-macOS-lightgrey)
![License](https://img.shields.io/badge/license-LGPL--2.1--or--later-green)
![Downloads](https://img.shields.io/github/downloads/Alien4042x/WineForge/total)

WineForge is a macOS-focused Wine 11.12 source tree focused on improving Windows game compatibility through D3DMetal, Rosetta, and launcher compatibility.

It is built around one idea: keep the Wine side understandable while making the Apple Game Porting Toolkit / D3DMetal runtime path easier to test for Windows games on macOS.

This repository is not upstream Wine, not CrossOver, and not a complete runtime bundle. It is a prepared Wine source tree with a focused patch set.

Current build identity:

```text
wine-11.12 (WineForge 0.6.0.0)
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

## Supported Launchers

Launcher compatibility is tested as part of the WineForge game-focused runtime path.

### Steam

Steam is supported for storefront, library, download, and game launch workflows. Steam overlay inside games is currently not supported in this tree.

### Epic Games Launcher

Epic Games Launcher is supported for storefront, download, install, and game launch workflows.

Known issue: opening the Friends/social panel can show a black surface in the Store UI. The launcher otherwise remains functional. This appears tied to Epic's accelerated browser surface; WineForge does not currently have a reliable launcher-side switch to disable that GPU acceleration path.

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
