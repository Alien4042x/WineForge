/*
 * Process-local graphics backend selection
 *
 * WineForge-Internal: graphics/process-backend-policy-v1.
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"
#include "unix_private.h"

#if defined(__APPLE__) && defined(__x86_64__)

WINE_DEFAULT_DEBUG_CHANNEL(module);

/* WineForge-Internal: graphics/process-backend-policy-v1. */
enum process_graphics_backend
{
    PROCESS_GRAPHICS_BACKEND_UNINITIALIZED,
    PROCESS_GRAPHICS_BACKEND_WINE,
    PROCESS_GRAPHICS_BACKEND_D3DMETAL,
    PROCESS_GRAPHICS_BACKEND_DXMT,
};

/* WineForge-Internal: graphics/process-backend-policy-v1. */
static enum process_graphics_backend process_graphics_backend;
static BOOL process_uses_cef_software;
static BOOL process_uses_wfdx_launchers;
static BOOL process_uses_launcher_dxmt;
static BOOL process_uses_battlenet_cef_cow;

/* WineForge-Internal: graphics/process-backend-policy-v1. */
static BOOL parse_graphics_backend( const char *name, enum process_graphics_backend *backend )
{
    if (name && !strcmp( name, "d3dmetal" ))
    {
        *backend = PROCESS_GRAPHICS_BACKEND_D3DMETAL;
        return TRUE;
    }
    if (name && !strcmp( name, "dxmt" ))
    {
        *backend = PROCESS_GRAPHICS_BACKEND_DXMT;
        return TRUE;
    }
    if (name && !strcmp( name, "wine" ))
    {
        *backend = PROCESS_GRAPHICS_BACKEND_WINE;
        return TRUE;
    }
    return FALSE;
}

/* WineForge-Internal: graphics/process-backend-policy-v1. */
static const char *process_graphics_backend_name( enum process_graphics_backend backend )
{
    switch (backend)
    {
    case PROCESS_GRAPHICS_BACKEND_D3DMETAL: return "d3dmetal";
    case PROCESS_GRAPHICS_BACKEND_DXMT: return "dxmt";
    default: return "wine";
    }
}

/* WineForge-Internal: graphics/process-backend-policy-v1. */
void init_process_graphics_backend( const WCHAR *image_path )
{
    const char *backend = getenv( "GRAPHICS_BACKEND" );
    const WCHAR *image_name = image_path;
    const WCHAR *p;
    char image_pathA[PATH_MAX];
    char image_nameA[MAX_PATH];
    enum process_graphics_backend configured_backend;
    struct launcher_policy launcher;
    int path_len, name_len;

    if (!backend || !backend[0]) backend = getenv( "ACTIVE_GRAPHICS_BACKEND" );
    if (image_path)
    {
        for (p = image_path; *p; p++)
            if (*p == '\\' || *p == '/') image_name = p + 1;
    }
    path_len = image_path ? ntdll_wcstoumbs( image_path, wcslen( image_path ), image_pathA,
                                             sizeof(image_pathA) - 1, FALSE ) : 0;
    if (path_len < 0) path_len = 0;
    image_pathA[path_len] = 0;
    name_len = image_name ? ntdll_wcstoumbs( image_name, wcslen( image_name ), image_nameA,
                                             sizeof(image_nameA) - 1, FALSE ) : 0;
    if (name_len < 0) name_len = 0;
    image_nameA[name_len] = 0;

    if (!parse_graphics_backend( backend, &configured_backend ))
        configured_backend = PROCESS_GRAPHICS_BACKEND_WINE;
    process_graphics_backend = configured_backend;
    get_launcher_policy( image_pathA, &launcher );

    /* WineForge-Internal: launcher-compat/cef-software-policy-v1. */
    process_uses_cef_software = configured_backend == PROCESS_GRAPHICS_BACKEND_DXMT &&
                                launcher.cef_software;

    /* WineForge-Internal: launcher-compat/battlenet-cef-cow-protection-v1. */
    process_uses_battlenet_cef_cow = launcher.battlenet_cef_cow;

    /* WineForge-Internal: launcher-compat/dxmt-cef-launcher-policy-v1. */
    process_uses_launcher_dxmt = launcher.dxmt && dxmt_runtime_available();
    if (process_uses_launcher_dxmt)
        process_graphics_backend = PROCESS_GRAPHICS_BACKEND_DXMT;

    /* WineForge-Internal: wfdxcompat/rockstar-launcher-policy-v1. */
    process_uses_wfdx_launchers = FALSE;
    if (launcher.wfdx)
    {
        process_graphics_backend = PROCESS_GRAPHICS_BACKEND_D3DMETAL;
        if (wfdxcompat_launcher_runtime_available())
            process_uses_wfdx_launchers = TRUE;
        else
            process_graphics_backend = configured_backend;
    }

    TRACE( "graphics backend for %s: global %s, selected %s, policy %s%s%s\n", image_nameA,
           backend && backend[0] ? backend : "wine",
           process_graphics_backend_name( process_graphics_backend ),
           process_uses_cef_software ? "cef-software" : "default",
           process_uses_launcher_dxmt ? ",launcher-dxmt" : "",
           process_uses_wfdx_launchers ? ",wfdx-launchers" : "" );
}

/* WineForge-Internal: graphics/process-backend-policy-v1. */
BOOL d3dmetal_graphics_backend_enabled(void)
{
    const char *backend = getenv( "GRAPHICS_BACKEND" );

    if (process_uses_cef_software) return FALSE;
    if (process_graphics_backend != PROCESS_GRAPHICS_BACKEND_UNINITIALIZED)
        return process_graphics_backend == PROCESS_GRAPHICS_BACKEND_D3DMETAL;
    if (!backend || !backend[0]) backend = getenv( "ACTIVE_GRAPHICS_BACKEND" );
    return backend && !strcmp( backend, "d3dmetal" );
}

/* WineForge-Internal: graphics/process-backend-policy-v1. */
BOOL dxmt_graphics_backend_enabled(void)
{
    const char *backend = getenv( "GRAPHICS_BACKEND" );

    if (process_uses_cef_software) return FALSE;
    if (process_graphics_backend != PROCESS_GRAPHICS_BACKEND_UNINITIALIZED)
        return process_graphics_backend == PROCESS_GRAPHICS_BACKEND_DXMT;
    if (!backend || !backend[0]) backend = getenv( "ACTIVE_GRAPHICS_BACKEND" );
    return backend && !strcmp( backend, "dxmt" );
}

/* WineForge-Internal: graphics/process-backend-policy-v1. */
BOOL wfdxcompat_launcher_runtime_enabled(void)
{
    return process_uses_wfdx_launchers;
}

/* WineForge-Internal: graphics/process-backend-policy-v1. */
BOOL battlenet_cef_cow_compat_enabled(void)
{
    return process_uses_battlenet_cef_cow;
}

/* WineForge-Internal: graphics/process-backend-policy-v1. */
BOOL cef_software_policy_enabled(void)
{
    return process_uses_cef_software;
}

#endif
