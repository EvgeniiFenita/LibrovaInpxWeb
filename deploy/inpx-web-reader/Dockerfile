# syntax=docker/dockerfile:1

ARG UBUNTU_IMAGE=ubuntu:24.04@sha256:c4a8d5503dfb2a3eb8ab5f807da5bc69a85730fb49b5cfca2330194ebcc41c7b

FROM ${UBUNTU_IMAGE} AS linux-build-env

ARG TARGETARCH
ARG INPX_WEB_READER_BUILD_JOBS=1
ENV DEBIAN_FRONTEND=noninteractive
ENV INPX_WEB_READER_BUILD_JOBS=${INPX_WEB_READER_BUILD_JOBS}
ENV VCPKG_MAX_CONCURRENCY=${INPX_WEB_READER_BUILD_JOBS}
RUN test "${TARGETARCH}" = "amd64" \
    || { echo "InpxWebReader supports only Docker TARGETARCH=amd64 (received ${TARGETARCH})." >&2; exit 1; }
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        curl \
        git \
        ninja-build \
        nodejs \
        npm \
        pkg-config \
        python3 \
        unzip \
        zip \
    && rm -rf /var/lib/apt/lists/*

ARG VCPKG_REPOSITORY=https://github.com/microsoft/vcpkg.git
ARG VCPKG_REF=b5d1a94fb7f88fd835e360fd23a45a09ceedbf48
RUN git clone "${VCPKG_REPOSITORY}" /opt/vcpkg \
    && git -C /opt/vcpkg checkout --detach "${VCPKG_REF}" \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH="/opt/vcpkg:${PATH}"
ENV VCPKG_DEFAULT_BINARY_CACHE=/root/.cache/vcpkg/archives
ENV VCPKG_BINARY_SOURCES="clear;files,/root/.cache/vcpkg/archives,readwrite"

WORKDIR /vcpkg-manifest
COPY vcpkg.json .
RUN mkdir -p /root/.cache/vcpkg/archives \
    && vcpkg install --triplet x64-linux --x-install-root=/opt/inpx-web-reader-vcpkg-prime \
    && rm -rf /opt/inpx-web-reader-vcpkg-prime

FROM linux-build-env AS build

WORKDIR /src
COPY web/inpx-web-reader/package.json web/inpx-web-reader/package-lock.json web/inpx-web-reader/
RUN mkdir -p out/build/linux-release/web/inpx-web-reader/workspace \
    && cp web/inpx-web-reader/package.json web/inpx-web-reader/package-lock.json out/build/linux-release/web/inpx-web-reader/workspace/ \
    && npm ci --prefix out/build/linux-release/web/inpx-web-reader/workspace --cache /root/.npm --update-notifier=false \
    && python3 -c 'from pathlib import Path; import hashlib; workspace = Path("/src/out/build/linux-release/web/inpx-web-reader/workspace"); hasher = hashlib.sha256(); [hasher.update((workspace / name).name.encode("utf-8")) or hasher.update(b"\0") or hasher.update((workspace / name).read_bytes()) or hasher.update(b"\0") for name in ("package.json", "package-lock.json")]; (workspace.parent / ".npm-ci.sha256").write_text(hasher.hexdigest(), encoding="utf-8")'

COPY . .

RUN cmake --preset linux-release -DINPX_WEB_READER_BUILD_WEB_ASSETS=ON
RUN cmake --build --preset linux-release \
    --target InpxWebReader InpxWebReaderWebAssets \
    --parallel "${INPX_WEB_READER_BUILD_JOBS}"

RUN mkdir -p /package/web \
    && cp out/build/linux-release/apps/InpxWebReader/inpx-web-reader /package/inpx-web-reader \
    && cp -R out/build/linux-release/web/inpx-web-reader/dist/. /package/web/

FROM ${UBUNTU_IMAGE} AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /package/inpx-web-reader /opt/inpx-web-reader/inpx-web-reader
COPY --from=build /package/web /opt/inpx-web-reader/web
COPY deploy/inpx-web-reader/entrypoint.sh /usr/local/bin/inpx-web-reader-entrypoint

RUN groupadd --system --gid 10001 inpx-web-reader \
    && useradd --system --uid 10001 --gid inpx-web-reader --home-dir /nonexistent --shell /usr/sbin/nologin inpx-web-reader \
    && chmod +x /opt/inpx-web-reader/inpx-web-reader /usr/local/bin/inpx-web-reader-entrypoint \
    && mkdir -p /data /source \
    && chown -R inpx-web-reader:inpx-web-reader /opt/inpx-web-reader /data /source

ENV INPX_WEB_READER_CACHE_ROOT=/data/cache
ENV INPX_WEB_READER_RUNTIME_ROOT=/data/runtime
ENV INPX_WEB_READER_STATIC_ASSETS_ROOT=/opt/inpx-web-reader/web
ENV INPX_WEB_READER_SERVER_HOST=0.0.0.0
ENV INPX_WEB_READER_SERVER_PORT=8080

VOLUME ["/source", "/data"]
EXPOSE 8080

USER inpx-web-reader

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD curl -fsS "http://127.0.0.1:${INPX_WEB_READER_SERVER_PORT:-8080}/api/ready" >/dev/null || exit 1

ENTRYPOINT ["/usr/local/bin/inpx-web-reader-entrypoint"]
