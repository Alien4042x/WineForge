/*
 * WineForge launcher compatibility policy
 *
 * Copyright (C) 2026 Radim Vesely for WineForge
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"

#include "launchers.h"

struct launcher_process_policy
{
    const WCHAR *exe_name;
    const WCHAR *path;
    const WCHAR *path_alt;
    const WCHAR *excluded_path;
    const WCHAR *excluded_path_alt;
    const WCHAR *blocked_args[3];
    const WCHAR *append_args;
};

static const struct launcher_process_policy launcher_process_policies[] =
{
    /* WineForge-Internal: launcher-compat/steam-cef-swiftshader-v1. */
    {
        L"steamwebhelper.exe",
        L"\\Steam\\bin\\cef\\",
        L"/Steam/bin/cef/",
        NULL,
        NULL,
        {L"--type=crashpad-handler", L"--disable-gpu", NULL},
        L" --no-sandbox"
        L" --in-process-gpu"
        L" --disable-gpu"
        L" --disable-d3d11"
        L" --enable-unsafe-swiftshader"
        L" --use-gl=angle"
        L" --use-angle=swiftshader",
    },
    /* WineForge-Internal: launcher-compat/rockstar-cef-in-process-gpu-v1. */
    {
        L"SocialClubHelper.exe",
        L"\\Rockstar Games\\Social Club\\",
        L"/Rockstar Games/Social Club/",
        NULL,
        NULL,
        {L"--type=", L"--in-process-gpu", NULL},
        L" --in-process-gpu",
    },
    /* WineForge-Internal: launcher-compat/ea-app-in-process-gpu-v1. */
    {
        L"EADesktop.exe",
        L"\\Program Files\\Electronic Arts\\EA Desktop\\",
        L"/Program Files/Electronic Arts/EA Desktop/",
        L"\\compatibility32\\",
        L"/compatibility32/",
        {L"--type=", L"--in-process-gpu", NULL},
        L" --in-process-gpu",
    },
    /* WineForge-Internal: launcher-compat/battlenet-in-process-gpu-v1. */
    {
        L"Battle.net.exe",
        L"\\Program Files (x86)\\Battle.net\\",
        L"/Program Files (x86)/Battle.net/",
        NULL,
        NULL,
        {L"--type=", L"--in-process-gpu", NULL},
        L" --in-process-gpu",
    },
    /* WineForge-Internal: launcher-compat/ubisoft-connect-in-process-gpu-v1. */
    {
        L"upc.exe",
        L"\\Program Files (x86)\\Ubisoft\\Ubisoft Game Launcher\\",
        L"/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/",
        NULL,
        NULL,
        {L"--type=", L"--in-process-gpu", NULL},
        L" --in-process-gpu",
    },
};

/* WineForge-Internal: launcher-compat/steam-cef-single-process-v1. */
static const WCHAR steam_cef_software_args[] =
    L" --disable-gpu --disable-gpu-compositing --no-sandbox";
static const WCHAR steam_cef_single_process_args[] =
    L" --disable-gpu --disable-gpu-compositing --single-process --no-sandbox";

static BOOL command_line_has_switch_ci( const WCHAR *command, const WCHAR *option )
{
    SIZE_T length = lstrlenW( option );

    while (command && *command)
    {
        const WCHAR *start, *end;
        unsigned int backslashes = 0;
        BOOL quoted = FALSE;

        while (*command == ' ' || *command == '\t') command++;
        start = command;
        for (; *command; command++)
        {
            if (*command == '\\')
            {
                backslashes++;
                continue;
            }
            if (*command == '"' && !(backslashes & 1)) quoted = !quoted;
            else if (!quoted && (*command == ' ' || *command == '\t')) break;
            backslashes = 0;
        }
        end = command;
        if (end - start >= 2 && *start == '"' && end[-1] == '"') start++, end--;
        if ((SIZE_T)(end - start) == length && !wcsnicmp( start, option, length )) return TRUE;
    }
    return FALSE;
}

static BOOL string_contains_ci( const WCHAR *string, const WCHAR *needle )
{
    SIZE_T needle_len;

    if (!string || !needle) return FALSE;
    needle_len = lstrlenW( needle );
    if (!needle_len) return TRUE;
    for (; *string; string++)
        if (!wcsnicmp( string, needle, needle_len )) return TRUE;
    return FALSE;
}

static BOOL process_matches( const WCHAR *app_name, const WCHAR *cmd_line,
                             const struct launcher_process_policy *policy )
{
    return ((string_contains_ci( app_name, policy->exe_name ) &&
             (string_contains_ci( app_name, policy->path ) ||
              string_contains_ci( app_name, policy->path_alt ))) ||
            (string_contains_ci( cmd_line, policy->exe_name ) &&
             (string_contains_ci( cmd_line, policy->path ) ||
              string_contains_ci( cmd_line, policy->path_alt ))));
}

const WCHAR *get_launcher_process_args( const WCHAR *app_name, const WCHAR *cmd_line )
{
    unsigned int i, j;

    for (i = 0; i < ARRAY_SIZE(launcher_process_policies); i++)
    {
        const struct launcher_process_policy *policy = &launcher_process_policies[i];

        if (!process_matches( app_name, cmd_line, policy )) continue;
        if (string_contains_ci( app_name, policy->excluded_path ) ||
            string_contains_ci( app_name, policy->excluded_path_alt ) ||
            string_contains_ci( cmd_line, policy->excluded_path ) ||
            string_contains_ci( cmd_line, policy->excluded_path_alt ))
            return NULL;
        if (i == 0 && !string_contains_ci( cmd_line, L"--type=" ))
        {
            if (command_line_has_switch_ci( cmd_line, L"--single-process" ))
            {
                if (command_line_has_switch_ci( cmd_line, L"--disable-gpu" ) &&
                    command_line_has_switch_ci( cmd_line, L"--disable-gpu-compositing" ) &&
                    command_line_has_switch_ci( cmd_line, L"--no-sandbox" )) return NULL;
                return steam_cef_software_args;
            }
            return steam_cef_single_process_args;
        }
        for (j = 0; j < ARRAY_SIZE(policy->blocked_args) && policy->blocked_args[j]; j++)
            if (string_contains_ci( cmd_line, policy->blocked_args[j] )) return NULL;
        return policy->append_args;
    }
    return NULL;
}
