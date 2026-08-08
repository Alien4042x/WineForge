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

#ifndef __WINE_KERNELBASE_LAUNCHERS_H
#define __WINE_KERNELBASE_LAUNCHERS_H

const WCHAR *get_launcher_process_args( const WCHAR *app_name, const WCHAR *cmd_line );

#endif
