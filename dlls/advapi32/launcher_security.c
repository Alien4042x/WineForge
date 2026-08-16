/*
 * WineForge launcher security compatibility
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
#include "winnt.h"
#include "accctrl.h"
#include "aclapi.h"

#include "advapi32_misc.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(advapi);

/* WineForge-Internal: launcher-compat/battlenet-install-acl-v1. */
static const WCHAR *const battlenet_security_paths[] =
{
    L"\\ProgramData\\Battle.net\\Agent",
    L"/ProgramData/Battle.net/Agent",
    L"\\ProgramData\\Battle.net_components",
    L"/ProgramData/Battle.net_components",
    L"\\Program Files (x86)\\Battle.net",
    L"/Program Files (x86)/Battle.net",
};

static BOOL string_contains_ci( const WCHAR *string, const WCHAR *needle )
{
    SIZE_T length;

    if (!string || !needle) return FALSE;
    length = lstrlenW( needle );
    for (; *string; string++)
        if (!wcsnicmp( string, needle, length )) return TRUE;
    return FALSE;
}

static BOOL is_battlenet_security_path( const WCHAR *name )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(battlenet_security_paths); i++)
        if (string_contains_ci( name, battlenet_security_paths[i] )) return TRUE;
    return FALSE;
}

static BOOL create_well_known_sid( WELL_KNOWN_SID_TYPE type, BYTE *buffer )
{
    DWORD size = SECURITY_MAX_SID_SIZE;

    return CreateWellKnownSid( type, NULL, buffer, &size );
}

void wineforge_adjust_launcher_file_security( const WCHAR *name, PSID *owner, PSID *group,
                                              PACL *dacl, PACL *sacl,
                                              PSECURITY_DESCRIPTOR *descriptor )
{
    static const WELL_KNOWN_SID_TYPE sid_types[] =
    {
        WinBuiltinAdministratorsSid,
        WinBuiltinUsersSid,
        WinInteractiveSid,
    };
    BYTE sid_buffers[ARRAY_SIZE(sid_types)][SECURITY_MAX_SID_SIZE];
    EXPLICIT_ACCESSW access[ARRAY_SIZE(sid_types)] = {0};
    TRUSTEEW descriptor_owner = {0};
    PSECURITY_DESCRIPTOR replacement;
    BOOL defaulted, present;
    ULONG replacement_size;
    DWORD error;
    unsigned int i;

    if (!descriptor || !*descriptor || !is_battlenet_security_path( name )) return;

    for (i = 0; i < ARRAY_SIZE(sid_types); i++)
    {
        if (!create_well_known_sid( sid_types[i], sid_buffers[i] ))
        {
            WARN( "failed to create Battle.net security SID %u\n", i );
            return;
        }
        access[i].grfAccessPermissions = FILE_ALL_ACCESS;
        access[i].grfAccessMode = SET_ACCESS;
        access[i].grfInheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
        access[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access[i].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        access[i].Trustee.ptstrName = (WCHAR *)sid_buffers[i];
    }

    descriptor_owner.TrusteeForm = TRUSTEE_IS_SID;
    descriptor_owner.TrusteeType = TRUSTEE_IS_GROUP;
    descriptor_owner.ptstrName = (WCHAR *)sid_buffers[0];

    error = BuildSecurityDescriptorW( &descriptor_owner, NULL, ARRAY_SIZE(access), access,
                                      0, NULL, *descriptor, &replacement_size, &replacement );
    if (error)
    {
        WARN( "failed to project Battle.net security descriptor, error %lu\n", error );
        return;
    }

    LocalFree( *descriptor );
    *descriptor = replacement;
    if (owner) GetSecurityDescriptorOwner( replacement, owner, &defaulted );
    if (group) GetSecurityDescriptorGroup( replacement, group, &defaulted );
    if (dacl) GetSecurityDescriptorDacl( replacement, &present, dacl, &defaulted );
    if (sacl) GetSecurityDescriptorSacl( replacement, &present, sacl, &defaulted );
}
