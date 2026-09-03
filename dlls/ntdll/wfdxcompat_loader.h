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

#ifndef __WINE_WFDXCOMPAT_LOADER_H
#define __WINE_WFDXCOMPAT_LOADER_H

#ifdef _WIN64
NTSTATUS wfdxcompat_get_runtime_alias( const WCHAR *name, UNICODE_STRING *nt_name );
#endif

/* WineForge-Internal: wfdxcompat/launcher-process-interposition-v1.
 * These callbacks run under the Wine loader lock. */
void wfdx_launcher_activate( HMODULE d3d11 );
void *wfdx_launcher_get_import( HMODULE module, const char *name );
void *wfdx_launcher_get_proc( HMODULE module, const ANSI_STRING *name );

#endif /* __WINE_WFDXCOMPAT_LOADER_H */
