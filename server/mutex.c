/*
 * Server-side mutex management
 * WineForge-Internal: usync-v6.
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
#include "usync.h"

static const WCHAR mutex_name[] = {'M','u','t','a','n','t'};

struct type_descr mutex_type =
{
    { mutex_name, sizeof(mutex_name) },   /* name */
    MUTANT_ALL_ACCESS,                    /* valid_access */
    {                                     /* mapping */
        STANDARD_RIGHTS_READ | MUTANT_QUERY_STATE,
        STANDARD_RIGHTS_WRITE,
        STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE,
        MUTANT_ALL_ACCESS
    },
};

struct mutex_sync
{
    struct object       obj;                /* object header */
    struct thread      *owner;              /* mutex owner */
    unsigned int        count;              /* recursion count */
    int                 abandoned;          /* has it been abandoned? */
    struct list         entry;              /* entry in owner thread mutex list */
    struct list         usync_entry;      /* entry in USync mutex list */
    struct usync     *usync;            /* optional shared fast-path state */
};

static void mutex_sync_dump( struct object *obj, int verbose );
static int mutex_sync_add_queue( struct object *obj, struct wait_queue_entry *entry );
static void mutex_sync_remove_queue( struct object *obj, struct wait_queue_entry *entry );
static int mutex_sync_signaled( struct object *obj, struct wait_queue_entry *entry );
static void mutex_sync_satisfied( struct object *obj, struct wait_queue_entry *entry );
static void mutex_sync_destroy( struct object *obj );

static struct list usync_mutexes = LIST_INIT( usync_mutexes );

static const struct object_ops mutex_sync_ops =
{
    .size         = sizeof(struct mutex_sync),
    .type         = &no_type,
    .dump         = mutex_sync_dump,
    .add_queue    = mutex_sync_add_queue,
    .remove_queue = mutex_sync_remove_queue,
    .signaled     = mutex_sync_signaled,
    .satisfied    = mutex_sync_satisfied,
    .destroy      = mutex_sync_destroy,
};

/* grab a mutex for a given thread */
static void do_grab( struct mutex_sync *mutex, struct thread *thread )
{
    assert( !mutex->count || (mutex->owner == thread) );

    if (!mutex->count++)  /* FIXME: avoid wrap-around */
    {
        assert( !mutex->owner );
        grab_object( mutex );
        mutex->owner = thread;
        list_add_head( &thread->mutex_list, &mutex->entry );
    }
}

/* release a mutex once the recursion count is 0 */
static int do_release( struct mutex_sync *mutex, struct thread *thread, int count )
{
    if (!mutex->count || (mutex->owner != thread))
    {
        set_error( STATUS_MUTANT_NOT_OWNED );
        return 0;
    }
    if (!(mutex->count -= count))
    {
        /* remove the mutex from the thread list of owned mutexes */
        list_remove( &mutex->entry );
        mutex->owner = NULL;
        wake_up( &mutex->obj, 0 );
        release_object( mutex );
    }
    return 1;
}

static void mutex_sync_dump( struct object *obj, int verbose )
{
    struct mutex_sync *mutex = (struct mutex_sync *)obj;
    assert( obj->ops == &mutex_sync_ops );
    fprintf( stderr, "Mutex count=%u owner=%p\n", mutex->count, mutex->owner );
}

static int mutex_sync_add_queue( struct object *obj, struct wait_queue_entry *entry )
{
    struct mutex_sync *mutex = (struct mutex_sync *)obj;
    assert( obj->ops == &mutex_sync_ops );
    if (mutex->usync) usync_add_server_waiter( mutex->usync );
    return add_queue( obj, entry );
}

static void mutex_sync_remove_queue( struct object *obj, struct wait_queue_entry *entry )
{
    struct mutex_sync *mutex = (struct mutex_sync *)obj;
    assert( obj->ops == &mutex_sync_ops );
    if (mutex->usync) usync_remove_server_waiter( mutex->usync );
    remove_queue( obj, entry );
}

static void mutex_sync_destroy( struct object *obj )
{
    struct mutex_sync *mutex = (struct mutex_sync *)obj;
    assert( obj->ops == &mutex_sync_ops );
    if (mutex->usync)
    {
        list_remove( &mutex->usync_entry );
        usync_destroy( mutex->usync );
    }
    assert( !mutex->count );
}

static int mutex_sync_signaled( struct object *obj, struct wait_queue_entry *entry )
{
    struct mutex_sync *mutex = (struct mutex_sync *)obj;
    assert( obj->ops == &mutex_sync_ops );
    if (mutex->usync) return usync_mutex_signaled( mutex->usync, get_wait_queue_thread( entry )->id );
    return (!mutex->count || (mutex->owner == get_wait_queue_thread( entry )));
}

static void mutex_sync_satisfied( struct object *obj, struct wait_queue_entry *entry )
{
    struct mutex_sync *mutex = (struct mutex_sync *)obj;
    int abandoned;

    assert( obj->ops == &mutex_sync_ops );

    if (mutex->usync)
    {
        if (usync_mutex_satisfied( mutex->usync, get_wait_queue_thread( entry )->id, &abandoned ) &&
            abandoned)
            make_wait_abandoned( entry );
        return;
    }

    do_grab( mutex, get_wait_queue_thread( entry ));
    if (mutex->abandoned) make_wait_abandoned( entry );
    mutex->abandoned = 0;
}

static struct object *create_mutex_sync( int owned, int inproc )
{
    struct mutex_sync *mutex;

    if (!(mutex = alloc_object( &mutex_sync_ops ))) return NULL;
    mutex->count = 0;
    mutex->owner = NULL;
    mutex->abandoned = 0;
    list_init( &mutex->usync_entry );
    mutex->usync = NULL;
    if (inproc && do_usync())
    {
        mutex->usync = create_usync( owned ? current->id : 0, owned ? 1 : 0, INPROC_SYNC_MUTEX );
        if (mutex->usync)
        {
            list_add_tail( &usync_mutexes, &mutex->usync_entry );
            return &mutex->obj;
        }
        clear_error();
    }
    if (owned) do_grab( mutex, current );

    return &mutex->obj;
}

struct mutex
{
    struct object       obj;             /* object header */
    struct object      *sync;            /* mutex sync object */
};

struct mutex_init_data
{
    int owned;
};

static void mutex_dump( struct object *obj, int verbose );
static bool mutex_init( struct object *obj, const void *init_data );
static struct object *mutex_get_sync( struct object *obj );
static int mutex_signal( struct object *obj, unsigned int access, int signal );
static void mutex_destroy( struct object *obj );

static const struct object_ops mutex_ops =
{
    .size     = sizeof(struct mutex),
    .type     = &mutex_type,
    .dump     = mutex_dump,
    .init     = mutex_init,
    .signal   = mutex_signal,
    .get_sync = mutex_get_sync,
    .destroy  = mutex_destroy,
};

void abandon_mutexes( struct thread *thread )
{
    struct list *ptr;

    while ((ptr = list_head( &thread->mutex_list )) != NULL)
    {
        struct mutex_sync *mutex = LIST_ENTRY( ptr, struct mutex_sync, entry );
        assert( mutex->owner == thread );
        mutex->abandoned = 1;
        do_release( mutex, thread, mutex->count );
    }

    LIST_FOR_EACH( ptr, &usync_mutexes )
    {
        struct mutex_sync *mutex = LIST_ENTRY( ptr, struct mutex_sync, usync_entry );

        if (usync_abandon_mutex( mutex->usync, thread->id ))
        {
            wake_up( &mutex->obj, 0 );
            usync_wake_waiters( usync_get_index( mutex->usync ) );
        }
    }

    abandon_inproc_mutexes( thread->id );
}

static void mutex_dump( struct object *obj, int verbose )
{
    struct mutex *mutex = (struct mutex *)obj;
    assert( obj->ops == &mutex_ops );
    mutex->sync->ops->dump( mutex->sync, verbose );
}

static bool mutex_init( struct object *obj, const void *init_data )
{
    struct mutex *mutex = (struct mutex *)obj;
    const struct mutex_init_data *data = init_data;

    return !!(mutex->sync = create_mutex_sync( data->owned, 1 ));
}

static struct object *mutex_get_sync( struct object *obj )
{
    struct mutex *mutex = (struct mutex *)obj;
    assert( obj->ops == &mutex_ops );
    return grab_object( mutex->sync );
}

static int mutex_signal( struct object *obj, unsigned int access, int signal )
{
    struct mutex *mutex = (struct mutex *)obj;
    assert( obj->ops == &mutex_ops );

    assert( mutex->sync->ops == &mutex_sync_ops ); /* never called with inproc syncs */
    assert( signal == -1 ); /* always called from signal_object */

    if (!(access & SYNCHRONIZE))
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }
    if (((struct mutex_sync *)mutex->sync)->usync)
    {
        unsigned int prev;

        if (!usync_release_mutex( ((struct mutex_sync *)mutex->sync)->usync, current->id, &prev ))
            return 0;
        if (prev == 1)
        {
            wake_up( mutex->sync, 0 );
            usync_wake_waiters( usync_get_index( ((struct mutex_sync *)mutex->sync)->usync ) );
        }
        return 1;
    }
    return do_release( (struct mutex_sync *)mutex->sync, current, 1 );
}

static void mutex_destroy( struct object *obj )
{
    struct mutex *mutex = (struct mutex *)obj;
    assert( obj->ops == &mutex_ops );
    if (mutex->sync) release_object( mutex->sync );
}

int get_mutex_usync_idx( struct object *obj, int *type )
{
    struct mutex_sync *sync;
    struct mutex *mutex;

    if (!do_usync() || !obj || obj->ops != &mutex_ops) return -1;
    mutex = (struct mutex *)obj;
    if (!mutex->sync || mutex->sync->ops != &mutex_sync_ops) return -1;
    sync = (struct mutex_sync *)mutex->sync;
    if (!sync->usync) return -1;

    *type = INPROC_SYNC_MUTEX;
    return usync_get_index( sync->usync );
}

/* create a mutex */
DECL_HANDLER(create_mutex)
{
    struct mutex_init_data data = { .owned = req->owned };
    struct object_params params = { .ops = &mutex_ops, .access = req->access, .init_data = &data };

    if (!get_req_object_attributes( &params )) return;
    reply->handle = create_named_obj_handle( current->process, &params );
    if (params.root) release_object( params.root );
}

/* open a handle to a mutex */
DECL_HANDLER(open_mutex)
{
    reply->handle = open_object( current->process, req->rootdir, req->access,
                                 &mutex_ops, get_req_unicode_str(), req->attributes );
}

/* release a mutex */
DECL_HANDLER(release_mutex)
{
    struct mutex *mutex;

    if ((mutex = (struct mutex *)get_handle_obj( current->process, req->handle,
                                                 0, &mutex_ops )))
    {
        struct mutex_sync *sync = (struct mutex_sync *)mutex->sync;
        assert( mutex->sync->ops == &mutex_sync_ops ); /* never called with inproc syncs */

        if (sync->usync)
        {
            if (usync_release_mutex( sync->usync, current->id, &reply->prev_count ) &&
                reply->prev_count == 1)
            {
                wake_up( mutex->sync, 0 );
                usync_wake_waiters( usync_get_index( sync->usync ) );
            }
        }
        else
        {
            reply->prev_count = sync->count;
            do_release( sync, current, 1 );
        }
        release_object( mutex );
    }
}

/* return details about the mutex */
DECL_HANDLER(query_mutex)
{
    struct mutex *mutex;

    if ((mutex = (struct mutex *)get_handle_obj( current->process, req->handle,
                                                 MUTANT_QUERY_STATE, &mutex_ops )))
    {
        struct mutex_sync *sync = (struct mutex_sync *)mutex->sync;
        assert( mutex->sync->ops == &mutex_sync_ops ); /* never called with inproc syncs */

        if (sync->usync)
            usync_get_mutex_state( sync->usync, current->id, &reply->count,
                                     &reply->owned, &reply->abandoned );
        else
        {
            reply->count = sync->count;
            reply->owned = (sync->owner == current);
            reply->abandoned = sync->abandoned;
        }

        release_object( mutex );
    }
}
