from __future__ import annotations

DEFAULT_SERVER_LIMITS = {
    "maxPageSize": 200,
    "maxHttpThreads": 4,
    "maxHttpQueuedRequests": 32,
    "maxBackendQueueDepth": 64,
    "maxScanWorkers": 4,
    "maxConcurrentDownloads": 2,
    "maxRequestBodyBytes": 64 * 1024,
    "httpReadTimeoutMs": 15 * 1000,
    "httpWriteTimeoutMs": 30 * 1000,
    "maxCoverCacheMiB": 128,
    "maxSteadyStateMemoryMiB": 1024,
}

SERVER_LIMIT_ENVIRONMENT = (
    ("maxPageSize", "INPX_WEB_READER_MAX_PAGE_SIZE"),
    ("maxHttpThreads", "INPX_WEB_READER_MAX_HTTP_THREADS"),
    ("maxHttpQueuedRequests", "INPX_WEB_READER_MAX_HTTP_QUEUED_REQUESTS"),
    ("maxBackendQueueDepth", "INPX_WEB_READER_MAX_BACKEND_QUEUE_DEPTH"),
    ("maxScanWorkers", "INPX_WEB_READER_MAX_SCAN_WORKERS"),
    ("maxConcurrentDownloads", "INPX_WEB_READER_MAX_CONCURRENT_DOWNLOADS"),
    ("maxRequestBodyBytes", "INPX_WEB_READER_MAX_REQUEST_BODY_BYTES"),
    ("httpReadTimeoutMs", "INPX_WEB_READER_HTTP_READ_TIMEOUT_MS"),
    ("httpWriteTimeoutMs", "INPX_WEB_READER_HTTP_WRITE_TIMEOUT_MS"),
    ("maxCoverCacheMiB", "INPX_WEB_READER_MAX_COVER_CACHE_MIB"),
    ("maxSteadyStateMemoryMiB", "INPX_WEB_READER_MAX_STEADY_STATE_MEMORY_MIB"),
)


def server_limit_env_lines(limit_values: dict[str, int]) -> list[str]:
    missing_keys = [key for key, _env_name in SERVER_LIMIT_ENVIRONMENT if key not in limit_values]
    if missing_keys:
        raise RuntimeError(f"Missing server limit value(s): {', '.join(missing_keys)}")

    return [
        f"{env_name}={limit_values[key]}"
        for key, env_name in SERVER_LIMIT_ENVIRONMENT
    ]
