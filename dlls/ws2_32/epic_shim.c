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

#include "ws2_32_private.h"
#include "epic_shim.h"

/* WineForge launcher-compat: Epic EOSH MainService install progress shim.
 * WineForge-Internal: launcher-compat/epic-launcher-v1.
 * WineForge-Feature: launcher-compat/epic-eosh-mainservice-progress-v1.
 *
 * only Epic processes and localhost MainService ports are considered. */
#define EPIC_EOSH_MIN_PORT 35783
#define EPIC_EOSH_MAX_PORT 35791
#define EPIC_EOSH_TRACKED_SOCKETS 128
#define EPIC_EOSH_OPERATION_ID_MAX 96
#define EPIC_EOSH_OPERATION_TYPE_MAX 32
#define EPIC_EOSH_PROGRESS_JSON_MAX 8192
#define EPIC_EOSH_PROGRESS_TAIL_MAX 262144
#define EPIC_EOSH_CORRELATION_MAX 160
#define EPIC_EOSH_SERVICE_LOG_MAX (2 * 1024 * 1024)

struct epic_eosh_operation_binding
{
    char id[EPIC_EOSH_OPERATION_ID_MAX];
    char type[EPIC_EOSH_OPERATION_TYPE_MAX];
    char correlation[EPIC_EOSH_CORRELATION_MAX];
    BOOL completed, cancelled, paused, suspended, queued;
};

enum epic_eosh_request_type
{
    EPIC_EOSH_REQUEST_NONE,
    EPIC_EOSH_REQUEST_GET_OPERATIONS,
    EPIC_EOSH_REQUEST_GET_INSTALLATIONS,
    EPIC_EOSH_REQUEST_POST_INSTALL_OPERATION,
};

static SOCKET epic_eosh_tracked_sockets[EPIC_EOSH_TRACKED_SOCKETS];
static enum epic_eosh_request_type epic_eosh_socket_request_type[EPIC_EOSH_TRACKED_SOCKETS];
static char epic_eosh_active_operation_id[EPIC_EOSH_OPERATION_ID_MAX];
static char epic_eosh_active_operation_type[EPIC_EOSH_OPERATION_TYPE_MAX] = "install";
static ULONGLONG epic_eosh_active_operation_started;
static char epic_eosh_progress_json[EPIC_EOSH_PROGRESS_JSON_MAX];
static SRWLOCK epic_eosh_lock = SRWLOCK_INIT;

static BOOL epic_eosh_is_epic_process(void)
{
    static int cached;
    static const WCHAR launcherW[] = L"EpicGamesLauncher.exe";
    static const WCHAR hostW[] = L"EpicOnlineServicesHost.exe";
    static const WCHAR helperW[] = L"EpicWebHelper.exe";
    WCHAR image[MAX_PATH], *name;
    DWORD len;

    if (cached) return cached > 0;

    if (!(len = GetModuleFileNameW( 0, image, ARRAY_SIZE(image) )) || len >= ARRAY_SIZE(image))
    {
        cached = -1;
        return FALSE;
    }

    name = image;
    if ((name = wcsrchr( image, '\\' ))) name++;
    else if ((name = wcsrchr( image, '/' ))) name++;
    else name = image;

    cached = (!wcsicmp( name, launcherW ) || !wcsicmp( name, hostW ) || !wcsicmp( name, helperW )) ? 1 : -1;
    return cached > 0;
}

static unsigned short epic_eosh_ntohs( unsigned short value )
{
    return (value >> 8) | (value << 8);
}

static unsigned short epic_eosh_sockaddr_port( const struct sockaddr *addr )
{
    if (!addr) return 0;
    if (addr->sa_family == AF_INET)
        return epic_eosh_ntohs( ((const struct sockaddr_in *)addr)->sin_port );
    if (addr->sa_family == AF_INET6)
        return epic_eosh_ntohs( ((const struct sockaddr_in6 *)addr)->sin6_port );
    return 0;
}

static void epic_eosh_socket_ports( SOCKET s, unsigned short *local_port, unsigned short *peer_port )
{
    struct sockaddr_storage addr;
    int len = sizeof(addr);

    *local_port = *peer_port = 0;
    if (!getsockname( s, (struct sockaddr *)&addr, &len ))
        *local_port = epic_eosh_sockaddr_port( (struct sockaddr *)&addr );
    len = sizeof(addr);
    if (!getpeername( s, (struct sockaddr *)&addr, &len ))
        *peer_port = epic_eosh_sockaddr_port( (struct sockaddr *)&addr );
}

static BOOL epic_eosh_is_port( unsigned short port )
{
    return port >= EPIC_EOSH_MIN_PORT && port <= EPIC_EOSH_MAX_PORT;
}

static BOOL epic_eosh_is_socket_tracked( SOCKET s )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(epic_eosh_tracked_sockets); ++i)
        if (epic_eosh_tracked_sockets[i] == s) return TRUE;
    return FALSE;
}

static unsigned int epic_eosh_track_socket( SOCKET s )
{
    unsigned int i, slot = (ULONG_PTR)s % ARRAY_SIZE(epic_eosh_tracked_sockets);

    for (i = 0; i < ARRAY_SIZE(epic_eosh_tracked_sockets); ++i)
    {
        unsigned int idx = (slot + i) % ARRAY_SIZE(epic_eosh_tracked_sockets);
        if (epic_eosh_tracked_sockets[idx] == s) return idx;
    }
    for (i = 0; i < ARRAY_SIZE(epic_eosh_tracked_sockets); ++i)
    {
        unsigned int idx = (slot + i) % ARRAY_SIZE(epic_eosh_tracked_sockets);
        if (!epic_eosh_tracked_sockets[idx])
        {
            epic_eosh_tracked_sockets[idx] = s;
            epic_eosh_socket_request_type[idx] = EPIC_EOSH_REQUEST_NONE;
            return idx;
        }
    }
    epic_eosh_tracked_sockets[slot] = s;
    epic_eosh_socket_request_type[slot] = EPIC_EOSH_REQUEST_NONE;
    return slot;
}

static void epic_eosh_set_socket_request_type( SOCKET s, enum epic_eosh_request_type type )
{
    epic_eosh_socket_request_type[epic_eosh_track_socket( s )] = type;
}

static enum epic_eosh_request_type epic_eosh_get_socket_request_type( SOCKET s )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(epic_eosh_tracked_sockets); ++i)
        if (epic_eosh_tracked_sockets[i] == s) return epic_eosh_socket_request_type[i];
    return EPIC_EOSH_REQUEST_NONE;
}

static const char *epic_eosh_memmem( const char *buf, DWORD len, const char *needle, size_t needle_len )
{
    DWORD i;

    if (!needle_len || len < needle_len) return NULL;
    for (i = 0; i + needle_len <= len; ++i)
        if (!memcmp( buf + i, needle, needle_len )) return buf + i;
    return NULL;
}

static BOOL epic_eosh_request_line( const char *buf, int len, char *line, size_t line_size )
{
    int i, copy;

    if (len < 5) return FALSE;
    if (memcmp( buf, "GET ", 4 ) && memcmp( buf, "POST ", 5 ) &&
        memcmp( buf, "DELETE ", 7 ) && memcmp( buf, "PATCH ", 6 )) return FALSE;

    for (i = 0; i < len && i < 512; ++i)
        if (buf[i] == '\n' || buf[i] == '\r') break;
    copy = min( i, (int)line_size - 1 );
    memcpy( line, buf, copy );
    line[copy] = 0;
    return TRUE;
}

static int epic_eosh_headers_len( const char *buf, int len )
{
    int i;

    for (i = 0; i + 3 < len && i < 4096; ++i)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return i + 4;
    return 0;
}

static BOOL epic_eosh_interesting_request( const char *line )
{
    return strstr( line, " /api/v1/installation/" ) != NULL;
}

static BOOL epic_eosh_operations_request( const char *line )
{
    return strstr( line, " /api/v1/installation/operations" ) != NULL ||
           strstr( line, " /api/v1/installation/installations" ) != NULL;
}

static BOOL epic_eosh_operations_poll_request( const char *line )
{
    return !memcmp( line, "GET /api/v1/installation/operations ", 36 );
}

static BOOL epic_eosh_installations_poll_request( const char *line )
{
    return !memcmp( line, "GET /api/v1/installation/installations ", 39 );
}

static BOOL epic_eosh_install_operation_request( const char *line )
{
    return !memcmp( line, "POST /api/v1/installation/operations/install ", 45 );
}

static BOOL epic_eosh_match_header_name( const char *line, int len, const char *name )
{
    unsigned int i, name_len = strlen(name);

    if (len < name_len) return FALSE;
    for (i = 0; i < name_len; ++i)
    {
        char a = line[i], b = name[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return FALSE;
    }
    return TRUE;
}

BOOL epic_shim_prepare_send( SOCKET s, WSABUF *buffers, DWORD buffer_count,
                                           WSABUF *modified, char **modified_buf, DWORD *original_len )
{
    unsigned short local_port, peer_port;
    const char *buf, *remove_start = NULL, *remove_end = NULL;
    char line[640];
    int len, headers_len, pos = 0, new_len;

    *modified_buf = NULL;
    *original_len = 0;

    /* Epic MainService may return cached empty operations/installations; force a fresh response. */
    if (buffer_count != 1 || !buffers || !buffers[0].buf || !buffers[0].len) return FALSE;
    if (!epic_eosh_is_epic_process()) return FALSE;

    epic_eosh_socket_ports( s, &local_port, &peer_port );
    if (!epic_eosh_is_port( local_port ) && !epic_eosh_is_port( peer_port )) return FALSE;

    buf = buffers[0].buf;
    len = buffers[0].len;
    if (!epic_eosh_request_line( buf, len, line, sizeof(line) )) return FALSE;
    if (!epic_eosh_operations_poll_request( line ) && !epic_eosh_installations_poll_request( line )) return FALSE;
    if (!(headers_len = epic_eosh_headers_len( buf, len ))) return FALSE;

    while (pos < headers_len)
    {
        int start = pos, line_len;

        while (pos < headers_len && buf[pos] != '\r' && buf[pos] != '\n') pos++;
        line_len = pos - start;
        while (pos < headers_len && (buf[pos] == '\r' || buf[pos] == '\n')) pos++;

        if (epic_eosh_match_header_name( buf + start, line_len, "If-None-Match:" ))
        {
            remove_start = buf + start;
            remove_end = buf + pos;
            break;
        }
        if (!line_len) break;
    }

    if (!remove_start || remove_end <= remove_start) return FALSE;

    new_len = len - (remove_end - remove_start);
    if (!(*modified_buf = HeapAlloc( GetProcessHeap(), 0, new_len ))) return FALSE;

    memcpy( *modified_buf, buf, remove_start - buf );
    memcpy( *modified_buf + (remove_start - buf), remove_end, (buf + len) - remove_end );

    *modified = buffers[0];
    modified->buf = *modified_buf;
    modified->len = new_len;
    *original_len = len;

    return TRUE;
}

static BOOL epic_eosh_extract_json_string( const char *buf, DWORD len, const char *key, char *out, size_t out_size )
{
    const char *end = buf + len, *pos = buf;
    size_t key_len = strlen(key);

    while (pos + key_len < end)
    {
        if (!memcmp( pos, key, key_len ))
        {
            const char *value = pos + key_len;
            size_t copy = 0;

            while (value < end && (*value == ' ' || *value == ':' || *value == '"')) value++;
            while (value < end && *value != '"' && *value != ',' && *value != '}' && copy + 1 < out_size)
                out[copy++] = *value++;
            out[copy] = 0;
            return copy != 0;
        }
        pos++;
    }
    return FALSE;
}

static ULONGLONG epic_eosh_operation_timestamp( const char *operation_id )
{
    ULONGLONG value = 0;
    const char *p;

    if (memcmp( operation_id, "op-", 3 )) return GetTickCount64();
    for (p = operation_id + 3; *p >= '0' && *p <= '9'; ++p)
        value = value * 10 + (*p - '0');
    return value ? value : GetTickCount64();
}

static BOOL epic_eosh_log_id( const char *text, char *id, size_t size )
{
    size_t length = 0;

    while ((text[length] >= 'a' && text[length] <= 'z') ||
           (text[length] >= '0' && text[length] <= '9') || text[length] == '-') length++;
    if (length < 5 || length >= size || memcmp( text, "op-", 3 )) return FALSE;
    if (text[length] && text[length] != ' ' && text[length] != '\r' && text[length] != ':') return FALSE;
    memcpy( id, text, length );
    id[length] = 0;
    return TRUE;
}

static BOOL epic_eosh_log_correlation( const char *text, const char *id, char *out, size_t size )
{
    char marker[160];
    const char *line, *value, *end, *line_end;
    size_t length;

    snprintf( marker, sizeof(marker), "LogInstallationApiService: Log: Creating operation %s [correlationId=", id );
    if (!(line = strstr( text, marker ))) return FALSE;
    value = line + strlen(marker);
    line_end = strchr( value, '\n' );
    if (!(end = strchr( value, ']' )) || (line_end && line_end < end)) return FALSE;
    length = end - value;
    if (!length || length >= size) return FALSE;
    memcpy( out, value, length );
    out[length] = 0;
    return TRUE;
}

static BOOL epic_eosh_same_client( const char *first, const char *second )
{
    const char *last_first = strrchr( first, '-' ), *last_second = strrchr( second, '-' );
    size_t length;

    if (strncmp( first, "UE5-", 4 ) || strncmp( second, "UE5-", 4 ) || !last_first || !last_second) return FALSE;
    length = last_first - first;
    return length > 16 && (size_t)(last_second - second) == length && !memcmp( first, second, length );
}

static BOOL epic_eosh_parse_service_log( char *text, const char *requested_id,
                                        const char *requested_type, struct epic_eosh_operation_binding *binding )
{
    static const char prefix[] = "LogOperationManagementService: Log: ";
    static const char *types[] = { "install", "uninstall", "repair" };
    char own_correlation[EPIC_EOSH_CORRELATION_MAX], id[EPIC_EOSH_OPERATION_ID_MAX], marker[192];
    char *line, *end, saved;
    const char *message, *action;
    unsigned int i;
    BOOL typed = FALSE;

    memset( binding, 0, sizeof(*binding) );
    if (!strstr( text, "OperationManagementService initialized (max concurrent: 1," )) return FALSE;
    if (!epic_eosh_log_correlation( text, requested_id, own_correlation, sizeof(own_correlation) )) return FALSE;
    for (line = text; line && *line; line = end ? end + 1 : NULL)
    {
        end = strchr( line, '\n' );
        if (!end) break;
        saved = end ? *end : 0;
        if (end) *end = 0;
        if ((message = strstr( line, prefix )))
        {
            message += strlen(prefix);
            if (!strncmp( message, "Started operation ", 18 ) &&
                epic_eosh_log_id( message + 18, id, sizeof(id) ))
            {
                memset( binding, 0, sizeof(*binding) );
                lstrcpynA( binding->id, id, sizeof(binding->id) );
            }
            else if (!strncmp( message, "Operation ", 10 ) &&
                     epic_eosh_log_id( message + 10, id, sizeof(id) ) && !strcmp( id, binding->id ))
            {
                action = message + 10 + strlen(id);
                if (strstr( action, "completed with status:" ) || strstr( action, "successfully cancelled" ))
                {
                    binding->completed = TRUE;
                    binding->cancelled = strstr( action, "Cancelled" ) || strstr( action, "successfully cancelled" );
                }
                else if (strstr( action, "successfully paused" )) binding->paused = TRUE;
                else if (strstr( action, " suspended" )) binding->suspended = TRUE;
                else if (strstr( action, "re-queued" )) binding->queued = TRUE;
            }
            else if (!strncmp( message, "Resuming paused operation ", 26 ) &&
                     epic_eosh_log_id( message + 26, id, sizeof(id) ) && !strcmp( id, binding->id ))
                binding->paused = FALSE;
        }
        if (end) *end = saved;
    }
    if (!binding->id[0] || binding->queued ||
        !epic_eosh_log_correlation( text, binding->id, binding->correlation, sizeof(binding->correlation) )) return FALSE;
    if (strcmp( requested_id, binding->id ) && !epic_eosh_same_client( own_correlation, binding->correlation )) return FALSE;
    for (i = 0; i < ARRAY_SIZE(types); ++i)
    {
        snprintf( marker, sizeof(marker), "LogInstallHelperWrapperOperation_%s_%s:", types[i], binding->id );
        if (!strstr( text, marker )) continue;
        lstrcpynA( binding->type, types[i], sizeof(binding->type) );
        typed = TRUE;
        break;
    }
    if (!typed && !strcmp( requested_id, binding->id ))
    {
        lstrcpynA( binding->type, requested_type, sizeof(binding->type) );
        typed = TRUE;
    }
    return typed;
}

static BOOL epic_eosh_bind_operation_from_logs( struct epic_eosh_operation_binding *binding )
{
    static const char dir[] = "C:\\ProgramData\\Epic\\EpicOnlineServices\\MainService\\Logs\\";
    struct epic_eosh_operation_binding candidate;
    WIN32_FIND_DATAA data, best;
    char pattern[MAX_PATH], path[MAX_PATH], *text;
    HANDLE find, file;
    LARGE_INTEGER size;
    DWORD read;
    BOOL found = FALSE;

    snprintf( pattern, sizeof(pattern), "%s*_mainService_*.log", dir );
    if ((find = FindFirstFileA( pattern, &data )) == INVALID_HANDLE_VALUE) return FALSE;
    do
    {
        ULARGE_INTEGER stamp;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        stamp.LowPart = data.ftLastWriteTime.dwLowDateTime;
        stamp.HighPart = data.ftLastWriteTime.dwHighDateTime;
        if (stamp.QuadPart / 10000 < epic_eosh_active_operation_started + 11644473600000ULL) continue;
        if (found && CompareFileTime( &data.ftLastWriteTime, &best.ftLastWriteTime ) <= 0) continue;
        snprintf( path, sizeof(path), "%s%s", dir, data.cFileName );
        file = CreateFileA( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
        if (file == INVALID_HANDLE_VALUE) continue;
        text = NULL;
        if (GetFileSizeEx( file, &size ) && size.QuadPart > 0 && size.QuadPart <= EPIC_EOSH_SERVICE_LOG_MAX &&
            (text = HeapAlloc( GetProcessHeap(), 0, size.QuadPart + 1 )) &&
            ReadFile( file, text, size.QuadPart, &read, NULL ) && read == size.QuadPart)
        {
            text[read] = 0;
            if (epic_eosh_parse_service_log( text, epic_eosh_active_operation_id,
                                             epic_eosh_active_operation_type, &candidate ))
            {
                *binding = candidate;
                best = data;
                found = TRUE;
            }
        }
        if (text) HeapFree( GetProcessHeap(), 0, text );
        CloseHandle( file );
    } while (FindNextFileA( find, &data ));
    FindClose( find );
    return found;
}

static BOOL epic_eosh_extract_counter( const char *buf, DWORD len, const char *key, unsigned int *value )
{
    const char *end = buf + len, *pos;
    size_t key_len = strlen(key);
    unsigned int ret = 0;

    if (!(pos = epic_eosh_memmem( buf, len, key, key_len ))) return FALSE;
    pos += key_len;
    while (pos < end && (*pos == ' ' || *pos == ':')) pos++;
    if (pos >= end || *pos < '0' || *pos > '9') return FALSE;
    while (pos < end && *pos >= '0' && *pos <= '9')
    {
        ret = ret * 10 + (*pos - '0');
        pos++;
    }
    *value = ret;
    return TRUE;
}

static BOOL epic_eosh_copy_json_object( const char *buf, DWORD len, char *out, size_t out_size )
{
    DWORD i;
    int depth = 0;
    BOOL in_string = FALSE, escaped = FALSE;

    if (!len || buf[0] != '{' || out_size < 2) return FALSE;

    for (i = 0; i < len; ++i)
    {
        char ch = buf[i];

        if (in_string)
        {
            if (escaped) escaped = FALSE;
            else if (ch == '\\') escaped = TRUE;
            else if (ch == '"') in_string = FALSE;
            continue;
        }

        if (ch == '"') in_string = TRUE;
        else if (ch == '{') depth++;
        else if (ch == '}' && --depth == 0)
        {
            DWORD copy = i + 1;

            if (copy >= out_size) return FALSE;
            memcpy( out, buf, copy );
            out[copy] = 0;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL epic_eosh_extract_progress_body( const char *buf, DWORD len, char *out, size_t out_size )
{
    const char *pos = buf, *last = NULL, *end = buf + len;
    static const char progress[] = "\"type\":\"progress\"";
    static const char body[] = "\"body\"";

    while (pos < end)
    {
        const char *next = epic_eosh_memmem( pos, end - pos, progress, strlen(progress) );
        if (!next) break;
        last = next;
        pos = next + strlen(progress);
    }
    if (!last) return FALSE;
    if (!(last = epic_eosh_memmem( last, end - last, body, strlen(body) ))) return FALSE;
    last += strlen(body);
    while (last < end && (*last == ' ' || *last == ':')) last++;
    if (last >= end || *last != '{') return FALSE;

    return epic_eosh_copy_json_object( last, end - last, out, out_size );
}

static BOOL epic_eosh_header_correlation( const char *text, DWORD len, const char *correlation )
{
    static const char prefix[] = "--correlationid=";
    const char *value = epic_eosh_memmem( text, len, prefix, sizeof(prefix) - 1 );
    const char *end = text + len;
    size_t length = strlen(correlation);

    if (!value || !length) return FALSE;
    value += sizeof(prefix) - 1;
    if (value < end && *value == '"') value++;
    if ((size_t)(end - value) <= length || memcmp( value, correlation, length )) return FALSE;
    value += length;
    return *value == '"' || *value == ' ' || *value == '\r' || *value == '\n' || *value == ',';
}

static BOOL epic_eosh_log_matches_operation( const char *path, const char *correlation )
{
    char *header;
    HANDLE file;
    DWORD read;
    BOOL ret = FALSE;

    file = CreateFileA( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if ((header = HeapAlloc( GetProcessHeap(), 0, 65536 )))
    {
        if (ReadFile( file, header, 65536, &read, NULL ))
            ret = epic_eosh_header_correlation( header, read, correlation );
        HeapFree( GetProcessHeap(), 0, header );
    }
    CloseHandle( file );
    return ret;
}

static BOOL epic_eosh_read_progress_from_log( const char *path, char *out, size_t out_size )
{
    HANDLE file;
    DWORD file_size, offset = 0, read = 0, tail_size;
    char *tail;
    BOOL ret = FALSE;

    file = CreateFileA( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;

    file_size = GetFileSize( file, NULL );
    if (file_size < 32) goto done;
    tail_size = min( file_size, (DWORD)EPIC_EOSH_PROGRESS_TAIL_MAX );
    offset = file_size - tail_size;
    if (!(tail = HeapAlloc( GetProcessHeap(), 0, tail_size + 1 ))) goto done;
    SetFilePointer( file, offset, NULL, FILE_BEGIN );
    if (ReadFile( file, tail, tail_size, &read, NULL ) && read)
    {
        tail[read] = 0;
        ret = epic_eosh_extract_progress_body( tail, read, out, out_size );
    }
    HeapFree( GetProcessHeap(), 0, tail );

done:
    CloseHandle( file );
    return ret;
}

static BOOL epic_eosh_refresh_progress_from_logs( const char *correlation )
{
    static const char dir[] = "C:\\ProgramData\\Epic\\EpicOnlineServices\\InstallHelper\\Logs\\";
    char pattern[MAX_PATH], path[MAX_PATH], best_path[MAX_PATH];
    WIN32_FIND_DATAA data, best_data;
    HANDLE find;
    BOOL found = FALSE;

    snprintf( pattern, sizeof(pattern), "%sInstallHelper-*.log", dir );
    find = FindFirstFileA( pattern, &data );
    if (find == INVALID_HANDLE_VALUE) return FALSE;

    do
    {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        snprintf( path, sizeof(path), "%s%s", dir, data.cFileName );
        if (!epic_eosh_log_matches_operation( path, correlation )) continue;
        if (!found || CompareFileTime( &data.ftCreationTime, &best_data.ftCreationTime ) > 0)
        {
            best_data = data;
            lstrcpynA( best_path, path, sizeof(best_path) );
            found = TRUE;
        }
    } while (FindNextFileA( find, &data ));

    FindClose( find );
    if (!found) return FALSE;
    if (!epic_eosh_read_progress_from_log( best_path, epic_eosh_progress_json,
                                          sizeof(epic_eosh_progress_json) )) return FALSE;

    return TRUE;
}

static const char *epic_eosh_detect_operation_type( const char *buf, DWORD len )
{
    if (epic_eosh_memmem( buf, len, "\"type\":\"Uninstall\"", 18 ) ||
        epic_eosh_memmem( buf, len, "\"type\": \"Uninstall\"", 19 ))
        return "uninstall";
    if (epic_eosh_memmem( buf, len, "\"type\":\"Repair\"", 15 ) ||
        epic_eosh_memmem( buf, len, "\"type\": \"Repair\"", 16 ))
        return "repair";
    return "install";
}

static BOOL epic_eosh_buffer_mentions_operation_type( const char *buf, DWORD len )
{
    return epic_eosh_memmem( buf, len, "\"type\":\"Uninstall\"", 18 ) ||
           epic_eosh_memmem( buf, len, "\"type\": \"Uninstall\"", 19 ) ||
           epic_eosh_memmem( buf, len, "\"type\":\"Repair\"", 15 ) ||
           epic_eosh_memmem( buf, len, "\"type\": \"Repair\"", 16 ) ||
           epic_eosh_memmem( buf, len, "\"type\":\"InstallOrUpdate\"", 24 ) ||
           epic_eosh_memmem( buf, len, "\"type\": \"InstallOrUpdate\"", 25 );
}

static void epic_eosh_note_operation_type( const char *buf, DWORD len )
{
    const char *type = epic_eosh_detect_operation_type( buf, len );

    lstrcpynA( epic_eosh_active_operation_type, type, sizeof(epic_eosh_active_operation_type) );
}

static void epic_eosh_note_operation_id( const char *buf, DWORD len )
{
    char operation_id[EPIC_EOSH_OPERATION_ID_MAX];

    if (!epic_eosh_extract_json_string( buf, len, "\"operationId\"", operation_id, sizeof(operation_id) )) return;
    lstrcpynA( epic_eosh_active_operation_id, operation_id, sizeof(epic_eosh_active_operation_id) );
    epic_eosh_active_operation_started = epic_eosh_operation_timestamp( operation_id );
}

static BOOL epic_eosh_replace_operations_response( SOCKET s, char *buf, DWORD capacity, ULONG_PTR *byte_count )
{
    static const char headers_fmt[] =
        "HTTP/1.1 200 OK\r\n"
        "Vary: Origin\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %u\r\n"
        "Connection: keep-alive\r\n"
        "Keep-Alive: timeout=5\r\n\r\n";
    static const char running_progress[] =
        "{\"installTaskStep\":\"Bps\",\"bpsOngoingStats\":{\"state\":\"Downloading\","
        "\"downloadHealth\":\"Excellent\",\"updateProgress\":0.01},\"bpsEndStats\":{"
        "\"errorCode\":\"OK\",\"processSuccess\":true,\"finalProgress\":0}}";
    static const char completed_progress[] =
        "{\"installTaskStep\":\"Bps\",\"bpsOngoingStats\":{\"state\":\"Completed\","
        "\"downloadHealth\":\"Excellent\",\"updateProgress\":1.0},\"bpsEndStats\":{"
        "\"errorCode\":\"OK\",\"processSuccess\":true,\"finalProgress\":1.0}}";
    static const char cancelled_progress[] =
        "{\"installTaskStep\":\"Bps\",\"bpsOngoingStats\":{\"state\":\"Completed\","
        "\"updateProgress\":0},\"bpsEndStats\":{\"errorCode\":\"UC01\","
        "\"failureType\":\"UserCanceled\",\"processSuccess\":false,\"finalProgress\":0}}";
    struct epic_eosh_operation_binding binding;
    char body[12288], response[12800], row[EPIC_EOSH_PROGRESS_JSON_MAX + 512], summary_json[1024];
    const char *status, *progress_json, *summary, *queued_row = "", *running_row = "";
    const char *suspended_row = "", *completed_row = "";
    unsigned int running, completed, suspended;
    ULONGLONG started;
    int row_len, body_len, response_len;

    if (!epic_eosh_active_operation_id[0] || *byte_count > capacity) return FALSE;
    if (!epic_eosh_memmem( buf, *byte_count, "HTTP/1.1 200 OK", 15 )) return FALSE;
    if (!epic_eosh_memmem( buf, *byte_count, "\"queued\":[]", 10 ) ||
        !epic_eosh_memmem( buf, *byte_count, "\"running\":[]", 12 ) ||
        !epic_eosh_memmem( buf, *byte_count, "\"suspended\":[]", 14 ) ||
        !epic_eosh_memmem( buf, *byte_count, "\"completed\":[]", 14 )) return FALSE;
    if (!(summary = epic_eosh_memmem( buf, *byte_count, "\"globalSummary\"", 15 ))) return FALSE;
    if (!epic_eosh_extract_counter( summary, buf + *byte_count - summary, "\"running\"", &running ) ||
        !epic_eosh_extract_counter( summary, buf + *byte_count - summary, "\"completed\"", &completed )) return FALSE;
    summary += 15;
    while (summary < buf + *byte_count && (*summary == ':' || *summary == ' ')) summary++;
    if (!epic_eosh_copy_json_object( summary, buf + *byte_count - summary, summary_json, sizeof(summary_json) )) return FALSE;
    if (!epic_eosh_bind_operation_from_logs( &binding )) return FALSE;

    /* A later request may only be queued. The backend's started operation owns the helper. */
    if (binding.completed)
    {
        if (!completed) return FALSE;
        status = "Completed";
        completed_row = row;
    }
    else if (binding.suspended || (binding.paused && !running))
    {
        if (!epic_eosh_extract_counter( summary_json, strlen(summary_json), "\"suspended\"", &suspended ) || !suspended)
            return FALSE;
        status = binding.paused ? "Paused" : "Suspended";
        suspended_row = row;
    }
    else
    {
        if (running != 1) return FALSE;
        status = binding.paused ? "Paused" : "Running";
        running_row = row;
    }
    progress_json = binding.completed ? completed_progress : running_progress;
    if (epic_eosh_refresh_progress_from_logs( binding.correlation )) progress_json = epic_eosh_progress_json;
    if (binding.cancelled) progress_json = cancelled_progress;
    started = epic_eosh_operation_timestamp( binding.id );
    row_len = snprintf( row, sizeof(row),
        "{\"id\":\"%s\",\"request\":{\"type\":\"%s\",\"body\":{}},\"progress\":%s,"
        "\"status\":\"%s\",\"startedAt\":%llu,\"queuedAt\":%llu}",
        binding.id, binding.type, progress_json, status, (unsigned long long)started, (unsigned long long)started );
    if (row_len <= 0 || row_len >= sizeof(row)) return FALSE;
    body_len = snprintf( body, sizeof(body),
        "{\"queued\":[%s],\"running\":[%s],\"suspended\":[%s],\"completed\":[%s],\"globalSummary\":%s}",
        queued_row, running_row, suspended_row, completed_row, summary_json );
    if (body_len <= 0 || body_len >= sizeof(body)) return FALSE;
    response_len = snprintf( response, sizeof(response), headers_fmt, body_len );
    if (response_len <= 0 || response_len + body_len >= sizeof(response) || response_len + body_len > capacity) return FALSE;
    memcpy( response + response_len, body, body_len );
    response_len += body_len;
    memcpy( buf, response, response_len );
    *byte_count = response_len;
    return TRUE;
}

static BOOL epic_eosh_append_egi_installation( char *body, DWORD body_size, DWORD *body_len,
                                               const char *path, DWORD *count, char *revision,
                                               size_t revision_size )
{
    HANDLE file;
    DWORD size, file_size, read, start = *body_len;
    char egi_revision[64];
    char *file_buf, *v4, *end;
    BOOL ret = FALSE;

    /* Epic InstallHelper .egi files are authoritative when MainService loses installed-state cache. */
    file = CreateFileA( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;

    file_size = GetFileSize( file, NULL );
    if (file_size < 16 || file_size > 32768) goto done;
    if (!(file_buf = HeapAlloc( GetProcessHeap(), 0, file_size + 1 ))) goto done;

    if (!ReadFile( file, file_buf, file_size, &read, NULL ) || read != file_size)
    {
        HeapFree( GetProcessHeap(), 0, file_buf );
        goto done;
    }
    file_buf[file_size] = 0;

    v4 = strstr( file_buf, "{\"v4\":" );
    end = file_buf + file_size;
    if (!v4 || end <= v4 + 7 || end[-1] != '}')
    {
        HeapFree( GetProcessHeap(), 0, file_buf );
        goto done;
    }

    if (*count)
    {
        if (*body_len + 1 >= body_size)
        {
            HeapFree( GetProcessHeap(), 0, file_buf );
            goto done;
        }
        body[(*body_len)++] = ',';
    }

    size = end - (v4 + 6) - 1;
    if (*body_len + size >= body_size)
    {
        *body_len = start;
        HeapFree( GetProcessHeap(), 0, file_buf );
        goto done;
    }

    memcpy( body + *body_len, v4 + 6, size );
    *body_len += size;
    (*count)++;
    if (revision_size &&
        epic_eosh_extract_json_string( file_buf, file_size, "\"revision\"", egi_revision, sizeof(egi_revision) ))
    {
        if (!revision[0] || strcmp( egi_revision, revision ) > 0)
            lstrcpynA( revision, egi_revision, revision_size );
    }
    ret = TRUE;
    HeapFree( GetProcessHeap(), 0, file_buf );

done:
    CloseHandle( file );
    return ret;
}

static BOOL epic_eosh_build_installations_body( char *body, DWORD body_size, DWORD *body_len,
                                                DWORD *count, char *revision, size_t revision_size )
{
    static const char dir[] = "C:\\ProgramData\\Epic\\EpicOnlineServices\\InstallHelper\\InstalledItems\\";
    char pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA data;
    HANDLE find;

    *body_len = snprintf( body, body_size, "{\"installations\":[" );
    if (!*body_len || *body_len >= body_size) return FALSE;
    *count = 0;
    if (revision_size) revision[0] = 0;

    snprintf( pattern, sizeof(pattern), "%s*.egi", dir );
    find = FindFirstFileA( pattern, &data );
    if (find == INVALID_HANDLE_VALUE) return FALSE;

    do
    {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        snprintf( path, sizeof(path), "%s%s", dir, data.cFileName );
        epic_eosh_append_egi_installation( body, body_size, body_len, path, count, revision, revision_size );
    } while (FindNextFileA( find, &data ));

    FindClose( find );
    if (*body_len + 64 >= body_size) return FALSE;
    *body_len += snprintf( body + *body_len, body_size - *body_len,
                           "],\"globalSummary\":{\"total\":%lu}}", *count );
    return *count != 0 && revision[0] && *body_len < body_size;
}

static BOOL epic_eosh_replace_installations_response( SOCKET s, char *buf, DWORD capacity, ULONG_PTR *byte_count )
{
    static const char headers_fmt[] =
        "HTTP/1.1 200 OK\r\n"
        "Vary: Origin\r\n"
        "etag: %s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %lu\r\n"
        "Connection: keep-alive\r\n"
        "Keep-Alive: timeout=5\r\n"
        "\r\n";
    char *body, *response;
    char revision[64];
    DWORD body_len, count;
    int header_len;
    BOOL ret = FALSE;

    if (!epic_eosh_memmem( buf, *byte_count, "HTTP/1.1 200 OK", 15 )) return FALSE;
    if (!epic_eosh_memmem( buf, *byte_count, "\"installations\":[", 17 )) return FALSE;
    if (!epic_eosh_memmem( buf, *byte_count, "\"globalSummary\"", 15 )) return FALSE;

    if (!(body = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, 49152 ))) return FALSE;
    if (!(response = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, 65536 )))
    {
        HeapFree( GetProcessHeap(), 0, body );
        return FALSE;
    }

    if (!epic_eosh_build_installations_body( body, 49152, &body_len, &count, revision, sizeof(revision) )) goto done;
    header_len = snprintf( response, 65536, headers_fmt, revision, body_len );
    if (header_len <= 0 || header_len + body_len > capacity || header_len + body_len >= 65536) goto done;
    memcpy( response + header_len, body, body_len );
    memcpy( buf, response, header_len + body_len );
    *byte_count = header_len + body_len;

    ret = TRUE;

done:
    HeapFree( GetProcessHeap(), 0, response );
    HeapFree( GetProcessHeap(), 0, body );
    return ret;
}

static void epic_eosh_maybe_shim_http_response( SOCKET s, WSABUF *buffers, DWORD buffer_count, ULONG_PTR *byte_count )
{
    enum epic_eosh_request_type type;
    DWORD len;

    if (buffer_count != 1 || !buffers || !buffers[0].buf || !byte_count || !*byte_count ||
        *byte_count > buffers[0].len) return;
    if (!epic_eosh_is_epic_process()) return;
    AcquireSRWLockExclusive( &epic_eosh_lock );
    if (!epic_eosh_is_socket_tracked( s )) goto done;

    len = min( buffers[0].len, (DWORD)*byte_count );
    type = epic_eosh_get_socket_request_type( s );
    if (type == EPIC_EOSH_REQUEST_POST_INSTALL_OPERATION)
    {
        epic_eosh_note_operation_id( buffers[0].buf, len );
        epic_eosh_set_socket_request_type( s, EPIC_EOSH_REQUEST_NONE );
        goto done;
    }
    if (type == EPIC_EOSH_REQUEST_GET_INSTALLATIONS)
    {
        epic_eosh_replace_installations_response( s, buffers[0].buf, buffers[0].len, byte_count );
        epic_eosh_set_socket_request_type( s, EPIC_EOSH_REQUEST_NONE );
        goto done;
    }
    if (type == EPIC_EOSH_REQUEST_GET_OPERATIONS)
    {
        epic_eosh_replace_operations_response( s, buffers[0].buf, buffers[0].len, byte_count );
        epic_eosh_set_socket_request_type( s, EPIC_EOSH_REQUEST_NONE );
    }
done:
    ReleaseSRWLockExclusive( &epic_eosh_lock );
}

static void epic_eosh_maybe_track_http( const char *direction, SOCKET s, const WSABUF *buffers,
                                        DWORD buffer_count, DWORD byte_count )
{
    unsigned short local_port, peer_port;
    char line[640];
    const char *buf;
    DWORD len;

    if (!buffer_count || !buffers || !byte_count) return;
    if (!epic_eosh_is_epic_process()) return;
    epic_eosh_socket_ports( s, &local_port, &peer_port );
    if (!epic_eosh_is_port( local_port ) && !epic_eosh_is_port( peer_port )) return;

    buf = buffers[0].buf;
    len = min( buffers[0].len, byte_count );
    if (!buf || !len) return;
    AcquireSRWLockExclusive( &epic_eosh_lock );

    if (epic_eosh_request_line( buf, len, line, sizeof(line) ))
    {
        if (!epic_eosh_interesting_request( line )) goto done;
        if (epic_eosh_operations_poll_request( line ))
            epic_eosh_set_socket_request_type( s, EPIC_EOSH_REQUEST_GET_OPERATIONS );
        else if (epic_eosh_installations_poll_request( line ))
            epic_eosh_set_socket_request_type( s, EPIC_EOSH_REQUEST_GET_INSTALLATIONS );
        else if (epic_eosh_install_operation_request( line ))
        {
            epic_eosh_set_socket_request_type( s, EPIC_EOSH_REQUEST_POST_INSTALL_OPERATION );
            if (epic_eosh_buffer_mentions_operation_type( buf, len ))
                epic_eosh_note_operation_type( buf, len );
        }
        else if (epic_eosh_operations_request( line ))
            epic_eosh_set_socket_request_type( s, EPIC_EOSH_REQUEST_NONE );
        goto done;
    }

    if (!epic_eosh_is_socket_tracked( s )) goto done;
    if (!strcmp( direction, "send" ))
    {
        if (epic_eosh_get_socket_request_type( s ) == EPIC_EOSH_REQUEST_POST_INSTALL_OPERATION &&
            epic_eosh_buffer_mentions_operation_type( buf, len ))
            epic_eosh_note_operation_type( buf, len );
        goto done;
    }
done:
    ReleaseSRWLockExclusive( &epic_eosh_lock );
}

void epic_shim_sent( SOCKET s, const WSABUF *buffers, DWORD buffer_count, DWORD byte_count )
{
    epic_eosh_maybe_track_http( "send", s, buffers, buffer_count, byte_count );
}

void epic_shim_received( SOCKET s, WSABUF *buffers, DWORD buffer_count, ULONG_PTR *byte_count )
{
    epic_eosh_maybe_shim_http_response( s, buffers, buffer_count, byte_count );
    epic_eosh_maybe_track_http( "recv", s, buffers, buffer_count, *byte_count );
}
