# Native macOS fullscreen

WineForge's native fullscreen integration requires both:

- macOS 27 Golden Gate or later at runtime.
- A Wine build made with the macOS 27 SDK or later.

The SDK guard and runtime version check are implemented in
[`macdrv_cocoa_fullscreen_available()`](../dlls/winemac.drv/cocoa_fullscreen.m).
On earlier macOS versions, or in builds made with an older SDK, Wine keeps its
existing fullscreen handling. This requirement applies to the native
fullscreen feature. Apple's release information uses the name
[macOS 27 Golden Gate](https://support.apple.com/en-us/127257).

## Behavior

Borderless windows that Wine identifies as fullscreen can enter native macOS
fullscreen, including borderless windows with `WS_MAXIMIZE`. Captioned and
child windows keep their existing handling. Maximizing an ordinary decorated
window does not itself request this feature.

The integration preserves fullscreen intent across temporary window hiding and
reconciles it when the window is shown or activated again. A deliberate native
fullscreen exit remains respected.

The active managed fullscreen window controls Dock, menu-bar and screen-corner
suppression. Previous presentation settings are restored when ownership ends.
Existing application-requested cursor confinement follows the managed window
through native fullscreen transitions.

This behavior is implemented in `winemac.drv` and follows the window state. It
does not change the selected graphics backend or enable Game Mode.

## Validation scope

The retained Wine 11.20 reference passed focused fullscreen lifecycle,
presentation-policy, cursor and unavailable-API controls. Game observations
apply to their recorded runtime combinations. They do not establish runtime
acceptance for the Wine 11.21 update or every game.

The fallback was checked with the new API path disabled. An older macOS system
has not been used for a runtime test.
