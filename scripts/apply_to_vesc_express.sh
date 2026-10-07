#!/usr/bin/env bash
# Apply the P4 Dashboard port to a clean vesc_express checkout.
#
#   scripts/apply_to_vesc_express.sh /path/to/vesc_express
#
# - copies main/hwconf/p4_dashboard into the checkout
# - applies main/hwconf/p4_dashboard/port/upstream.patch (LVGL sources, lvgl component)
# - installs sdkconfig.defaults.esp32p4_dashboard
set -euo pipefail

if [ $# -ne 1 ] || [ ! -d "$1/main" ]; then
  echo "usage: $0 /path/to/vesc_express   (a checkout that contains main/)" >&2
  exit 1
fi

HERE="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="$(cd "$1" && pwd)"
SRC="$HERE/main/hwconf/p4_dashboard"

echo "==> copying hwconf/p4_dashboard"
mkdir -p "$TARGET/main/hwconf"
rm -rf "$TARGET/main/hwconf/p4_dashboard"
cp -R "$SRC" "$TARGET/main/hwconf/p4_dashboard"

echo "==> applying upstream.patch"
cd "$TARGET"
if grep -q "lvbr_core.c" main/CMakeLists.txt && grep -q "lvgl/lvgl" main/idf_component.yml; then
  echo "    main/CMakeLists.txt and main/idf_component.yml already contain the port, skipping"
elif git apply --check "$SRC/port/upstream.patch" 2>/dev/null; then
  git apply "$SRC/port/upstream.patch"
  echo "    applied"
else
  echo "    the patch does not apply to this vesc_express revision." >&2
  echo "    Merge $HERE/main/CMakeLists.txt and $HERE/main/idf_component.yml by hand." >&2
  exit 1
fi

echo "==> sdkconfig.defaults.esp32p4_dashboard"
cp "$SRC/sdkconfig.defaults" "$TARGET/sdkconfig.defaults.esp32p4_dashboard"

cat <<MSG

Done. Build and flash:

  idf.py -B build_p4 -DHW_NAME="P4 DASHBOARD" -DSDKCONFIG=build_p4/sdkconfig build
  idf.py -B build_p4 -DHW_NAME="P4 DASHBOARD" -DSDKCONFIG=build_p4/sdkconfig -p <port> flash
MSG
