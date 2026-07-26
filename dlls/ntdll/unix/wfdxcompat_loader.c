/*
 * WineForge DXCompat runtime loader helpers
 *
 * WineForge-Internal: wfdxcompat-runtime-loader-v1.
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
#include "winnt.h"
#include "unix_private.h"

#if defined(__APPLE__) && defined(__x86_64__)

static const char *module_basename( const char *path )
{
    const char *name = strrchr( path, '/' );
    return name ? name + 1 : path;
}

static char *derive_runtime_dir( const char *backend_dir )
{
    char *copy, *end, *slash, *runtime_dir;

    if (!backend_dir || !backend_dir[0] || !(copy = strdup( backend_dir ))) return NULL;
    end = copy + strlen( copy );
    while (end > copy + 1 && end[-1] == '/') *--end = 0;
    if (!(slash = strrchr( copy, '/' )) || slash == copy)
    {
        free( copy );
        return NULL;
    }
    *slash = 0;
    if (asprintf( &runtime_dir, "%s/wfdxcompat", copy ) == -1) runtime_dir = NULL;
    free( copy );
    return runtime_dir;
}

static char *get_wfdxcompat_runtime_dir(void)
{
    const char *runtime_dir = getenv( "WFDXCOMPAT_RUNTIME_DIR" );

    if (runtime_dir && runtime_dir[0]) return strdup( runtime_dir );
    if ((runtime_dir = getenv( "D3DMETAL_RUNTIME_DIR" ))) return derive_runtime_dir( runtime_dir );
    return NULL;
}

BOOL is_wfdxcompat_frontend_module_name( const char *path )
{
    const char *name = module_basename( path );

    return !strcasecmp( name, "d3d12.dll" ) ||
           (wfdxcompat_launcher_runtime_enabled() &&
            !strcasecmp( name, "wfdx-launchers-v1.dll" ));
}

BOOL is_wfdxcompat_backend_module_name( const char *path )
{
    const char *name = module_basename( path );

    return !strcasecmp( name, "wfdxbackend-d3d12.dll" ) ||
           !strcasecmp( name, "wfdxbackend-d3d12.so" );
}

BOOL wfdxcompat_launcher_runtime_available(void)
{
    char *runtime_dir, *companion = NULL;
    char *backend_pe = NULL, *backend_unix = NULL;
    BOOL available = FALSE;

    if (!(runtime_dir = get_wfdxcompat_runtime_dir())) return FALSE;
    if (asprintf( &companion, "%s/x86_64-windows/wfdx-launchers-v1.dll",
                  runtime_dir ) == -1) companion = NULL;
    backend_pe = resolve_d3dmetal_pe_path( "d3d11.dll" );
    backend_unix = resolve_d3dmetal_unixlib_path( "d3d11.so" );
    available = companion && backend_pe && backend_unix &&
                !access( companion, R_OK ) &&
                !access( backend_pe, R_OK ) && !access( backend_unix, R_OK );
    free( backend_unix );
    free( backend_pe );
    free( companion );
    free( runtime_dir );
    return available;
}

char *resolve_wfdxcompat_frontend_pe_path( const char *module, USHORT machine )
{
    const char *name = module_basename( module );
    char *runtime_dir, *resolved;

    if (!d3dmetal_graphics_backend_enabled() || machine != IMAGE_FILE_MACHINE_AMD64 ||
        !is_wfdxcompat_frontend_module_name( name )) return NULL;
    if (!(runtime_dir = get_wfdxcompat_runtime_dir())) return NULL;
    if (asprintf( &resolved, "%s/x86_64-windows/%s", runtime_dir, name ) == -1) resolved = NULL;
    free( runtime_dir );
    if (!resolved) return NULL;
    if (!access( resolved, R_OK )) return resolved;
    free( resolved );
    return NULL;
}

char *resolve_wfdxcompat_backend_pe_path( const char *module )
{
    if (!is_wfdxcompat_backend_module_name( module ) || !d3dmetal_graphics_backend_enabled()) return NULL;
    return resolve_d3dmetal_pe_path( "d3d12.dll" );
}

char *resolve_wfdxcompat_backend_unixlib_path( const char *module )
{
    if (!is_wfdxcompat_backend_module_name( module ) || !d3dmetal_graphics_backend_enabled()) return NULL;
    return resolve_d3dmetal_unixlib_path( "d3d12.so" );
}

#endif
