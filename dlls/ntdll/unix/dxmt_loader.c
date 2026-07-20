/*
 * DXMT runtime loader helpers
 *
 * WineForge-Internal: dxmt-runtime-loader-paths-v1.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "windef.h"
#include "unix_private.h"

#if defined(__APPLE__) && defined(__x86_64__)

static const char *get_dxmt_runtime_dir(void)
{
    return getenv( dxmt_cef_runtime_enabled() ? "DXMT_CEF_RUNTIME_DIR" : "DXMT_RUNTIME_DIR" );
}

static const char *const dxmt_module_names[] =
{
    "dxgi.dll",
    "d3d10core.dll",
    "d3d11.dll",
    "nvapi64.dll",
    "nvngx.dll",
    "winemetal.dll",
};

BOOL is_dxmt_module_name( const char *path )
{
    const char *name = strrchr( path, '/' );
    unsigned int i;

    name = name ? name + 1 : path;
    for (i = 0; i < ARRAY_SIZE(dxmt_module_names); i++)
        if (!strcasecmp( name, dxmt_module_names[i] )) return TRUE;
    return FALSE;
}

BOOL is_dxmt_module_basename( const WCHAR *name )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(dxmt_module_names); i++)
    {
        const char *module = dxmt_module_names[i];
        size_t len = strlen( module ) - 4;
        size_t pos;

        for (pos = 0; pos < len && name[pos]; pos++)
        {
            WCHAR left = name[pos];
            unsigned char right = module[pos];

            if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
            if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
            if (left != right) break;
        }
        if (pos == len && !name[pos]) return TRUE;
    }
    return FALSE;
}

char *resolve_dxmt_pe_path( const char *module, USHORT machine )
{
    const char *runtime_dir = get_dxmt_runtime_dir();
    const char *name = strrchr( module, '/' );
    const char *pe_dir;
    char *resolved;

    if (!dxmt_graphics_backend_enabled() || !runtime_dir || !runtime_dir[0]) return NULL;
    if (machine == IMAGE_FILE_MACHINE_I386) pe_dir = "i386-windows";
    else if (machine == IMAGE_FILE_MACHINE_AMD64) pe_dir = "x86_64-windows";
    else return NULL;

    name = name ? name + 1 : module;
    if (asprintf( &resolved, "%s/%s/%s", runtime_dir, pe_dir, name ) == -1) return NULL;
    if (!access( resolved, R_OK )) return resolved;
    free( resolved );
    return NULL;
}

char *resolve_dxmt_unixlib_path( const char *path )
{
    const char *runtime_dir = get_dxmt_runtime_dir();
    const char *name = strrchr( path, '/' );
    char *resolved;

    if (!dxmt_graphics_backend_enabled() || !runtime_dir || !runtime_dir[0]) return NULL;
    name = name ? name + 1 : path;
    if (strcasecmp( name, "winemetal.so" )) return NULL;

    if (asprintf( &resolved, "%s/x86_64-unix/winemetal.so", runtime_dir ) == -1) return NULL;
    if (!access( resolved, R_OK )) return resolved;
    free( resolved );
    return NULL;
}

#endif
