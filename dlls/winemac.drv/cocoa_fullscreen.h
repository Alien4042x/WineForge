/*
 * Native macOS fullscreen lifecycle for Wine windows.
 * WineForge-Internal: fullscreen/native-window-v1.
 *
 * Copyright (C) 2026 Radim Vesely for WineForge
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef WINE_COCOA_FULLSCREEN_H
#define WINE_COCOA_FULLSCREEN_H

#import <AppKit/AppKit.h>

int macdrv_cocoa_fullscreen_available(void);
BOOL macdrv_cocoa_fullscreen_managed(NSWindow *window);
NSRect macdrv_cocoa_fullscreen_confinement(NSWindow *window, NSRect requested);
NSUInteger macdrv_cocoa_fullscreen_style(NSWindow *window, NSUInteger style);
void macdrv_cocoa_fullscreen_reconcile(NSWindow *window);
void macdrv_cocoa_fullscreen_will_hide(NSWindow *window);
void macdrv_cocoa_fullscreen_close(NSWindow *window);
void macdrv_cocoa_fullscreen_transition(NSWindow *window, BOOL entering, BOOL complete, BOOL failed);
NSApplicationPresentationOptions macdrv_cocoa_fullscreen_options(NSWindow *window,
                                                                NSApplicationPresentationOptions proposed);

#endif
