# Native Steam bridge

This document covers the Wine components only. The native hook and overlay shim belong to WineForge-Steam. A compatible host application owns Steam installation and game-profile management.

## Components

`dlls/ntdll/steam_bridge.c` implements the opt-in PE export forwarding. Normal Windows Steam loading keeps its existing path when the opt-in is absent. Unsupported PE machines use the existing loader behavior.

`dlls/lsteamclient` contains the Valve bridge and the retained macOS adaptations. Its Unix library links against libc++. Missing native client initialization returns a recoverable failure instead of using the fatal assertion macro for that operation.

`programs/steam.exe` contains the Windows helper. It checks the bridge before changing Steam registration and preserves child arguments and exit codes. It must not overwrite the real Windows Steam executable in a user bottle.

## Environment and private DLL paths

A game environment lives under `NotProton/<AppID>` in the selected WineForge storage folder. The environment name is NotProton.

Within that environment, Steam client DLLs are placed in `drive_c/WineForge/NativeSteam`. Windows sees this internal directory as `C:\WineForge\NativeSteam`. It is a DLL directory inside the NotProton environment, not the environment's name.

```text
NotProton/<AppID>/
  drive_c/WineForge/NativeSteam/
    steamclient.dll
    steamclient64.dll
```

The Windows helper, private deployment code and registry paths currently use this internal DLL directory. Renaming it requires a coordinated code and migration change. Documentation must match the existing runtime.

## Focused build and packaging

Use the configured Wine build tree and its matching PE toolchain.

```sh
make dlls/lsteamclient/all
make programs/steam.exe/x86_64-windows/steam.exe
```

The Wine loader change must also be included in the matching ntdll build. Keep the 64-bit and 32-bit PE halves and the Unix library from one compatible Wine build.

The app runtime expects these bridge artifacts in addition to the matching Wine runtime.

| Runtime path | Build output |
| --- | --- |
| `lib/wine/x86_64-windows/lsteamclient.dll` | `dlls/lsteamclient/x86_64-windows/lsteamclient.dll` |
| `lib/wine/i386-windows/lsteamclient.dll` | `dlls/lsteamclient/i386-windows/lsteamclient.dll` |
| `lib/wine/x86_64-unix/lsteamclient.so` | `dlls/lsteamclient/lsteamclient.so` |
| `libexec/wineforge-native-steam/steam.exe` | `programs/steam.exe/x86_64-windows/steam.exe` |

The packaging layer copies the helper to the private libexec path. A plain Wine build does not create the complete app integration or its validation receipt. Native hook and overlay-shim builds remain outside this Wine source tree.

The host application owns per-game prefixes, private Valve PE deployment, registry registration, D3DMetal or DXMT selection, WFUSync and HUD settings. The loader bridge is enabled only for that native Steam launch route.

## Source provenance

The Valve lsteamclient input is pinned to Proton commit `164e0ccd2ea2b1ec2e5d08dc97f65b184c2539ca`. The macOS adaptations and helper inputs come from [NotProton](https://github.com/NotProtonNot/NotProton) v1.1.0 at commit [05a27874798ffbffb8e64a37595bde6f16d5320a](https://github.com/NotProtonNot/NotProton/commit/05a27874798ffbffb8e64a37595bde6f16d5320a).

The helper's OpenVR headers are pinned to `f51d87ecf8f7903e859b0aa4d617ff1e5f33db5a`. Its Wine header input is pinned to `015230dc0f78a543032dea0907f6c97304b25ca3`. Original per-file notices and component license files are preserved.

The WineForge ntdll bridge uses Wine's typed loader interface. WineForge adaptations carry `WineForge-Internal` source markers. This source change does not use the CrossOver binary patcher.

## Validation scope

The local source reference is WineForge commit `a342f4aeb23d719339f444b9e94e06ecf0ff1a14` plus the Steam component changes. All 3187 retained component-source hashes matched the built candidate during preparation.

The accepted connected control on 2026-10-14 used native Steam client SHA-256 `625715ada315323351ada75a077142c15d4b742be2db9e63ec354828082ab194`. It passed in 32.128 seconds with distinct 64-bit and 32-bit probe processes. Both connected through SteamClient017, Utils and Apps, reported AppID 2179380, loaded the private PE clients and mapped the expected ntdll, lsteamclient and native Steam libraries. Spaced arguments and child/helper exit code 73 were preserved.

The exact installed signed runtime is bound to that test by its separate app-owned receipt. User prefix registries and protected runtime files were unchanged. The test prefix was stopped and no probe processes remained.

This verifies the bridge contract. It does not validate every Steam interface, a full game session, graphics performance, the Metal overlay or anti-cheat compatibility. It is not validation of a Wine 11.20 port, native ARM64 Wine or FEX.
