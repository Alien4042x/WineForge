/*
 * WineForge PE runtime aliases and process-local launcher integration.
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"
#include "wine/debug.h"
#include "wfdxcompat_loader.h"

WINE_DEFAULT_DEBUG_CHANNEL(module);

#ifdef _WIN64
static NTSTATUS get_runtime_env( const WCHAR *name, SIZE_T extra, UNICODE_STRING *ret )
{
    NTSTATUS status;
    SIZE_T len, size = 1024 + extra;

    for (;;)
    {
        if (!(ret->Buffer = RtlAllocateHeap( GetProcessHeap(), 0, size * sizeof(WCHAR) )))
            return STATUS_NO_MEMORY;
        status = RtlQueryEnvironmentVariable( NULL, name, wcslen(name),
                                              ret->Buffer, size - extra - 1, &len );
        if (!status)
        {
            ret->Buffer[len] = 0;
            ret->Length = len * sizeof(WCHAR);
            ret->MaximumLength = size * sizeof(WCHAR);
            return status;
        }
        RtlFreeHeap( GetProcessHeap(), 0, ret->Buffer );
        ret->Buffer = NULL;
        if (status != STATUS_BUFFER_TOO_SMALL) return status;
        size = len + 1 + extra;
    }
}

/* WineForge-Internal: wfdxcompat-runtime-loader-v1.
 * Resolve private PE aliases; opening and tracking modules stays in loader.c. */
NTSTATUS wfdxcompat_get_runtime_alias( const WCHAR *name, UNICODE_STRING *nt_name )
{
    static const WCHAR unix_prefixW[] = L"\\??\\unix";
    static const WCHAR runtime_envW[] = L"WFDXCOMPAT_RUNTIME_DIR";
    static const WCHAR d3dmetal_envW[] = L"D3DMETAL_RUNTIME_DIR";
    static const WCHAR runtime_nameW[] = L"/wfdxcompat";
    static const WCHAR backend_suffixW[] = L"/x86_64-windows/wfdxbackend-d3d12.dll";
    static const WCHAR launcher_suffixW[] = L"/x86_64-windows/wfdx-launchers-v1.dll";
    const WCHAR *suffix;
    UNICODE_STRING runtime;
    WCHAR *end, *slash;
    SIZE_T length;
    NTSTATUS status;
    BOOL derived = FALSE;

    if (!wcsicmp( name, L"wfdxbackend-d3d12.dll" )) suffix = backend_suffixW;
    else if (!wcsicmp( name, L"wfdx-launchers-v1.dll" )) suffix = launcher_suffixW;
    else return STATUS_DLL_NOT_FOUND;

    if (get_runtime_env( runtime_envW, ARRAY_SIZE(runtime_nameW) + wcslen(suffix) + 1, &runtime ))
    {
        if ((status = get_runtime_env( d3dmetal_envW,
                                   ARRAY_SIZE(runtime_nameW) + wcslen(suffix) + 1, &runtime )))
            return status == STATUS_VARIABLE_NOT_FOUND ? STATUS_DLL_NOT_FOUND : status;
        derived = TRUE;
    }

    end = runtime.Buffer + runtime.Length / sizeof(WCHAR);
    while (end > runtime.Buffer + 1 && end[-1] == '/') *--end = 0;
    if (derived)
    {
        if (!(slash = wcsrchr( runtime.Buffer, '/' )) || slash == runtime.Buffer)
        {
            RtlFreeUnicodeString( &runtime );
            return STATUS_DLL_NOT_FOUND;
        }
        *slash = 0;
        wcscat( runtime.Buffer, runtime_nameW );
    }

    length = wcslen( unix_prefixW ) + wcslen( runtime.Buffer ) + wcslen( suffix );
    if (!(nt_name->Buffer = RtlAllocateHeap( GetProcessHeap(), 0,
                                             (length + 1) * sizeof(WCHAR) )))
    {
        RtlFreeUnicodeString( &runtime );
        return STATUS_NO_MEMORY;
    }
    wcscpy( nt_name->Buffer, unix_prefixW );
    wcscat( nt_name->Buffer, runtime.Buffer );
    wcscat( nt_name->Buffer, suffix );
    nt_name->Length = length * sizeof(WCHAR);
    nt_name->MaximumLength = (length + 1) * sizeof(WCHAR);
    RtlFreeUnicodeString( &runtime );

    return STATUS_SUCCESS;
}
#endif

/* WineForge-Internal: wfdxcompat/launcher-process-interposition-v1.
 * State is private to this process; loader callbacks hold the loader lock. */
static HMODULE wfdx_launcher_d3d11;
static HMODULE wfdx_launcher_dxgi;
static HMODULE wfdx_launcher_companion;
static void *wfdx_launcher_create_device;
static void *wfdx_launcher_create_device_and_swapchain;
static void *wfdx_launcher_dxgi_d3d10_create_device;
static BOOL wfdx_launcher_interposition_active;

static BOOL wfdx_launcher_path_has_suffix( const UNICODE_STRING *path, const WCHAR *suffix )
{
    unsigned int path_len = path->Length / sizeof(WCHAR);
    unsigned int suffix_len = wcslen( suffix );
    unsigned int i;

    if (path_len < suffix_len) return FALSE;
    for (i = 0; i < suffix_len; ++i)
    {
        WCHAR a = path->Buffer[path_len - suffix_len + i];
        WCHAR b = suffix[i];

        if (a == '/') a = '\\';
        if (b == '/') b = '\\';
        if (towupper( a ) != towupper( b )) return FALSE;
    }
    return TRUE;
}

static BOOL wfdx_launcher_env_present( const WCHAR *name_str )
{
    WCHAR buffer[2];
    UNICODE_STRING name, value;
    NTSTATUS status;

    RtlInitUnicodeString( &name, name_str );
    value.Buffer = buffer;
    value.Length = 0;
    value.MaximumLength = sizeof(buffer);
    status = RtlQueryEnvironmentVariable_U( NULL, &name, &value );
    return status == STATUS_SUCCESS || status == STATUS_BUFFER_TOO_SMALL;
}

static BOOL wfdx_launcher_process_enabled(void)
{
    const UNICODE_STRING *path = &NtCurrentTeb()->Peb->ProcessParameters->ImagePathName;
    BOOL rockstar = wfdx_launcher_path_has_suffix( path,
            L"\\Rockstar Games\\Launcher\\Launcher.exe" ) ||
            wfdx_launcher_path_has_suffix( path,
            L"\\Rockstar Games\\Social Club\\SocialClubHelper.exe" );

    return rockstar && wfdx_launcher_env_present( L"WFDXCOMPAT_RUNTIME_DIR" );
}

static BOOL wfdx_launcher_export_name( const ANSI_STRING *name, const char *expected )
{
    unsigned int len = strlen( expected );
    return name && name->Length == len && !memcmp( name->Buffer, expected, len );
}

void wfdx_launcher_activate( HMODULE d3d11 )
{
    struct wfdx_launcher_init
    {
        UINT size;
        UINT abi_version;
        UINT flags;
        UINT reserved;
        void *create_device;
        void *create_device_and_swapchain;
    } init_data;
    static const WCHAR companion_nameW[] = L"wfdx-launchers-v1.dll";
    static const WCHAR dxgi_nameW[] = L"dxgi.dll";
    static const char create_nameA[] = "D3D11CreateDevice";
    static const char create_sc_nameA[] = "D3D11CreateDeviceAndSwapChain";
    static const char init_nameA[] = "wfdx_launcher_init";
    static const char wrapper_nameA[] = "wfdx_launcher_D3D11CreateDevice";
    static const char wrapper_sc_nameA[] = "wfdx_launcher_D3D11CreateDeviceAndSwapChain";
    static const char wrapper_d3d10_nameA[] = "wfdx_launcher_DXGID3D10CreateDevice";
    UNICODE_STRING companion_name;
    UNICODE_STRING dxgi_name;
    ANSI_STRING name;
    HMODULE companion = NULL;
    void *original_create = NULL, *original_create_sc = NULL;
    void *wrapper_create = NULL, *wrapper_create_sc = NULL;
    void *wrapper_d3d10 = NULL, *init_proc = NULL;
    HMODULE dxgi = NULL;
    BOOL (WINAPI *init)(const struct wfdx_launcher_init *);
    NTSTATUS status;

    if (wfdx_launcher_interposition_active || wfdx_launcher_companion ||
        !wfdx_launcher_process_enabled())
        return;

    RtlInitAnsiString( &name, create_nameA );
    if (LdrGetProcedureAddress( d3d11, &name, 0, &original_create ))
        return;
    RtlInitAnsiString( &name, create_sc_nameA );
    if (LdrGetProcedureAddress( d3d11, &name, 0, &original_create_sc ))
        return;

    RtlInitUnicodeString( &companion_name, companion_nameW );
    status = LdrLoadDll( NULL, NULL, &companion_name, &companion );
    if (status) return;

    RtlInitAnsiString( &name, init_nameA );
    status = LdrGetProcedureAddress( companion, &name, 0, &init_proc );
    if (!status)
    {
        RtlInitAnsiString( &name, wrapper_nameA );
        status = LdrGetProcedureAddress( companion, &name, 0, &wrapper_create );
    }
    if (!status)
    {
        RtlInitAnsiString( &name, wrapper_sc_nameA );
        status = LdrGetProcedureAddress( companion, &name, 0, &wrapper_create_sc );
    }
    if (!status)
    {
        RtlInitAnsiString( &name, wrapper_d3d10_nameA );
        status = LdrGetProcedureAddress( companion, &name, 0, &wrapper_d3d10 );
    }
    if (!status)
    {
        RtlInitUnicodeString( &dxgi_name, dxgi_nameW );
        status = LdrGetDllHandle( NULL, 0, &dxgi_name, &dxgi );
    }
    if (status)
    {
        LdrUnloadDll( companion );
        return;
    }

    init = init_proc;
    init_data.size = sizeof(init_data);
    init_data.abi_version = 1;
    init_data.flags = 0x00000001 | 0x00000002 | 0x00000004;
    init_data.reserved = 0;
    init_data.create_device = original_create;
    init_data.create_device_and_swapchain = original_create_sc;
    if (!init( &init_data ))
    {
        LdrUnloadDll( companion );
        return;
    }

    wfdx_launcher_d3d11 = d3d11;
    wfdx_launcher_dxgi = dxgi;
    wfdx_launcher_companion = companion;
    wfdx_launcher_create_device = wrapper_create;
    wfdx_launcher_create_device_and_swapchain = wrapper_create_sc;
    wfdx_launcher_dxgi_d3d10_create_device = wrapper_d3d10;
    wfdx_launcher_interposition_active = TRUE;
    TRACE( "activated Rockstar D3D11 companion %p for canonical backend %p\n",
            companion, d3d11 );
}

/* WineForge-Internal: wfdxcompat/launcher-process-interposition-v1. */
void *wfdx_launcher_get_import( HMODULE module, const char *name )
{
    if (wfdx_launcher_interposition_active && module == wfdx_launcher_dxgi &&
        !strcmp( name, "DXGID3D10CreateDevice" ))
        return wfdx_launcher_dxgi_d3d10_create_device;
    return NULL;
}

/* WineForge-Internal: wfdxcompat/launcher-process-interposition-v1. */
void *wfdx_launcher_get_proc( HMODULE module, const ANSI_STRING *name )
{
    if (!wfdx_launcher_interposition_active) return NULL;
    if (module == wfdx_launcher_d3d11)
    {
        if (wfdx_launcher_export_name( name, "D3D11CreateDevice" ))
            return wfdx_launcher_create_device;
        if (wfdx_launcher_export_name( name, "D3D11CreateDeviceAndSwapChain" ))
            return wfdx_launcher_create_device_and_swapchain;
    }
    else if (module == wfdx_launcher_dxgi &&
             wfdx_launcher_export_name( name, "DXGID3D10CreateDevice" ))
        return wfdx_launcher_dxgi_d3d10_create_device;
    return NULL;
}
