#!/usr/bin/env bash
# Prepares per-project dependencies without root:
#   .deps/root  - extracted jsoncpp/yaml-cpp/uuid/libsodium headers and libs
#   .deps/ssl   - Drogon 1.9.14 + trantor built with OpenSSL
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS="$ROOT/.deps"
PKGS="$DEPS/pkgs"
PREFIX="$DEPS/root"
DROGON_PREFIX="$DEPS/ssl"
SRC="$DEPS/src"
JOBS="${JOBS:-$(nproc)}"

mkdir -p "$PKGS" "$PREFIX" "$SRC"

packages=(
    libjsoncpp-dev libjsoncpp26 libyaml-cpp-dev libyaml-cpp0.8
    uuid-dev libsodium-dev libsqlite3-dev
)
for pkg in "${packages[@]}"; do
    if ! compgen -G "$PKGS/${pkg}*.deb" >/dev/null; then
        echo "== downloading $pkg"
        (cd "$PKGS" && apt-get download "$pkg") || {
            echo "warning: could not download $pkg (system package may already be present)" >&2
        }
    fi
done
for deb in "$PKGS"/*.deb; do
    [[ -e "$deb" ]] || continue
    dpkg-deb -x "$deb" "$PREFIX"
done

# libuuid-dev may ship only libuuid.a in the extracted prefix; use the system shared object.
if [[ ! -e "$PREFIX/usr/lib/x86_64-linux-gnu/libuuid.so.1.3.0" ]]; then
    ln -sf /usr/lib/x86_64-linux-gnu/libuuid.so.1.3.0 \
        "$PREFIX/usr/lib/x86_64-linux-gnu/libuuid.so.1.3.0" 2>/dev/null || true
fi

if [[ -f "$DROGON_PREFIX/include/drogon/drogon.h" && -f "$DROGON_PREFIX/lib/libdrogon.so" ]]; then
    echo "SSL-enabled Drogon already present in $DROGON_PREFIX"
else
    if [[ ! -d "$SRC/drogon/.git" ]]; then
        echo "== cloning Drogon v1.9.14"
        git clone --depth 1 --branch v1.9.14 --recursive \
            https://github.com/drogonframework/drogon.git "$SRC/drogon"
    fi
    echo "== building Drogon with OpenSSL"
    cmake -S "$SRC/drogon" -B "$SRC/drogon/build-ssl" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$DROGON_PREFIX" \
        -DCMAKE_PREFIX_PATH="$PREFIX/usr;$DEPS/root/usr/lib/x86_64-linux-gnu/cmake;$DEPS/root/usr/lib/x86_64-linux-gnu/cmake/jsoncpp;$DEPS/root/usr/lib/x86_64-linux-gnu/cmake/yaml-cpp" \
        -DBUILD_SHARED_LIBS=ON \
        -DUSE_SUBMODULE=ON \
        -DBUILD_CTL=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF \
        -DBUILD_ORM=OFF -DBUILD_BROTLI=OFF -DBUILD_YAML_CONFIG=OFF \
        -DOPENSSL_ROOT_DIR=/usr
    cmake --build "$SRC/drogon/build-ssl" -j"$JOBS"
    cmake --install "$SRC/drogon/build-ssl" --prefix "$DROGON_PREFIX"
fi

missing=0
for f in \
    "$DROGON_PREFIX/include/drogon/drogon.h" \
    "$DROGON_PREFIX/lib/libdrogon.so" \
    "$PREFIX/usr/include/jsoncpp/json/json.h" \
    "$PREFIX/usr/include/yaml-cpp/yaml.h" \
    "$PREFIX/usr/include/sodium.h" \
    "$PREFIX/usr/include/sqlite3.h"; do
    if [[ ! -e "$f" ]]; then echo "missing: $f" >&2; missing=1; fi
done
if [[ $missing -ne 0 ]]; then
    echo "Dependency bootstrap incomplete" >&2
    exit 1
fi
echo "dependencies ready: $PREFIX and $DROGON_PREFIX"
