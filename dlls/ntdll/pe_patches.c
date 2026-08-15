/*
 * Verified in-memory compatibility patches for native PE modules.
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

#include <stdarg.h>
#include <string.h>

#include "ntstatus.h"
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "wine/debug.h"
#include "ntdll_misc.h"
#include "pe_patches.h"

WINE_DEFAULT_DEBUG_CHANNEL(module);

#if defined(__i386__) || defined(__x86_64__)

struct wineforge_patch_fragment
{
    DWORD rva;
    const BYTE *before;
    const BYTE *after;
    SIZE_T size;
};

struct wineforge_patch_set
{
    const char *identifier;
    const WCHAR *module_name;
    const WCHAR *path_suffix;
    WORD machine;
    SIZE_T image_size;
    DWORD timestamp;
    BOOL builtin;
    BOOL (*should_apply)(void);
    const struct wineforge_patch_fragment *fragments;
    unsigned int fragment_count;
    void (*custom_apply)(void);
};

#define WF_PATCH_SIZE(before, after) \
    ((sizeof(before) - 1) + 0 * sizeof(char[sizeof(before) == sizeof(after) ? 1 : -1]))

#define WF_PATCH_FRAGMENT(rva, before, after) \
    { (rva), (const BYTE *)(before), (const BYTE *)(after), WF_PATCH_SIZE(before, after) }

static BOOL path_has_suffix( const WCHAR *path, const WCHAR *suffix )
{
    SIZE_T path_len, suffix_len;

    if (!suffix) return TRUE;
    if (!path) return FALSE;
    path_len = wcslen( path );
    suffix_len = wcslen( suffix );
    return path_len >= suffix_len && !wcsicmp( path + path_len - suffix_len, suffix );
}

/* Ubisoft Connect CEF 135 command-line compatibility (i386). */
/* WineForge-Internal: launcher-compat/ubisoft-cef135-command-line-gate-v1. */
#ifdef __i386__
static BOOL process_image_path_has_suffix( const WCHAR *suffix )
{
    const UNICODE_STRING *path = &NtCurrentTeb()->Peb->ProcessParameters->ImagePathName;
    SIZE_T path_len = path->Length / sizeof(WCHAR);
    SIZE_T suffix_len = wcslen( suffix );

    return path->Buffer && path_len >= suffix_len &&
           !wcsnicmp( path->Buffer + path_len - suffix_len, suffix, suffix_len );
}

static BOOL ubisoft_connect_process(void)
{
    return process_image_path_has_suffix(
            L"\\Program Files (x86)\\Ubisoft\\Ubisoft Game Launcher\\upc.exe" );
}

static const struct wineforge_patch_fragment ubisoft_cef_fragments[] =
{
    WF_PATCH_FRAGMENT(
        0x11e92d,
        "\x8b\x47\x34\x89\x46\x34\x8b\x47\x38\x89\x46\x38",
        "\x8b\x47\x34\x89\x46\x34\x31\xc0\x90\x89\x46\x38"
    ),
};
#endif

/* Steam Overlay D3DMetal DXGI thunk compatibility (x86_64). */
/* WineForge-Internal: launcher-compat/steam-overlay-d3dmetal-thunks-v2. */
#ifdef __x86_64__
static const struct wineforge_patch_fragment steam_dxgi_generator_fragments[] =
{
    WF_PATCH_FRAGMENT( 0x13a4, "\xff\x25",             "\x48\xb8" ),
    WF_PATCH_FRAGMENT( 0x13ba, "\xad\x0b\x00\x00", "\x00\x00\xff\xe0" ),
    WF_PATCH_FRAGMENT( 0x13c3, "\xff\x25",             "\x48\xb8" ),
    WF_PATCH_FRAGMENT( 0x13d9, "\xad\x0b\x00\x00", "\x00\x00\xff\xe0" ),
    WF_PATCH_FRAGMENT( 0x1437, "\x08", "\x02" ),
    WF_PATCH_FRAGMENT( 0x1507, "\x08", "\x02" ),
    WF_PATCH_FRAGMENT( 0x15b7, "\x08", "\x02" ),
    WF_PATCH_FRAGMENT( 0x1667, "\x08", "\x02" ),
    WF_PATCH_FRAGMENT( 0x1717, "\x08", "\x02" ),
    WF_PATCH_FRAGMENT( 0x17d7, "\x08", "\x02" ),
};

static BOOL patch_steam_dxgi_generator( BYTE *base, SIZE_T image_size )
{
    BOOL all_before = TRUE, all_after = TRUE;
    SIZE_T first = ~(SIZE_T)0, last = 0, protect_size;
    void *protect_addr;
    ULONG old_prot;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(steam_dxgi_generator_fragments); ++i)
    {
        const struct wineforge_patch_fragment *fragment = &steam_dxgi_generator_fragments[i];
        const BYTE *target;

        if (fragment->rva > image_size || fragment->size > image_size - fragment->rva)
        {
            WARN( "Steam D3DMetal generator fragment at RVA %#lx is out of range\n",
                  fragment->rva );
            return FALSE;
        }

        target = base + fragment->rva;
        if (memcmp( target, fragment->before, fragment->size )) all_before = FALSE;
        if (memcmp( target, fragment->after, fragment->size )) all_after = FALSE;
        if (fragment->rva < first) first = fragment->rva;
        if (fragment->rva + fragment->size > last) last = fragment->rva + fragment->size;
    }

    if (all_after) return TRUE;
    if (!all_before)
    {
        WARN( "Steam D3DMetal generator signature does not match\n" );
        return FALSE;
    }

    protect_addr = base + first;
    protect_size = last - first;
    if (NtProtectVirtualMemory( NtCurrentProcess(), &protect_addr, &protect_size,
                                PAGE_EXECUTE_READWRITE, &old_prot ))
    {
        WARN( "failed to make Steam D3DMetal generator writable\n" );
        return FALSE;
    }

    for (i = 0; i < ARRAY_SIZE(steam_dxgi_generator_fragments); ++i)
    {
        const struct wineforge_patch_fragment *fragment = &steam_dxgi_generator_fragments[i];
        memcpy( base + fragment->rva, fragment->after, fragment->size );
    }

    NtFlushInstructionCache( NtCurrentProcess(), base + first, last - first );
    NtProtectVirtualMemory( NtCurrentProcess(), &protect_addr, &protect_size,
                            old_prot, &old_prot );
    return TRUE;
}

static BOOL patch_steam_dxgi_thunk_table( BYTE *base, SIZE_T image_size )
{
    static const BYTE old_prefix[] = {0xff, 0x25, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
    const SIZE_T table_rva = 0x01007000, thunk_count = 0x400, thunk_size = 0x10;
    SIZE_T table_size = thunk_count * thunk_size, protect_size = table_size;
    unsigned int old_count = 0, new_count = 0, other_count = 0;
    BYTE *table, *entry;
    void *protect_addr;
    ULONG old_prot;
    SIZE_T i;

    if (table_rva > image_size || table_size > image_size - table_rva)
    {
        WARN( "Steam D3DMetal thunk table is out of range\n" );
        return FALSE;
    }

    table = base + table_rva;
    for (i = 0; i < thunk_count; ++i)
    {
        entry = table + i * thunk_size;
        if (!memcmp( entry, old_prefix, sizeof(old_prefix) )) ++old_count;
        else if (entry[0] == 0x48 && entry[1] == 0xb8 &&
                 entry[10] == 0xff && entry[11] == 0xe0) ++new_count;
        else ++other_count;
    }

    if (!old_count)
    {
        if (new_count) return TRUE;
        WARN( "Steam D3DMetal thunk table has no recognized entries\n" );
        return FALSE;
    }

    protect_addr = table;
    if (NtProtectVirtualMemory( NtCurrentProcess(), &protect_addr, &protect_size,
                                PAGE_EXECUTE_READWRITE, &old_prot ))
    {
        WARN( "failed to make Steam D3DMetal thunk table writable\n" );
        return FALSE;
    }

    for (i = 0; i < thunk_count; ++i)
    {
        ULONGLONG target;

        entry = table + i * thunk_size;
        if (memcmp( entry, old_prefix, sizeof(old_prefix) )) continue;
        memcpy( &target, entry + 8, sizeof(target) );
        entry[0] = 0x48;
        entry[1] = 0xb8;
        memcpy( entry + 2, &target, sizeof(target) );
        entry[10] = 0xff;
        entry[11] = 0xe0;
        memset( entry + 12, 0x90, 4 );
    }

    NtFlushInstructionCache( NtCurrentProcess(), table, table_size );
    NtProtectVirtualMemory( NtCurrentProcess(), &protect_addr, &protect_size,
                            old_prot, &old_prot );
    TRACE( "patched %u existing Steam D3DMetal thunks; %u already compatible, %u skipped\n",
           old_count, new_count, other_count );
    return TRUE;
}

static void apply_steam_d3dmetal_dxgi_thunks(void)
{
    const LIST_ENTRY *head = &NtCurrentTeb()->Peb->LdrData->InMemoryOrderModuleList;
    const LIST_ENTRY *entry;

    for (entry = head->Flink; entry != head; entry = entry->Flink)
    {
        const LDR_DATA_TABLE_ENTRY *module =
            CONTAINING_RECORD( entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks );
        IMAGE_NT_HEADERS *nt;

        if (!module->BaseDllName.Buffer || wcsicmp( module->BaseDllName.Buffer, L"dxgi.dll" ))
            continue;
        if (module->SizeOfImage != 0x01025000 || !(nt = RtlImageNtHeader( module->DllBase )))
            continue;
        if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) continue;
        if (!patch_steam_dxgi_generator( module->DllBase, module->SizeOfImage )) return;
        patch_steam_dxgi_thunk_table( module->DllBase, module->SizeOfImage );
        return;
    }
    TRACE( "Steam renderer loaded without D3DMetal dxgi.dll\n" );
}
#endif

static const struct wineforge_patch_set patch_sets[] =
{
#ifdef __i386__
    {
        "launcher-compat/ubisoft-cef135-command-line-gate-v1",
        L"libcef.dll",
        NULL,
        IMAGE_FILE_MACHINE_I386,
        0,
        0,
        FALSE,
        ubisoft_connect_process,
        ubisoft_cef_fragments,
        ARRAY_SIZE(ubisoft_cef_fragments),
        NULL,
    },
#endif
#ifdef __x86_64__
    {
        "launcher-compat/steam-overlay-d3dmetal-thunks-v2",
        L"gameoverlayrenderer64.dll",
        L"\\Program Files (x86)\\Steam\\gameoverlayrenderer64.dll",
        IMAGE_FILE_MACHINE_AMD64,
        0,
        0,
        FALSE,
        NULL,
        NULL,
        0,
        apply_steam_d3dmetal_dxgi_thunks,
    },
#endif
    {0},
};

/* WineForge-Internal: ntdll/verified-pe-patch-manager-v1. */
static void apply_patch_set( const struct wineforge_patch_set *set, const WCHAR *module_path,
                             void *module_base, SIZE_T image_size, BOOL is_builtin )
{
    IMAGE_NT_HEADERS *nt = RtlImageNtHeader( module_base );
    BOOL all_before = TRUE, all_after = TRUE;
    SIZE_T first = ~(SIZE_T)0, last = 0, protect_size;
    void *protect_addr;
    ULONG old_prot;
    unsigned int i;

    if (!nt || nt->FileHeader.Machine != set->machine) return;
    if (is_builtin != set->builtin) return;
    if (!path_has_suffix( module_path, set->path_suffix )) return;
    if (set->image_size && image_size != set->image_size) return;
    if (set->timestamp && nt->FileHeader.TimeDateStamp != set->timestamp) return;
    if (set->should_apply && !set->should_apply()) return;
    if (set->custom_apply)
    {
        set->custom_apply();
        return;
    }

    for (i = 0; i < set->fragment_count; ++i)
    {
        const struct wineforge_patch_fragment *fragment = &set->fragments[i];
        const BYTE *target;

        if (fragment->rva > image_size || fragment->size > image_size - fragment->rva)
        {
            WARN( "%s has an out-of-range fragment at RVA %#lx\n",
                  set->identifier, fragment->rva );
            return;
        }

        target = (const BYTE *)module_base + fragment->rva;
        if (memcmp( target, fragment->before, fragment->size )) all_before = FALSE;
        if (memcmp( target, fragment->after, fragment->size )) all_after = FALSE;
        if (fragment->rva < first) first = fragment->rva;
        if (fragment->rva + fragment->size > last) last = fragment->rva + fragment->size;
    }

    if (all_after) return;
    if (!all_before)
    {
        WARN( "%s original-byte signature does not match\n", set->identifier );
        return;
    }

    protect_addr = (BYTE *)module_base + first;
    protect_size = last - first;
    if (NtProtectVirtualMemory( NtCurrentProcess(), &protect_addr, &protect_size,
                                PAGE_EXECUTE_READWRITE, &old_prot ))
    {
        WARN( "failed to make %s writable\n", set->identifier );
        return;
    }

    for (i = 0; i < set->fragment_count; ++i)
    {
        const struct wineforge_patch_fragment *fragment = &set->fragments[i];
        memcpy( (BYTE *)module_base + fragment->rva, fragment->after, fragment->size );
    }

    NtFlushInstructionCache( NtCurrentProcess(), (BYTE *)module_base + first, last - first );
    NtProtectVirtualMemory( NtCurrentProcess(), &protect_addr, &protect_size,
                            old_prot, &old_prot );
    TRACE( "applied %s\n", set->identifier );
}

void wineforge_apply_pe_patches( const WCHAR *module_name, const WCHAR *module_path,
                                 void *module_base, SIZE_T image_size, BOOL is_builtin )
{
    unsigned int i;

    for (i = 0; patch_sets[i].identifier; ++i)
    {
        const struct wineforge_patch_set *set = &patch_sets[i];

        if (!wcsicmp( module_name, set->module_name ))
            apply_patch_set( set, module_path, module_base, image_size, is_builtin );
    }
}

#else

/* Native ARM and ARM64 have no verified WineForge PE patches yet.
 * Translated x86 processes use the i386/x86_64 implementation above. */
void wineforge_apply_pe_patches( const WCHAR *module_name, const WCHAR *module_path,
                                 void *module_base, SIZE_T image_size, BOOL is_builtin )
{
    (void)module_name;
    (void)module_path;
    (void)module_base;
    (void)image_size;
    (void)is_builtin;
}

#endif
