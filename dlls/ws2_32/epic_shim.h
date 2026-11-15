/*
 * Epic Online Services MainService compatibility
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

#ifndef __WINE_WS2_32_EPIC_SHIM_H
#define __WINE_WS2_32_EPIC_SHIM_H

#include "winsock2.h"

/* prepare_send allocates modified_buf on the process heap when it replaces a request.
 * The caller frees it after sending and reports original_len on success. */
BOOL epic_shim_prepare_send( SOCKET s, WSABUF *buffers, DWORD buffer_count,
                             WSABUF *modified, char **modified_buf, DWORD *original_len );
void epic_shim_sent( SOCKET s, const WSABUF *buffers, DWORD buffer_count, DWORD byte_count );
void epic_shim_received( SOCKET s, WSABUF *buffers, DWORD buffer_count, ULONG_PTR *byte_count );

#endif
