/*
 * Server-side semaphore management
 *
 * Copyright (C) 1998 Alexandre Julliard
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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/types.h>

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"

#include "handle.h"
#include "thread.h"
#include "request.h"
#include "security.h"
#include "wfusync.h"

static const WCHAR semaphore_name[] = {'S','e','m','a','p','h','o','r','e'};

struct type_descr semaphore_type =
{
    { semaphore_name, sizeof(semaphore_name) },   /* name */
    SEMAPHORE_ALL_ACCESS,                         /* valid_access */
    {                                             /* mapping */
        STANDARD_RIGHTS_READ | SEMAPHORE_QUERY_STATE,
        STANDARD_RIGHTS_WRITE | SEMAPHORE_MODIFY_STATE,
        STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE,
        SEMAPHORE_ALL_ACCESS
    },
};

struct semaphore_sync
{
    struct object       obj;                /* object header */
    unsigned int        count;              /* current count */
    unsigned int        max;                /* maximum possible count */
#ifdef __APPLE__
    struct wfusync     *wfusync;            /* optional shared fast-path state */
#endif
};

static void semaphore_sync_dump( struct object *obj, int verbose );
static int semaphore_sync_add_queue( struct object *obj, struct wait_queue_entry *entry );
static void semaphore_sync_remove_queue( struct object *obj, struct wait_queue_entry *entry );
static int semaphore_sync_signaled( struct object *obj, struct wait_queue_entry *entry );
static void semaphore_sync_satisfied( struct object *obj, struct wait_queue_entry *entry );
static void semaphore_sync_destroy( struct object *obj );

static const struct object_ops semaphore_sync_ops =
{
    .size         = sizeof(struct semaphore_sync),
    .type         = &no_type,
    .dump         = semaphore_sync_dump,
    .add_queue    = semaphore_sync_add_queue,
    .remove_queue = semaphore_sync_remove_queue,
    .signaled     = semaphore_sync_signaled,
    .satisfied    = semaphore_sync_satisfied,
    .destroy      = semaphore_sync_destroy,
};

static int release_semaphore( struct semaphore_sync *sem, unsigned int count,
                              unsigned int *prev )
{
#ifdef __APPLE__
    if (sem->wfusync)
    {
        unsigned int old_count;

        if (!wfusync_release_semaphore( sem->wfusync, count, &old_count )) return 0;
        if (prev) *prev = old_count;
        if (!old_count)
        {
            wake_up( &sem->obj, count );
            wfusync_wake_waiters( wfusync_get_index( sem->wfusync ) );
        }
        return 1;
    }
#endif

    if (prev) *prev = sem->count;
    if (sem->count + count < sem->count || sem->count + count > sem->max)
    {
        set_error( STATUS_SEMAPHORE_LIMIT_EXCEEDED );
        return 0;
    }
    else if (sem->count)
    {
        /* there cannot be any thread to wake up if the count is != 0 */
        sem->count += count;
    }
    else
    {
        sem->count = count;
        wake_up( &sem->obj, count );
    }
    return 1;
}

static void semaphore_sync_dump( struct object *obj, int verbose )
{
    struct semaphore_sync *sem = (struct semaphore_sync *)obj;
    assert( obj->ops == &semaphore_sync_ops );
    fprintf( stderr, "Semaphore count=%d max=%d\n", sem->count, sem->max );
}

static int semaphore_sync_add_queue( struct object *obj, struct wait_queue_entry *entry )
{
    struct semaphore_sync *sem = (struct semaphore_sync *)obj;
    assert( obj->ops == &semaphore_sync_ops );
#ifdef __APPLE__
    if (sem->wfusync) wfusync_add_server_waiter( sem->wfusync );
#endif
    return add_queue( obj, entry );
}

static void semaphore_sync_remove_queue( struct object *obj, struct wait_queue_entry *entry )
{
    struct semaphore_sync *sem = (struct semaphore_sync *)obj;
    assert( obj->ops == &semaphore_sync_ops );
#ifdef __APPLE__
    if (sem->wfusync) wfusync_remove_server_waiter( sem->wfusync );
#endif
    remove_queue( obj, entry );
}

static int semaphore_sync_signaled( struct object *obj, struct wait_queue_entry *entry )
{
    struct semaphore_sync *sem = (struct semaphore_sync *)obj;
    assert( obj->ops == &semaphore_sync_ops );
#ifdef __APPLE__
    if (sem->wfusync) return wfusync_semaphore_signaled( sem->wfusync );
#endif
    return (sem->count > 0);
}

static void semaphore_sync_satisfied( struct object *obj, struct wait_queue_entry *entry )
{
    struct semaphore_sync *sem = (struct semaphore_sync *)obj;
    assert( obj->ops == &semaphore_sync_ops );
#ifdef __APPLE__
    if (sem->wfusync)
    {
        wfusync_semaphore_satisfied( sem->wfusync );
        return;
    }
#endif
    assert( sem->count );
    sem->count--;
}

static struct object *create_semaphore_sync( unsigned int initial, unsigned int max, int inproc )
{
    struct semaphore_sync *sem;

    if (!(sem = alloc_object( &semaphore_sync_ops ))) return NULL;
    sem->count = initial;
    sem->max   = max;
#ifdef __APPLE__
    sem->wfusync = NULL;
    if (inproc && do_wfusync() &&
        !(sem->wfusync = create_wfusync( initial, max, INPROC_SYNC_SEMAPHORE )))
        clear_error();
#endif
    return &sem->obj;
}

static void semaphore_sync_destroy( struct object *obj )
{
#ifdef __APPLE__
    struct semaphore_sync *sem = (struct semaphore_sync *)obj;
    assert( obj->ops == &semaphore_sync_ops );
    if (sem->wfusync) wfusync_destroy( sem->wfusync );
#endif
}

struct semaphore
{
    struct object          obj;    /* object header */
    struct object         *sync;   /* semaphore sync object */
};

struct semaphore_init_data
{
    unsigned int initial;
    unsigned int max;
};

static void semaphore_dump( struct object *obj, int verbose );
static bool semaphore_init( struct object *obj, const void *init_data );
static struct object *semaphore_get_sync( struct object *obj );
static int semaphore_signal( struct object *obj, unsigned int access, int signal );
static void semaphore_destroy( struct object *obj );

static const struct object_ops semaphore_ops =
{
    .size     = sizeof(struct semaphore),
    .type     = &semaphore_type,
    .dump     = semaphore_dump,
    .init     = semaphore_init,
    .signal   = semaphore_signal,
    .get_sync = semaphore_get_sync,
    .destroy  = semaphore_destroy,
};

static void semaphore_dump( struct object *obj, int verbose )
{
    struct semaphore *sem = (struct semaphore *)obj;
    assert( obj->ops == &semaphore_ops );
    sem->sync->ops->dump( sem->sync, verbose );
}

static bool semaphore_init( struct object *obj, const void *init_data )
{
    struct semaphore *sem = (struct semaphore *)obj;
    const struct semaphore_init_data *data = init_data;

    return !!(sem->sync = create_semaphore_sync( data->initial, data->max, 1 ));
}

static struct object *semaphore_get_sync( struct object *obj )
{
    struct semaphore *sem = (struct semaphore *)obj;
    assert( obj->ops == &semaphore_ops );
    return grab_object( sem->sync );
}

static int semaphore_signal( struct object *obj, unsigned int access, int signal )
{
    struct semaphore *sem = (struct semaphore *)obj;
    assert( obj->ops == &semaphore_ops );

    assert( sem->sync->ops == &semaphore_sync_ops ); /* never called with inproc syncs */
    assert( signal == -1 ); /* always called from signal_object */

    if (!(access & SEMAPHORE_MODIFY_STATE))
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }
    return release_semaphore( (struct semaphore_sync *)sem->sync, 1, NULL );
}

static void semaphore_destroy( struct object *obj )
{
    struct semaphore *sem = (struct semaphore *)obj;
    assert( obj->ops == &semaphore_ops );
    if (sem->sync) release_object( sem->sync );
}

int get_semaphore_wfusync_idx( struct object *obj, int *type )
{
#ifdef __APPLE__
    struct semaphore_sync *sync;
    struct semaphore *sem;

    if (!do_wfusync() || !obj || obj->ops != &semaphore_ops) return -1;
    sem = (struct semaphore *)obj;
    if (!sem->sync || sem->sync->ops != &semaphore_sync_ops) return -1;
    sync = (struct semaphore_sync *)sem->sync;
    if (!sync->wfusync) return -1;
    wfusync_grab_object( sync->wfusync );
    *type = INPROC_SYNC_SEMAPHORE;
    return wfusync_get_index( sync->wfusync );
#else
    return -1;
#endif
}

/* create a semaphore */
DECL_HANDLER(create_semaphore)
{
    struct semaphore_init_data data = { .initial = req->initial, .max = req->max };
    struct object_params params = { .ops = &semaphore_ops, .access = req->access, .init_data = &data };

    if (!req->max || (req->initial > req->max))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    if (!get_req_object_attributes( &params )) return;
    reply->handle = create_named_obj_handle( current->process, &params );
    if (params.root) release_object( params.root );
}

/* open a handle to a semaphore */
DECL_HANDLER(open_semaphore)
{
    reply->handle = open_object( current->process, req->rootdir, req->access,
                                 &semaphore_ops, get_req_unicode_str(), req->attributes );
}

/* release a semaphore */
DECL_HANDLER(release_semaphore)
{
    struct semaphore *sem;

    if ((sem = (struct semaphore *)get_handle_obj( current->process, req->handle,
                                                   SEMAPHORE_MODIFY_STATE, &semaphore_ops )))
    {
        struct semaphore_sync *sync = (struct semaphore_sync *)sem->sync;
        assert( sem->sync->ops == &semaphore_sync_ops ); /* never called with inproc syncs */

        release_semaphore( sync, req->count, &reply->prev_count );
        release_object( sem );
    }
}

/* query details about the semaphore */
DECL_HANDLER(query_semaphore)
{
    struct semaphore *sem;

    if ((sem = (struct semaphore *)get_handle_obj( current->process, req->handle,
                                                   SEMAPHORE_QUERY_STATE, &semaphore_ops )))
    {
        struct semaphore_sync *sync = (struct semaphore_sync *)sem->sync;
        assert( sem->sync->ops == &semaphore_sync_ops ); /* never called with inproc syncs */

#ifdef __APPLE__
        if (sync->wfusync) wfusync_get_semaphore_state( sync->wfusync, &reply->current, &reply->max );
        else
#endif
        {
            reply->current = sync->count;
            reply->max = sync->max;
        }
        release_object( sem );
    }
}
