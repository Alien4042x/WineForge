/*
 * D3DMetal runtime loader helpers
 *
 * WineForge-Internal: d3dmetal-runtime-loader-paths-v1.
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

char *get_d3dmetal_dll_path(void)
{
    const char *runtime_dir = getenv( "D3DMETAL_RUNTIME_DIR" );
    char *path;

    if (!d3dmetal_graphics_backend_enabled() || !runtime_dir || !runtime_dir[0]) return NULL;
    if (asprintf( &path, "%s/wine", runtime_dir ) == -1) return NULL;
    if (!access( path, R_OK )) return path;
    free( path );
    return NULL;
}

BOOL is_d3dmetal_unixlib_name( const char *path )
{
    const char *name = strrchr( path, '/' );

    name = name ? name + 1 : path;
    return !strcmp( name, "dxgi.so" ) ||
           !strcmp( name, "d3d10.so" ) ||
           !strcmp( name, "d3d10core.so" ) ||
           !strcmp( name, "d3d11.so" ) ||
           !strcmp( name, "d3d12.so" ) ||
           !strcmp( name, "nvapi.so" ) ||
           !strcmp( name, "nvapi64.so" ) ||
           !strcmp( name, "nvngx.so" ) ||
           !strcmp( name, "nvngx-on-metalfx.so" );
}

static char *try_d3dmetal_unixlib_path( const char *dir, const char *name )
{
    char *resolved;

    if (!dir || !dir[0]) return NULL;
    if (asprintf( &resolved, "%s/%s", dir, name ) == -1) return NULL;
    if (!access( resolved, R_OK )) return resolved;
    free( resolved );
    return NULL;
}

BOOL is_d3dmetal_module_name( const char *path )
{
    const char *name = strrchr( path, '/' );

    name = name ? name + 1 : path;
    return !strcasecmp( name, "dxgi.dll" ) ||
           !strcasecmp( name, "d3d10.dll" ) ||
           !strcasecmp( name, "d3d10core.dll" ) ||
           !strcasecmp( name, "d3d11.dll" ) ||
           !strcasecmp( name, "d3d12.dll" ) ||
           !strcasecmp( name, "atidxx64.dll" ) ||
           !strcasecmp( name, "nvapi.dll" ) ||
           !strcasecmp( name, "nvapi64.dll" ) ||
           !strcasecmp( name, "nvngx.dll" ) ||
           !strcasecmp( name, "nvngx-on-metalfx.dll" );
}

char *resolve_d3dmetal_pe_path( const char *module )
{
    const char *runtime_dir = getenv( "D3DMETAL_RUNTIME_DIR" );
    const char *name = strrchr( module, '/' );
    char *resolved;

    if (!d3dmetal_graphics_backend_enabled() || !runtime_dir || !runtime_dir[0]) return NULL;
    name = name ? name + 1 : module;

    if (!strcasecmp( name, "nvapi.dll" )) name = "nvapi64.dll";
    else if (!strcasecmp( name, "nvngx.dll" ))
    {
        if (asprintf( &resolved, "%s/wine/x86_64-windows/nvngx-on-metalfx.dll", runtime_dir ) == -1)
            return NULL;
        if (!access( resolved, R_OK )) return resolved;
        free( resolved );
        name = "nvngx.dll";
    }

    if (asprintf( &resolved, "%s/wine/x86_64-windows/%s", runtime_dir, name ) == -1) return NULL;
    if (!access( resolved, R_OK )) return resolved;
    free( resolved );
    return NULL;
}

char *resolve_d3dmetal_unixlib_path( const char *path )
{
    const char *name = strrchr( path, '/' );
    const char *dir = getenv( "D3DMETAL_UNIXLIB_DIR" );
    const char *runtime_dir;
    char *resolved;

    if (!d3dmetal_graphics_backend_enabled()) return NULL;
    name = name ? name + 1 : path;
    if (!strcmp( name, "nvapi.so" )) name = "nvapi64.so";
    if ((resolved = try_d3dmetal_unixlib_path( dir, name ))) return resolved;

    runtime_dir = getenv( "D3DMETAL_RUNTIME_DIR" );
    if (runtime_dir && runtime_dir[0])
    {
        char *unixlib_dir;

        if (asprintf( &unixlib_dir, "%s/wine/x86_64-unix", runtime_dir ) != -1)
        {
            resolved = try_d3dmetal_unixlib_path( unixlib_dir, name );
            free( unixlib_dir );
            if (resolved) return resolved;
        }
    }

    return NULL;
}

#endif
