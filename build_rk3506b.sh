#!/bin/sh
# =============================================================================
# build_rk3506b.sh — build tuyaos_demo_wukong_ai for ATK-DLRK3506B (RK3506B, armhf).
#
# Self-contained: SDK headers (include/), libs (libs/libtuyaos.a), the armhf
# toolchain (vendor/.../toolchain) and the TKL adapter source
# (vendor/.../tuyaos_adapter/src) all live inside this app directory.
#
# apps/ is flat — app source sits directly under apps/ (not apps/<name>/). The
# build system (scripts/Makefile, scripts/mk/app.mk, apps/local.mk,
# build/build_param, vendor/.../tuyaos/Makefile) is patched to build from this
# repo directly, so NO external TuyaOS tree is needed.
#
# Usage:   ./build_rk3506b.sh [version]      (default version 1.0.55)
# Output:  apps/output/<app>_<ver>/<app>
# =============================================================================
set -e
SELF=$(readlink -f "$0")
APP_DIR=$(dirname "$SELF")
APP_NAME=tuyaos_demo_wukong_ai
APP_VER="${1:-1.0.55}"

echo "App source : $APP_DIR"
echo "App/Version: $APP_NAME / $APP_VER"
echo

cd "$APP_DIR"
make -f scripts/Makefile app_by_name APP_NAME="$APP_NAME" APP_VER="$APP_VER"

echo
BIN="$APP_DIR/apps/output/${APP_NAME}_${APP_VER}/${APP_NAME}"
if [ -x "$BIN" ]; then
    echo "BUILD OK -> $BIN"
    file "$BIN" | sed 's/.*LSB /  arch: LSB /'
else
    echo "BUILD FAILED (no binary at $BIN)"; exit 1
fi
