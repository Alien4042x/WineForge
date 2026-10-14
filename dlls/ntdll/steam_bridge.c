/*
 * Native Steam client export forwarding for WineForge.
 *
 * Copyright 2026 WineForge contributors
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
 *
 * Contract reference: NotProton v1.1.0 ntdll-patch/detour.c and detour32.c,
 * and Valve's Wine proton_11.0 build_module(). This implementation uses
 * Wine's typed loader interface instead of CrossOver binary offsets.
 */

#include <stdarg.h>
#include <string.h>
#include <wchar.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/debug.h"
#include "steam_bridge.h"

WINE_DEFAULT_DEBUG_CHANNEL(module);

#if defined(__x86_64__) || defined(__i386__)
struct steam_bridge_jump
{
    BYTE *source;
    void *target;
    void *protected_addr;
    SIZE_T protected_size;
    ULONG old_protect;
};

/* WineForge-Internal: native-steam/explicit-client-export-forwarding-v1. */
BOOL wineforge_prepare_steam_bridge( const WCHAR *name, HMODULE module, const WCHAR *load_path )
{
    static HMODULE bridge;
    UNICODE_STRING variable = RTL_CONSTANT_STRING( L"WINEFORGE_STEAM_BRIDGE" );
    UNICODE_STRING bridge_name = RTL_CONSTANT_STRING( L"lsteamclient.dll" );
    WCHAR enabled[2];
    UNICODE_STRING value = { 0, sizeof(enabled), enabled };
    IMAGE_NT_HEADERS *nt = RtlImageNtHeader( module );
    IMAGE_EXPORT_DIRECTORY *exports;
    IMAGE_SECTION_HEADER *sections;
    struct steam_bridge_jump *jumps;
    DWORD *names, *functions, export_size;
    WORD *ordinals;
    BYTE *base = (BYTE *)module;
    SIZE_T jump_size;
    unsigned int i, j, count = 0, protected_count = 0;
    BOOL result = FALSE;
    NTSTATUS status;

    if (!name || (wcsicmp( name, L"steamclient.dll" ) && wcsicmp( name, L"steamclient64.dll" ))) return FALSE;
    if (RtlQueryEnvironmentVariable_U( NULL, &variable, &value ) ||
        value.Length != sizeof(WCHAR) || enabled[0] != '1') return FALSE;
    if (!nt) return FALSE;
#ifdef __x86_64__
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return FALSE;
    jump_size = 12;
#else
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) return FALSE;
    jump_size = 5;
#endif
    exports = RtlImageDirectoryEntryToData( module, TRUE, IMAGE_DIRECTORY_ENTRY_EXPORT, &export_size );
    if (!exports || !exports->NumberOfNames || export_size < sizeof(*exports)) return FALSE;
    if (exports->NumberOfNames > nt->OptionalHeader.SizeOfImage / sizeof(*jumps)) return FALSE;
    if (exports->AddressOfNames > nt->OptionalHeader.SizeOfImage ||
        exports->NumberOfNames > (nt->OptionalHeader.SizeOfImage - exports->AddressOfNames) / sizeof(DWORD) ||
        exports->AddressOfNameOrdinals > nt->OptionalHeader.SizeOfImage ||
        exports->NumberOfNames > (nt->OptionalHeader.SizeOfImage - exports->AddressOfNameOrdinals) / sizeof(WORD) ||
        exports->AddressOfFunctions > nt->OptionalHeader.SizeOfImage ||
        exports->NumberOfFunctions > (nt->OptionalHeader.SizeOfImage - exports->AddressOfFunctions) / sizeof(DWORD)) return FALSE;
    if (!bridge && (status = LdrLoadDll( load_path, NULL, &bridge_name, &bridge )))
    {
        WARN( "WineForge native Steam bridge unavailable for %s: %#lx\n", debugstr_w(name), status );
        return FALSE;
    }
    if (!(jumps = RtlAllocateHeap( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                  exports->NumberOfNames * sizeof(*jumps) ))) return FALSE;
    names = (DWORD *)(base + exports->AddressOfNames);
    ordinals = (WORD *)(base + exports->AddressOfNameOrdinals);
    functions = (DWORD *)(base + exports->AddressOfFunctions);
    sections = IMAGE_FIRST_SECTION( nt );

    /* Validate the complete plan before changing any code. */
    for (i = 0; i < exports->NumberOfNames; ++i)
    {
        ANSI_STRING export_name;
        DWORD rva;
        void *target;
        BOOL executable = FALSE;

        if (names[i] >= nt->OptionalHeader.SizeOfImage || ordinals[i] >= exports->NumberOfFunctions) goto done;
        if (!memchr( base + names[i], 0, nt->OptionalHeader.SizeOfImage - names[i] )) goto done;
        RtlInitAnsiString( &export_name, (char *)base + names[i] );
        if (LdrGetProcedureAddress( bridge, &export_name, 0, &target )) continue;
        rva = functions[ordinals[i]];
        if (!rva || rva > nt->OptionalHeader.SizeOfImage ||
            jump_size > nt->OptionalHeader.SizeOfImage - rva) goto done;
        if (base + rva >= (BYTE *)exports && base + rva < (BYTE *)exports + export_size) continue;
        for (j = 0; j < nt->FileHeader.NumberOfSections; ++j)
        {
            DWORD offset;
            if (rva < sections[j].VirtualAddress) continue;
            offset = rva - sections[j].VirtualAddress;
            if ((sections[j].Characteristics & IMAGE_SCN_MEM_EXECUTE) &&
                offset <= sections[j].Misc.VirtualSize &&
                jump_size <= sections[j].Misc.VirtualSize - offset) executable = TRUE;
        }
        if (!executable) continue;
        for (j = 0; j < count; ++j)
        {
            SIZE_T distance = base + rva >= jumps[j].source ? base + rva - jumps[j].source : jumps[j].source - (base + rva);
            if (!distance && target == jumps[j].target) break; /* export alias */
            if (distance < jump_size) goto done;
        }
        if (j != count) continue;
        jumps[count].source = base + rva;
        jumps[count].target = target;
        count++;
    }
    if (!count || LdrAddRefDll( LDR_ADDREF_DLL_PIN, bridge )) goto done;
    for (i = 0; i < count; ++i)
    {
        jumps[i].protected_addr = jumps[i].source;
        jumps[i].protected_size = jump_size;
        if (NtProtectVirtualMemory( NtCurrentProcess(), &jumps[i].protected_addr,
                                    &jumps[i].protected_size, PAGE_EXECUTE_READWRITE,
                                    &jumps[i].old_protect )) goto restore;
        protected_count++;
    }
    for (i = 0; i < count; ++i)
    {
#ifdef __x86_64__
        ULONGLONG target = (ULONG_PTR)jumps[i].target;
        jumps[i].source[0] = 0x48; /* mov rax, target; jmp rax */
        jumps[i].source[1] = 0xb8;
        memcpy( jumps[i].source + 2, &target, sizeof(target) );
        jumps[i].source[10] = 0xff;
        jumps[i].source[11] = 0xe0;
#else
        ULONG displacement = (ULONG_PTR)jumps[i].target - (ULONG_PTR)jumps[i].source - 5;
        jumps[i].source[0] = 0xe9; /* jmp rel32 */
        memcpy( jumps[i].source + 1, &displacement, sizeof(displacement) );
#endif
    }
    NtFlushInstructionCache( NtCurrentProcess(), module, nt->OptionalHeader.SizeOfImage );
    result = TRUE;
    TRACE( "WineForge native Steam bridge: %s, %u exports forwarded\n", debugstr_w(name), count );

restore:
    /* Reverse order preserves the original protection for shared code pages. */
    while (protected_count)
    {
        struct steam_bridge_jump *jump = &jumps[--protected_count];
        ULONG unused;
        status = NtProtectVirtualMemory( NtCurrentProcess(), &jump->protected_addr,
                                          &jump->protected_size, jump->old_protect, &unused );
        if (status) WARN( "WineForge Steam bridge protection restore failed: %#lx\n", status );
    }
done:
    RtlFreeHeap( GetProcessHeap(), 0, jumps );
    return result;
}
#else
/* WineForge-Internal: native-steam/unsupported-machine-fallback-v1. */
BOOL wineforge_prepare_steam_bridge( const WCHAR *name, HMODULE module, const WCHAR *load_path )
{
    return FALSE;
}
#endif
