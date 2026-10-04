/*
 * MACDRV GameController mouse support
 *
 * Copyright 2026 Elvin Hayatov
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

#import <AppKit/AppKit.h>
#import <GameController/GameController.h>

#include "macdrv_cocoa.h"
#import "cocoa_app.h"
#import "cocoa_event.h"
#import "cocoa_window.h"

#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"


static NSMutableSet *configured_mice;
static dispatch_queue_t gcmouse_queue;
static BOOL observing_mice;
static bool gcmouse_active;


@interface WineApplicationController (GCMouseAccess)

    - (WineWindow *) mouseCaptureWindow;

@end


@interface WineApplicationController (GCMouseNotifications)

    - (void) gcmouseDidConnect:(NSNotification *)notification;
    - (void) gcmouseDidDisconnect:(NSNotification *)notification;

@end


static void post_mouse_movement(float delta_x, float delta_y)
{
    WineApplicationController *controller;
    WineWindow *window;
    macdrv_event *event;

    if (!macdrv_gcmouse_input_active() || !NSApp || (!delta_x && !delta_y)) return;

    controller = [WineApplicationController sharedController];
    window = [controller mouseCaptureWindow] ?: [controller frontWineWindow];
    if (!window) return;

    event = macdrv_create_event(MOUSE_MOVED_RAW, window);
    event->mouse_moved.x = delta_x;
    event->mouse_moved.y = -delta_y;
    event->mouse_moved.time_ms = [controller ticksForEventTime:[[NSProcessInfo processInfo] systemUptime]];
    [window.queue postEvent:event];
    macdrv_release_event(event);
}


static void configure_mouse(GCMouse *mouse)
{
    GCMouseInput *input;

    if (!mouse || [configured_mice containsObject:mouse]) return;

    if (!(input = [mouse mouseInput])) return;

    [mouse setHandlerQueue:gcmouse_queue];
    [input setMouseMovedHandler:^(GCMouseInput *input, float delta_x, float delta_y) {
        post_mouse_movement(delta_x, delta_y);
    }];

    [configured_mice addObject:mouse];
    __atomic_store_n(&gcmouse_active, true, __ATOMIC_SEQ_CST);
}


static void remove_mouse(GCMouse *mouse)
{
    if (!mouse || ![configured_mice containsObject:mouse]) return;

    [[mouse mouseInput] setMouseMovedHandler:nil];
    [configured_mice removeObject:mouse];
    __atomic_store_n(&gcmouse_active, [configured_mice count] != 0, __ATOMIC_SEQ_CST);
}


static void start_gcmouse_input(void)
{
    NSNotificationCenter *center;
    WineApplicationController *controller;
    GCMouse *mouse;

    if (!use_gcmouse) return;
    if (observing_mice) return;

    if (@available(macOS 14.0, *))
    {
        if (!NSClassFromString(@"GCMouse")) return;

        center = [NSNotificationCenter defaultCenter];
        controller = [WineApplicationController sharedController];
        gcmouse_queue = dispatch_queue_create("org.winehq.GCMouse", DISPATCH_QUEUE_SERIAL);
        if (!gcmouse_queue) return;
        dispatch_set_target_queue(gcmouse_queue,
                                  dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0));
        configured_mice = [[NSMutableSet alloc] init];
        observing_mice = TRUE;

        [center addObserver:controller selector:@selector(gcmouseDidConnect:)
                     name:GCMouseDidConnectNotification object:nil];
        [center addObserver:controller selector:@selector(gcmouseDidDisconnect:)
                     name:GCMouseDidDisconnectNotification object:nil];

        for (mouse in [GCMouse mice])
            configure_mouse(mouse);
    }
}


@implementation WineApplicationController (GCMouseNotifications)

    - (void) gcmouseDidConnect:(NSNotification *)notification
    {
        configure_mouse([notification object]);
    }

    - (void) gcmouseDidDisconnect:(NSNotification *)notification
    {
        remove_mouse([notification object]);
    }

@end


void macdrv_start_gcmouse_input(void)
{
    OnMainThreadAsync(^{ start_gcmouse_input(); });
}


bool macdrv_gcmouse_input_active(void)
{
    return __atomic_load_n(&gcmouse_active, __ATOMIC_SEQ_CST);
}
