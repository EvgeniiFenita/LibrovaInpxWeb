#!/bin/sh
set -eu
umask 077

log()
{
    printf '[inpx-web-reader-entrypoint] %s\n' "$*" >&2
}

fail()
{
    log "$*"
    exit 1
}

is_absolute_path()
{
    case "$1" in
        /*) return 0 ;;
        *) return 1 ;;
    esac
}

is_unsigned_integer()
{
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        *) return 0 ;;
    esac
}

is_ipv4_loopback_host()
{
    old_ifs="$IFS"
    IFS=.
    set -- $1
    IFS="$old_ifs"

    [ "$#" -eq 4 ] || return 1
    [ "$1" -eq 127 ] 2>/dev/null || return 1
    for octet in "$1" "$2" "$3" "$4"; do
        is_unsigned_integer "$octet" || return 1
        [ "$octet" -le 255 ] || return 1
    done
}

is_loopback_host()
{
    case "$1" in
        localhost|::1|'[::1]') return 0 ;;
        *) is_ipv4_loopback_host "$1" ;;
    esac
}

read_auth_token()
{
    if [ -n "${INPX_WEB_READER_AUTH_TOKEN_FILE:-}" ] && [ -n "${INPX_WEB_READER_AUTH_TOKEN:-}" ]; then
        fail "Set either INPX_WEB_READER_AUTH_TOKEN_FILE or INPX_WEB_READER_AUTH_TOKEN, not both."
    fi

    if [ -n "${INPX_WEB_READER_AUTH_TOKEN_FILE:-}" ]; then
        is_absolute_path "$INPX_WEB_READER_AUTH_TOKEN_FILE" || fail "INPX_WEB_READER_AUTH_TOKEN_FILE must be absolute."
        [ -f "$INPX_WEB_READER_AUTH_TOKEN_FILE" ] || fail "INPX_WEB_READER_AUTH_TOKEN_FILE does not exist: $INPX_WEB_READER_AUTH_TOKEN_FILE"
        sed -n '1{s/\r$//;p;q;}' "$INPX_WEB_READER_AUTH_TOKEN_FILE"
        return
    fi

    printf '%s\n' "${INPX_WEB_READER_AUTH_TOKEN:-}"
}

json_escape()
{
    printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}

contains_archive_zip()
{
    [ -d "$1" ] || return 1
    [ -n "$(find "$1" -maxdepth 1 -type f -iname '*.zip' -print -quit)" ]
}

try_find_single_inpx()
{
    if [ -n "${INPX_WEB_READER_INPX_PATH:-}" ]; then
        printf '%s\n' "$INPX_WEB_READER_INPX_PATH"
        return
    fi

    source_root="${INPX_WEB_READER_SOURCE_ROOT:-/source}"
    [ -d "$source_root" ] || return 1

    matches="$(find "$source_root" -maxdepth "${INPX_WEB_READER_INPX_SCAN_DEPTH:-3}" -type f -iname '*.inpx' | sort)"
    count="$(printf '%s\n' "$matches" | sed '/^$/d' | wc -l | tr -d ' ')"

    if [ "$count" -eq 0 ]; then
        return 1
    fi

    if [ "$count" -gt 1 ]; then
        log "Multiple INPX files were found under $source_root; INPX source config will not be written."
        return 1
    fi

    printf '%s\n' "$matches" | sed -n '1p'
}

try_find_archive_root()
{
    inpx_path="$1"

    if [ -n "${INPX_WEB_READER_ARCHIVE_ROOT:-}" ]; then
        printf '%s\n' "$INPX_WEB_READER_ARCHIVE_ROOT"
        return
    fi

    source_root="${INPX_WEB_READER_SOURCE_ROOT:-/source}"
    inpx_dir="$(dirname "$inpx_path")"

    for candidate in \
        "$inpx_dir/lib.rus.ec" \
        "$source_root/lib.rus.ec" \
        "$inpx_dir" \
        "$source_root"
    do
        if contains_archive_zip "$candidate"; then
            printf '%s\n' "$candidate"
            return
        fi
    done

    for child in "$inpx_dir"/* "$source_root"/*; do
        if contains_archive_zip "$child"; then
            printf '%s\n' "$child"
            return
        fi
    done

    return 1
}

write_config()
{
    config_path="$1"
    cache_root="$2"
    runtime_root="$3"
    inpx_path="$4"
    archive_root="$5"
    static_assets_root="$6"
    server_host="$7"
    server_port="$8"
    auth_token="$9"

    max_page_size="${INPX_WEB_READER_MAX_PAGE_SIZE:-200}"
    max_http_threads="${INPX_WEB_READER_MAX_HTTP_THREADS:-4}"
    max_http_queued_requests="${INPX_WEB_READER_MAX_HTTP_QUEUED_REQUESTS:-32}"
    max_backend_queue_depth="${INPX_WEB_READER_MAX_BACKEND_QUEUE_DEPTH:-64}"
    max_scan_workers="${INPX_WEB_READER_MAX_SCAN_WORKERS:-4}"
    max_concurrent_downloads="${INPX_WEB_READER_MAX_CONCURRENT_DOWNLOADS:-2}"
    max_request_body_bytes="${INPX_WEB_READER_MAX_REQUEST_BODY_BYTES:-65536}"
    http_read_timeout_ms="${INPX_WEB_READER_HTTP_READ_TIMEOUT_MS:-15000}"
    http_write_timeout_ms="${INPX_WEB_READER_HTTP_WRITE_TIMEOUT_MS:-30000}"
    max_cover_cache_mib="${INPX_WEB_READER_MAX_COVER_CACHE_MIB:-128}"
    max_steady_state_memory_mib="${INPX_WEB_READER_MAX_STEADY_STATE_MEMORY_MIB:-1024}"
    log_level="${INPX_WEB_READER_LOG_LEVEL:-info}"
    log_max_file_size_mib="${INPX_WEB_READER_LOG_MAX_FILE_SIZE_MIB:-20}"
    log_max_rotated_files="${INPX_WEB_READER_LOG_MAX_ROTATED_FILES:-4}"

    mkdir -p "$(dirname "$config_path")"

    {
        printf '{\n'
        printf '  "cacheRoot": "%s",\n' "$(json_escape "$cache_root")"
        printf '  "runtimeWorkspaceRoot": "%s"' "$(json_escape "$runtime_root")"
        if [ -n "$inpx_path" ] && [ -n "$archive_root" ]; then
            printf ',\n'
            printf '  "inpxSource": {\n'
            printf '    "inpxPath": "%s",\n' "$(json_escape "$inpx_path")"
            printf '    "archiveRoot": "%s"\n' "$(json_escape "$archive_root")"
            printf '  }'
        fi
        if [ -n "${INPX_WEB_READER_CONVERTER_PATH:-}" ]; then
            printf ',\n'
            printf '  "converter": {\n'
            printf '    "path": "%s"\n' "$(json_escape "$INPX_WEB_READER_CONVERTER_PATH")"
            printf '  }'
        fi
        printf ',\n'
        printf '  "server": {\n'
        printf '    "host": "%s",\n' "$(json_escape "$server_host")"
        printf '    "port": %s,\n' "$server_port"
        printf '    "staticAssetsRoot": "%s"\n' "$(json_escape "$static_assets_root")"
        printf '  },\n'
        printf '  "security": {\n'
        printf '    "token": "%s"\n' "$(json_escape "$auth_token")"
        printf '  },\n'
        printf '  "logging": {\n'
        printf '    "level": "%s",\n' "$(json_escape "$log_level")"
        printf '    "maxFileSizeMiB": %s,\n' "$log_max_file_size_mib"
        printf '    "maxRotatedFiles": %s\n' "$log_max_rotated_files"
        printf '  },\n'
        printf '  "limits": {\n'
        printf '    "maxPageSize": %s,\n' "$max_page_size"
        printf '    "maxHttpThreads": %s,\n' "$max_http_threads"
        printf '    "maxHttpQueuedRequests": %s,\n' "$max_http_queued_requests"
        printf '    "maxBackendQueueDepth": %s,\n' "$max_backend_queue_depth"
        printf '    "maxScanWorkers": %s,\n' "$max_scan_workers"
        printf '    "maxConcurrentDownloads": %s,\n' "$max_concurrent_downloads"
        printf '    "maxRequestBodyBytes": %s,\n' "$max_request_body_bytes"
        printf '    "httpReadTimeoutMs": %s,\n' "$http_read_timeout_ms"
        printf '    "httpWriteTimeoutMs": %s,\n' "$http_write_timeout_ms"
        printf '    "maxCoverCacheMiB": %s,\n' "$max_cover_cache_mib"
        printf '    "maxSteadyStateMemoryMiB": %s\n' "$max_steady_state_memory_mib"
        printf '  }\n'
        printf '}\n'
    } > "$config_path"
    chmod 600 "$config_path"
}

config_path="${INPX_WEB_READER_CONFIG_PATH:-/tmp/inpx-web-reader/server.json}"
cache_root="${INPX_WEB_READER_CACHE_ROOT:-/data/cache}"
runtime_root="${INPX_WEB_READER_RUNTIME_ROOT:-/data/runtime}"
static_assets_root="${INPX_WEB_READER_STATIC_ASSETS_ROOT:-/opt/inpx-web-reader/web}"
server_host="${INPX_WEB_READER_SERVER_HOST:-0.0.0.0}"
server_port="${INPX_WEB_READER_SERVER_PORT:-8080}"
auth_token="$(read_auth_token)"

is_absolute_path "$cache_root" || fail "INPX_WEB_READER_CACHE_ROOT must be absolute."
is_absolute_path "$runtime_root" || fail "INPX_WEB_READER_RUNTIME_ROOT must be absolute."
is_absolute_path "$static_assets_root" || fail "INPX_WEB_READER_STATIC_ASSETS_ROOT must be absolute."
is_absolute_path "$config_path" || fail "INPX_WEB_READER_CONFIG_PATH must be absolute."
is_unsigned_integer "$server_port" || fail "INPX_WEB_READER_SERVER_PORT must be a number."
[ "$server_port" -ge 1 ] && [ "$server_port" -le 65535 ] || fail "INPX_WEB_READER_SERVER_PORT must be in range 1..65535."
[ -d "$static_assets_root" ] || fail "Static assets root does not exist: $static_assets_root"
if [ -n "${INPX_WEB_READER_CONVERTER_PATH:-}" ]; then
    is_absolute_path "$INPX_WEB_READER_CONVERTER_PATH" || fail "INPX_WEB_READER_CONVERTER_PATH must be absolute."
fi

if [ -z "$auth_token" ] && ! is_loopback_host "$server_host"; then
    fail "INPX_WEB_READER_AUTH_TOKEN_FILE or INPX_WEB_READER_AUTH_TOKEN is required when INPX_WEB_READER_SERVER_HOST is not loopback."
fi

inpx_path="${INPX_WEB_READER_INPX_PATH:-}"
archive_root="${INPX_WEB_READER_ARCHIVE_ROOT:-}"
if { [ -n "$inpx_path" ] && [ -z "$archive_root" ]; } \
    || { [ -z "$inpx_path" ] && [ -n "$archive_root" ]; }; then
    fail "INPX source configuration is partial; both explicit INPX path and archive root are required."
fi
if [ -z "$inpx_path" ] && [ -z "$archive_root" ]; then
    if detected_inpx_path="$(try_find_single_inpx)"; then
        if detected_archive_root="$(try_find_archive_root "$detected_inpx_path")"; then
            inpx_path="$detected_inpx_path"
            archive_root="$detected_archive_root"
        else
            log "An INPX file was detected without an archive root; automatic source configuration is skipped."
        fi
    fi
fi

if { [ -n "$inpx_path" ] && [ -z "$archive_root" ]; } \
    || { [ -z "$inpx_path" ] && [ -n "$archive_root" ]; }; then
    fail "INPX source configuration is partial after auto-detection; both INPX path and archive root are required."
fi
if [ -z "$inpx_path" ]; then
    log "INPX source config was not detected; inpx-web-reader will open existing metadata or fail if the cache is absent."
fi

if [ -n "$inpx_path" ]; then
    is_absolute_path "$inpx_path" || fail "INPX path must be absolute: $inpx_path"
fi
if [ -n "$archive_root" ]; then
    is_absolute_path "$archive_root" || fail "Archive root must be absolute: $archive_root"
fi

mkdir -p "$cache_root" "$runtime_root"
write_config \
    "$config_path" \
    "$cache_root" \
    "$runtime_root" \
    "$inpx_path" \
    "$archive_root" \
    "$static_assets_root" \
    "$server_host" \
    "$server_port" \
    "$auth_token"

log "Starting inpx-web-reader with config $config_path."
unset \
    INPX_WEB_READER_CACHE_ROOT \
    INPX_WEB_READER_RUNTIME_ROOT \
    INPX_WEB_READER_INPX_PATH \
    INPX_WEB_READER_ARCHIVE_ROOT \
    INPX_WEB_READER_CONVERTER_PATH \
    INPX_WEB_READER_SERVER_HOST \
    INPX_WEB_READER_SERVER_PORT \
    INPX_WEB_READER_STATIC_ASSETS_ROOT \
    INPX_WEB_READER_AUTH_TOKEN \
    INPX_WEB_READER_AUTH_TOKEN_FILE \
    INPX_WEB_READER_LOG_LEVEL \
    INPX_WEB_READER_LOG_MAX_FILE_SIZE_MIB \
    INPX_WEB_READER_LOG_MAX_ROTATED_FILES \
    INPX_WEB_READER_MAX_PAGE_SIZE \
    INPX_WEB_READER_MAX_HTTP_THREADS \
    INPX_WEB_READER_MAX_HTTP_QUEUED_REQUESTS \
    INPX_WEB_READER_MAX_BACKEND_QUEUE_DEPTH \
    INPX_WEB_READER_MAX_SCAN_WORKERS \
    INPX_WEB_READER_MAX_CONCURRENT_DOWNLOADS \
    INPX_WEB_READER_MAX_REQUEST_BODY_BYTES \
    INPX_WEB_READER_HTTP_READ_TIMEOUT_MS \
    INPX_WEB_READER_HTTP_WRITE_TIMEOUT_MS \
    INPX_WEB_READER_MAX_COVER_CACHE_MIB \
    INPX_WEB_READER_MAX_STEADY_STATE_MEMORY_MIB
exec /opt/inpx-web-reader/inpx-web-reader --config "$config_path" "$@"
