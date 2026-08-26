#!/bin/bash
set -e

SDK_ROOT="${SDK_ROOT:-/opt/fslc-xwayland/5.15-scarthgap}"
SDK_ENV="$SDK_ROOT/environment-setup-cortexa53-fslc-linux"
SDK_CMAKE="$SDK_ROOT/sysroots/x86_64-fslcsdk-linux/usr/bin/cmake"
SDK_CTEST="$SDK_ROOT/sysroots/x86_64-fslcsdk-linux/usr/bin/ctest"
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

usage() {
    cat <<'EOF'
Usage: build.sh <target> [options]

Targets:
  debug       Cross-compile Debug build (default)
  release     Cross-compile Release build (-Os, LTO)
  sanitize    Cross-compile Debug build with ASan + UBSan
  test        Native build + run unit tests
  clean       Remove all build directories

Options:
  --lib <build_dir>  Use locally built fs-updater-lib from this build directory
  --sanitize         With 'test': run the native suite under ASan/UBSan
                     (separate build-test-san/ dir; cross builds keep the
                     'sanitize' target).
EOF
    exit 1
}

TARGET=""
LIB_BUILD_DIR=""
EXTRA_ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
    --lib)
        LIB_BUILD_DIR="$(realpath "$2")"
        shift
        ;;
    --sanitize)
        TEST_SUFFIX="-san"
        ;;
    debug | release | sanitize | test | clean)
        if [ -n "$TARGET" ]; then
            echo "Multiple targets specified: $TARGET and $1"
            usage
        fi
        TARGET="$1"
        ;;
    *)
        echo "Unknown option: $1"
        usage
        ;;
    esac
    shift
done

TARGET="${TARGET:-debug}"

if [ -n "${TEST_SUFFIX:-}" ] && [ "$TARGET" != "test" ]; then
    echo "--sanitize applies to the 'test' target only (cross builds: use the 'sanitize' target)"
    exit 1
fi

build_cross() {
    local build_dir="$PROJECT_ROOT/build"
    local cmake_args=("$@")

    unset LD_LIBRARY_PATH
    source "$SDK_ENV"

    if [ -n "$LIB_BUILD_DIR" ]; then
        local lib_install="$PROJECT_ROOT/build/fus_lib_install"
        echo "Installing fs-updater-lib from $LIB_BUILD_DIR..."
        "$SDK_CMAKE" --install "$LIB_BUILD_DIR" --prefix "$lib_install"
        cmake_args+=("-DFUS_LIB_DIR=$lib_install")
    fi

    mkdir -p "$build_dir" && cd "$build_dir"
    "$SDK_CMAKE" "${cmake_args[@]}" "$PROJECT_ROOT"
    make -j"$(nproc)"
}

case "$TARGET" in
debug)
    build_cross -DCMAKE_BUILD_TYPE=Debug "${EXTRA_ARGS[@]}"
    ;;
release)
    build_cross -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-Os -flto" "${EXTRA_ARGS[@]}"
    ;;
sanitize)
    build_cross -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all" \
        "${EXTRA_ARGS[@]}"
    ;;
test)
    cmake_bin="$SDK_CMAKE"
    ctest_bin="$SDK_CTEST"
    if [ ! -x "$cmake_bin" ]; then
        cmake_bin="$(command -v cmake 2>/dev/null)" || { echo "cmake not found"; exit 1; }
        ctest_bin="$(command -v ctest 2>/dev/null)" || ctest_bin="$cmake_bin --build . --target test"
    fi

    build_dir="$PROJECT_ROOT/build-test${TEST_SUFFIX:-}"
    cmake_args=(
        -DCMAKE_BUILD_TYPE=Debug
        -DCMAKE_C_COMPILER=gcc
        -DCMAKE_CXX_COMPILER=g++
        -DBUILD_TESTING=ON
    )
    if [ -n "${TEST_SUFFIX:-}" ]; then
        cmake_args+=("-DENABLE_SANITIZERS=ON")
    fi
    vendor_libsystemd="$PROJECT_ROOT/vendor/libsystemd-dev"
    if [ -d "$vendor_libsystemd" ]; then
        cmake_args+=(
            "-DLIBSYSTEMD_INCLUDE=$vendor_libsystemd/include"
            "-DLIBSYSTEMD_LIB=$vendor_libsystemd/lib"
        )
        export LD_LIBRARY_PATH="$vendor_libsystemd/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    fi
    if [ -n "$LIB_BUILD_DIR" ]; then
        cmake_args+=("-DFUS_LIB_DIR=$LIB_BUILD_DIR")
    fi
    mkdir -p "$build_dir" && cd "$build_dir"
    "$cmake_bin" "${cmake_args[@]}" "$PROJECT_ROOT"
    make -j"$(nproc)"
    "$ctest_bin" --output-on-failure
    ;;
clean)
    rm -rf "$PROJECT_ROOT/build" "$PROJECT_ROOT/build-test" "$PROJECT_ROOT/build-test-san"
    echo "Build directories removed."
    ;;
*)
    usage
    ;;
esac
