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

#ifndef __WINE_PE_PATCHES_H
#define __WINE_PE_PATCHES_H

void wineforge_apply_pe_patches( const WCHAR *module_name, const WCHAR *module_path,
                                 void *module_base, SIZE_T image_size, BOOL is_builtin );

#endif /* __WINE_PE_PATCHES_H */
