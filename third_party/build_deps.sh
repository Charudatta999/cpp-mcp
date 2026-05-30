#!/usr/bin/env bash
#
# build_deps.sh — Download and statically compile OpenSSL + libcurl
#                  for cpp-mcp on Linux (x86_64).
#
# Usage:
#   ./third_party/build_deps.sh           # build both
#   ./third_party/build_deps.sh openssl   # build only openssl
#   ./third_party/build_deps.sh curl      # build only curl (requires openssl)
#
# Output:
#   third_party/openssl/include/   — OpenSSL headers
#   third_party/openssl/lib/       — libssl.a, libcrypto.a
#   third_party/curl/include/      — curl headers
#   third_party/curl/lib/          — libcurl.a
#
# Build artifacts (sources, object files) are placed in third_party/_build/
# and can be deleted after a successful build.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/_build"
NPROC="${NPROC_OVERRIDE:-$(nproc 2>/dev/null || echo 4)}"

# ── Versions ────────────────────────────────────────────────────────────────
OPENSSL_VERSION="3.3.2"
OPENSSL_SHA256="2e8a40b01979afe8be0bbfb3de5dc1c6709fedb46d6c89c10da114ab5fc3d281"
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz"

CURL_VERSION="8.11.1"
CURL_SHA256="a889ac9dbba3644271bd9d1302b5c22a088893719b72be3487bc3d401e5c4e80"
CURL_URL="https://curl.se/download/curl-${CURL_VERSION}.tar.gz"

# ── Helpers ─────────────────────────────────────────────────────────────────
info()  { printf "\033[1;34m==> %s\033[0m\n" "$*"; }
error() { printf "\033[1;31mERROR: %s\033[0m\n" "$*" >&2; exit 1; }

verify_sha256() {
    local file="$1" expected="$2"
    local actual
    actual="$(sha256sum "$file" | awk '{print $1}')"
    if [ "$actual" != "$expected" ]; then
        error "SHA256 mismatch for $file: expected $expected, got $actual"
    fi
}

download() {
    local url="$1" dest="$2"
    if [ -f "$dest" ]; then
        info "Already downloaded: $(basename "$dest")"
        return
    fi
    info "Downloading $(basename "$dest")..."
    curl -fSL --retry 3 "$url" -o "$dest"
}

# ── Build OpenSSL ───────────────────────────────────────────────────────────
build_openssl() {
    local prefix="${SCRIPT_DIR}/openssl"
    local tarball="${BUILD_DIR}/openssl-${OPENSSL_VERSION}.tar.gz"
    local srcdir="${BUILD_DIR}/openssl-${OPENSSL_VERSION}"

    if [ -f "${prefix}/lib/libcrypto.a" ] && [ -f "${prefix}/lib/libssl.a" ]; then
        info "OpenSSL already built at ${prefix}"
        return
    fi

    mkdir -p "$BUILD_DIR"
    download "$OPENSSL_URL" "$tarball"
    verify_sha256 "$tarball" "$OPENSSL_SHA256"

    info "Extracting OpenSSL ${OPENSSL_VERSION}..."
    rm -rf "$srcdir"
    tar xzf "$tarball" -C "$BUILD_DIR"

    info "Configuring OpenSSL (static, no-shared)..."
    cd "$srcdir"
    ./Configure linux-x86_64 \
        --prefix="$prefix" \
        --libdir=lib \
        no-shared \
        no-tests \
        no-docs \
        no-apps \
        no-engine \
        no-dso \
        -fPIC

    info "Building OpenSSL (-j${NPROC})..."
    make -j"$NPROC"

    info "Installing OpenSSL headers + static libs..."
    make install_sw

    # Clean up — keep only what we need
    rm -rf "${prefix}/lib/pkgconfig"
    rm -f  "${prefix}/lib/"*.so* 2>/dev/null || true

    info "OpenSSL ${OPENSSL_VERSION} installed to ${prefix}"
}

# ── Build curl ──────────────────────────────────────────────────────────────
build_curl() {
    local prefix="${SCRIPT_DIR}/curl"
    local ssl_prefix="${SCRIPT_DIR}/openssl"
    local tarball="${BUILD_DIR}/curl-${CURL_VERSION}.tar.gz"
    local srcdir="${BUILD_DIR}/curl-${CURL_VERSION}"

    if [ -f "${prefix}/lib/libcurl.a" ]; then
        info "curl already built at ${prefix}"
        return
    fi

    if [ ! -f "${ssl_prefix}/lib/libcrypto.a" ]; then
        error "OpenSSL not found at ${ssl_prefix}. Build OpenSSL first."
    fi

    mkdir -p "$BUILD_DIR"
    download "$CURL_URL" "$tarball"
    verify_sha256 "$tarball" "$CURL_SHA256"

    info "Extracting curl ${CURL_VERSION}..."
    rm -rf "$srcdir"
    tar xzf "$tarball" -C "$BUILD_DIR"

    info "Configuring curl (static, with-openssl)..."
    cd "$srcdir"
    ./configure \
        --prefix="$prefix" \
        --with-openssl="${ssl_prefix}" \
        --enable-static \
        --disable-shared \
        --disable-ldap \
        --disable-ldaps \
        --disable-rtsp \
        --disable-dict \
        --disable-telnet \
        --disable-tftp \
        --disable-pop3 \
        --disable-imap \
        --disable-smb \
        --disable-smtp \
        --disable-gopher \
        --disable-mqtt \
        --disable-manual \
        --disable-docs \
        --disable-ntlm \
        --disable-unix-sockets \
        --without-brotli \
        --without-zstd \
        --without-libidn2 \
        --without-librtmp \
        --without-libpsl \
        --without-nghttp2 \
        --with-pic \
        CFLAGS="-fPIC" \
        LDFLAGS="-L${ssl_prefix}/lib" \
        CPPFLAGS="-I${ssl_prefix}/include"

    info "Building curl (-j${NPROC})..."
    make -j"$NPROC"

    info "Installing curl headers + static lib..."
    make install

    # Clean up — keep only what we need
    rm -rf "${prefix}/lib/pkgconfig"
    rm -rf "${prefix}/share"
    rm -rf "${prefix}/bin"
    rm -f  "${prefix}/lib/"*.so* 2>/dev/null || true
    rm -f  "${prefix}/lib/"*.la  2>/dev/null || true

    info "curl ${CURL_VERSION} installed to ${prefix}"
}

# ── Main ────────────────────────────────────────────────────────────────────
main() {
    local target="${1:-all}"

    case "$target" in
        openssl) build_openssl ;;
        curl)    build_curl ;;
        all)
            build_openssl
            build_curl
            ;;
        clean)
            info "Removing build directory..."
            rm -rf "$BUILD_DIR"
            ;;
        *)
            error "Unknown target: $target (use: all, openssl, curl, clean)"
            ;;
    esac

    info "Done. You can remove sources with: $0 clean"
}

main "$@"
