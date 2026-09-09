/*
 * Launcher process classification and module policy
 *
 * WineForge-Internal: launcher-compat/process-classification-v1.
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

#include <string.h>

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"
#include "unix_private.h"

#if defined(__APPLE__) && defined(__x86_64__)

/* WineForge-Internal: launcher-compat/process-classification-v1. */
static BOOL ascii_path_ends_with_ci( const char *path, const char *suffix )
{
    size_t path_len = strlen( path );
    size_t suffix_len = strlen( suffix );
    size_t i;

    if (suffix_len > path_len) return FALSE;
    path += path_len - suffix_len;
    for (i = 0; i < suffix_len; i++)
    {
        unsigned char left = path[i], right = suffix[i];

        if ((left == '/' || left == '\\') && (right == '/' || right == '\\')) continue;
        if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
        if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
        if (left != right) return FALSE;
    }
    return TRUE;
}

/* WineForge-Internal: wfdxcompat/rockstar-launcher-policy-v1. */
static const char *const rockstar_launcher_app_paths[] =
{
    "\\rockstar games\\launcher\\launcher.exe",
    "\\rockstar games\\social club\\socialclubhelper.exe",
};

/* WineForge-Internal: launcher-compat/process-classification-v1. */
struct dxmt_launcher_policy
{
    const char *path;
    const char *required_argument;
};

/* WineForge-Internal: launcher-compat/process-classification-v1. */
static const struct dxmt_launcher_policy dxmt_launcher_policies[] =
{
    { "\\program files (x86)\\battle.net\\battle.net.exe", NULL },
    { "\\program files (x86)\\ubisoft\\ubisoft game launcher\\upc.exe", "--in-process-gpu" },
};

/* WineForge-Internal: launcher-compat/process-classification-v1. */
static BOOL is_launcher_app( const char *image_path, const char *const *paths, unsigned int count )
{
    unsigned int i;

    for (i = 0; i < count; i++)
        if (ascii_path_ends_with_ci( image_path, paths[i] )) return TRUE;
    return FALSE;
}

/* WineForge-Internal: launcher-compat/process-classification-v1. */
static BOOL wargv_has_ascii_argument_ci( const char *argument )
{
    unsigned int i;

    if (!main_wargv) return FALSE;
    for (i = 1; main_wargv[i]; i++)
    {
        const WCHAR *wide = main_wargv[i];
        const char *ascii = argument;

        while (*wide && *ascii)
        {
            WCHAR left = *wide++;
            unsigned char right = *ascii++;

            if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
            if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
            if (left != right) break;
        }
        if (!*wide && !*ascii) return TRUE;
    }
    return FALSE;
}

/* WineForge-Internal: launcher-compat/process-classification-v1. */
static BOOL is_dxmt_launcher_app( const char *image_path )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(dxmt_launcher_policies); i++)
    {
        const struct dxmt_launcher_policy *policy = &dxmt_launcher_policies[i];

        if (!ascii_path_ends_with_ci( image_path, policy->path )) continue;
        if (!policy->required_argument || wargv_has_ascii_argument_ci( policy->required_argument ))
            return TRUE;
    }
    return FALSE;
}

/* WineForge-Internal: launcher-compat/process-classification-v1. */
static BOOL is_software_cef_process(void)
{
    return wargv_has_ascii_argument_ci( "--disable-gpu" ) &&
           wargv_has_ascii_argument_ci( "--disable-d3d11" ) &&
           wargv_has_ascii_argument_ci( "--use-angle=swiftshader" );
}

/* WineForge-Internal: launcher-compat/process-classification-v1. */
void get_launcher_policy( const char *image_path, struct launcher_policy *policy )
{
    policy->cef_software = is_software_cef_process();
    policy->battlenet_cef_cow =
        ascii_path_ends_with_ci( image_path, "\\program files (x86)\\battle.net\\battle.net.exe" ) &&
        wargv_has_ascii_argument_ci( "--type=renderer" );
    policy->dxmt = is_dxmt_launcher_app( image_path );
    policy->wfdx = is_launcher_app( image_path, rockstar_launcher_app_paths,
                                   ARRAY_SIZE(rockstar_launcher_app_paths) );
}

/*
 * WineForge launcher-compat: Steam CEF/native Vulkan gate.
 * WineForge-Internal: launcher-compat/steam-cef-native-vulkan-v1.
 */
static BOOL unicode_string_contains_ascii_ci( const UNICODE_STRING *str, const char *needle )
{
    unsigned int i, j, len, needle_len;

    if (!str || !str->Buffer || !needle) return FALSE;
    len = str->Length / sizeof(WCHAR);
    needle_len = strlen( needle );
    if (!needle_len || needle_len > len) return FALSE;

    for (i = 0; i <= len - needle_len; ++i)
    {
        for (j = 0; j < needle_len; ++j)
        {
            WCHAR wc = str->Buffer[i + j];
            unsigned char ch = needle[j];

            if (wc > 127) break;
            if (wc >= 'A' && wc <= 'Z') wc += 'a' - 'A';
            if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
            if (wc != ch) break;
        }
        if (j == needle_len) return TRUE;
    }
    return FALSE;
}

/* WineForge-Internal: launcher-compat/steam-cef-native-vulkan-v1. */
BOOL steam_cef_native_vulkan_loader( const UNICODE_STRING *nt_name, const char *module )
{
    if (!module || strcasecmp( module, "vulkan-1.dll" )) return FALSE;

    return unicode_string_contains_ascii_ci( nt_name, "\\steam\\bin\\cef\\" ) ||
           unicode_string_contains_ascii_ci( nt_name, "/steam/bin/cef/" );
}

#endif
