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

#include "config.h"
#import <objc/runtime.h>
#import "cocoa_app.h"
#import "cocoa_window.h"
#import "cocoa_fullscreen.h"

/* Keep Win32 fullscreen intent separate from AppKit transition state. */
@interface WineDarwinFullscreenState : NSObject
{
@public
    BOOL requested, managed, transitioning, failed, suppressed, suspended;
    BOOL restoreResizable;
    NSWindowCollectionBehavior restoreFullscreenBehavior;
}
@end
@implementation WineDarwinFullscreenState
@end

/* App presentation belongs only to the active managed fullscreen window. */
@interface WineDarwinFullscreenPresentation : NSObject
{
    NSApplicationPresentationOptions savedOptions;
    NSWindow *owner;
    BOOL owned, failed;
    BOOL observingOptions, settingOptions, updateQueued;
}
+ (instancetype) sharedController;
- (void) update;
@end

static char presentation_options_context;

int macdrv_cocoa_fullscreen_available(void)
{
#ifdef MAC_OS_VERSION_27_0
    if (@available(macOS 27.0, *)) return TRUE;
#endif
    return FALSE;
}

static WineDarwinFullscreenState *fullscreen_state(NSWindow *window, BOOL create)
{
    static char state_key;
    WineDarwinFullscreenState *state;

    if (!macdrv_cocoa_fullscreen_available()) return nil;
    state = objc_getAssociatedObject(window, &state_key);
    if (!state && create)
    {
        state = [[[WineDarwinFullscreenState alloc] init] autorelease];
        objc_setAssociatedObject(window, &state_key, state, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [WineDarwinFullscreenPresentation sharedController];
    }
    return state;
}

BOOL macdrv_cocoa_fullscreen_managed(NSWindow *window)
{
    WineDarwinFullscreenState *state = fullscreen_state(window, NO);
    return state && state->managed;
}

@implementation WineDarwinFullscreenPresentation

+ (instancetype) sharedController
{
    static WineDarwinFullscreenPresentation *controller;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ controller = [[self alloc] init]; });
    return controller;
}

- (id) init
{
    self = [super init];
    if (self && macdrv_cocoa_fullscreen_available())
    {
        NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
        for (NSString *name in @[NSApplicationDidBecomeActiveNotification, NSWindowDidBecomeKeyNotification,
                                NSWindowDidDeminiaturizeNotification, NSWindowDidChangeOcclusionStateNotification])
            [center addObserver:self selector:@selector(activated:) name:name object:nil];
        [center addObserver:self selector:@selector(suspended:) name:NSApplicationWillResignActiveNotification object:nil];
        [center addObserver:self selector:@selector(suspended:) name:NSWindowDidResignKeyNotification object:nil];
        [center addObserver:self selector:@selector(closing:) name:NSWindowWillCloseNotification object:nil];
        [[[NSWorkspace sharedWorkspace] notificationCenter] addObserver:self selector:@selector(activated:)
                                                                 name:NSWorkspaceActiveSpaceDidChangeNotification object:nil];
        [NSApp addObserver:self forKeyPath:@"currentSystemPresentationOptions" options:0
                   context:&presentation_options_context];
        observingOptions = YES;
    }
    return self;
}

- (void) dealloc
{
    if (observingOptions)
        [NSApp removeObserver:self forKeyPath:@"currentSystemPresentationOptions" context:&presentation_options_context];
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:self];
    [owner release];
    [super dealloc];
}

- (void) setOptions:(NSApplicationPresentationOptions)options
{
    settingOptions = YES;
    @try { NSApp.presentationOptions = options; }
    @finally { settingOptions = NO; }
}

- (void) observeValueForKeyPath:(NSString*)path ofObject:(id)object
                       change:(NSDictionary*)change context:(void*)context
{
    if (context != &presentation_options_context)
    {
        [super observeValueForKeyPath:path ofObject:object change:change context:context];
        return;
    }
    if (settingOptions || updateQueued || !NSApp.active) return;
    updateQueued = YES;
    dispatch_async(dispatch_get_main_queue(), ^{
        updateQueued = NO;
        [self update];
    });
}

- (BOOL) restore
{
    if (!owned) return YES;
    @try
    {
        [self setOptions:savedOptions];
        owned = NO;
        [owner release];
        owner = nil;
        return YES;
    }
    @catch (NSException *exception)
    {
        NSLog(@"Wine fullscreen presentation restore failed: %@", exception.reason);
        return NO;
    }
}

- (void) update
{
    NSWindow *window = NSApp.keyWindow;
    WineDarwinFullscreenState *state = fullscreen_state(window, NO);
    BOOL eligible = NSApp.active && state && state->managed && !state->transitioning && !state->failed &&
                    window.visible && window.onActiveSpace && !window.miniaturized &&
                    (window.styleMask & NSWindowStyleMaskFullScreen);
    if (!eligible)
    {
        [self restore];
        failed = NO;
        return;
    }
    if (failed) return;
    if (owned && owner != window && ![self restore]) return;
    if (!owned)
    {
        savedOptions = NSApp.presentationOptions;
        owner = [window retain];
        owned = YES;
    }
    @try
    {
        NSApplicationPresentationOptions options = macdrv_cocoa_fullscreen_options(window, savedOptions);
        NSApplicationPresentationOptions controlled = NSApplicationPresentationHideDock |
            NSApplicationPresentationAutoHideDock | NSApplicationPresentationHideMenuBar |
            NSApplicationPresentationAutoHideMenuBar | NSApplicationPresentationAutoHideToolbar;
#ifdef MAC_OS_VERSION_27_0
        if (@available(macOS 27.0, *)) controlled |= NSApplicationPresentationDisableScreenCornerInteractions;
#endif
        if (NSApp.presentationOptions != options ||
            ((NSApp.currentSystemPresentationOptions ^ options) & controlled))
            [self setOptions:options];
    }
    @catch (NSException *exception)
    {
        [self restore];
        failed = YES;
        NSLog(@"Wine fullscreen presentation rejected: %@", exception.reason);
    }
}

- (void) activated:(NSNotification*)notification
{
    macdrv_cocoa_fullscreen_reconcile(NSApp.keyWindow);
    [self update];
    dispatch_async(dispatch_get_main_queue(), ^{
        macdrv_cocoa_fullscreen_reconcile(NSApp.keyWindow);
        [self update];
    });
}

- (void) suspended:(NSNotification*)notification
{
    if (notification.object == NSApp || notification.object == owner) [self restore];
}

- (void) closing:(NSNotification*)notification
{
    if (notification.object == owner) [self restore];
}

@end

NSRect macdrv_cocoa_fullscreen_confinement(NSWindow *window, NSRect requested)
{
    NSRect screen;

    if (!macdrv_cocoa_fullscreen_managed(window) ||
        !(window.styleMask & NSWindowStyleMaskFullScreen) || !window.screen)
        return requested;

    screen = [window convertRectFromScreen:window.screen.frame];
    if (!NSContainsRect(requested, screen) || NSWidth(screen) <= 4 || NSHeight(screen) <= 4)
        return requested;

    /* Avoid the exact screen boundary for full-monitor confinement. */
    return NSIntersectionRect(requested, NSInsetRect(screen, 2, 2));
}

NSUInteger macdrv_cocoa_fullscreen_style(NSWindow *window, NSUInteger style)
{
    WineDarwinFullscreenState *state = fullscreen_state(window, NO);
    if (state && state->managed)
    {
        state->restoreResizable = !!(style & NSWindowStyleMaskResizable);
        style |= NSWindowStyleMaskResizable;
    }
    return style;
}

void macdrv_cocoa_fullscreen_reconcile(NSWindow *window)
{
    WineDarwinFullscreenState *state = fullscreen_state(window, NO);
    BOOL native = !!(window.styleMask & NSWindowStyleMaskFullScreen);
    if (!state || state->transitioning || state->failed) return;

    if (!state->requested)
    {
        if (native && state->managed)
        {
            state->transitioning = TRUE;
            [window toggleFullScreen:nil];
        }
        return;
    }
    if (native || state->suppressed || !NSApp.active || !window.keyWindow || !window.visible ||
        !window.onActiveSpace || window.miniaturized || window.parentWindow ||
        window.styleMask & (NSWindowStyleMaskUtilityWindow | NSWindowStyleMaskNonactivatingPanel)) return;

    state->restoreResizable = !!(window.styleMask & NSWindowStyleMaskResizable);
    state->restoreFullscreenBehavior = window.collectionBehavior &
        (NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorFullScreenPrimary);
    state->suspended = FALSE;
    state->managed = TRUE;
    state->transitioning = TRUE;
    [window setStyleMask:window.styleMask | NSWindowStyleMaskResizable];
    [window setCollectionBehavior:(window.collectionBehavior & ~NSWindowCollectionBehaviorFullScreenAuxiliary) |
                                 NSWindowCollectionBehaviorFullScreenPrimary];
    [window toggleFullScreen:nil];
}

void macdrv_set_window_native_fullscreen(WineWindow *window, int requested)
{
    if (!macdrv_cocoa_fullscreen_available()) return;
    OnMainThreadAsync(^{
        WineDarwinFullscreenState *state = fullscreen_state(window, requested);
        if (!state) return;
        if (state->requested != !!requested)
        {
            state->requested = !!requested;
            state->failed = FALSE;
            state->suppressed = FALSE;
        }
        macdrv_cocoa_fullscreen_reconcile(window);
        [[WineDarwinFullscreenPresentation sharedController] update];
    });
}

void macdrv_cocoa_fullscreen_transition(NSWindow *window, BOOL entering, BOOL complete, BOOL failed)
{
    WineDarwinFullscreenState *state = fullscreen_state(window, NO);
    if (!state || !state->managed) return;
    state->transitioning = !complete;
    if (!complete)
    {
        [[WineDarwinFullscreenPresentation sharedController] update];
        return;
    }
    state->failed = failed;
    if ((!entering && !failed) || (entering && failed))
    {
        state->managed = FALSE;
        /* Wine temporarily closes a native window while hiding it. That is
         * not a user request to leave fullscreen when it is shown again. */
        state->suppressed = state->requested && !state->suspended;
        if (!state->restoreResizable)
            [window setStyleMask:window.styleMask & ~NSWindowStyleMaskResizable];
        [window setCollectionBehavior:(window.collectionBehavior & ~(NSWindowCollectionBehaviorFullScreenPrimary |
                                     NSWindowCollectionBehaviorFullScreenAuxiliary)) | state->restoreFullscreenBehavior];
    }
    [[WineDarwinFullscreenPresentation sharedController] update];
    dispatch_async(dispatch_get_main_queue(), ^{
        macdrv_cocoa_fullscreen_reconcile(window);
        [[WineDarwinFullscreenPresentation sharedController] update];
    });
}

void macdrv_cocoa_fullscreen_will_hide(NSWindow *window)
{
    WineDarwinFullscreenState *state = fullscreen_state(window, NO);
    if (state && state->managed) state->suspended = TRUE;
}

void macdrv_cocoa_fullscreen_close(NSWindow *window)
{
    WineDarwinFullscreenState *state = fullscreen_state(window, NO);
    if (!state) return;
    state->requested = FALSE;
    state->failed = TRUE;
    state->managed = FALSE;
    [[WineDarwinFullscreenPresentation sharedController] update];
}

NSApplicationPresentationOptions macdrv_cocoa_fullscreen_options(NSWindow *window,
                                                                NSApplicationPresentationOptions proposed)
{
    if (!macdrv_cocoa_fullscreen_managed(window)) return proposed;
    proposed &= ~(NSApplicationPresentationAutoHideDock | NSApplicationPresentationAutoHideMenuBar |
                  NSApplicationPresentationAutoHideToolbar);
#ifdef MAC_OS_VERSION_27_0
    if (@available(macOS 27.0, *))
        proposed |= NSApplicationPresentationDisableScreenCornerInteractions;
#endif
    return proposed | NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar;
}
