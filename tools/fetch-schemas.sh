#!/usr/bin/env bash
# Fetch the two kernel device tree schemas the binding refers to, so that
# `make check` can validate it outside a kernel tree.
#
#   tools/fetch-schemas.sh [dir]     default ~/.cache/pisugar3-dt-schemas
set -euo pipefail
DIR=${1:-$HOME/.cache/pisugar3-dt-schemas}
BASE=https://raw.githubusercontent.com/torvalds/linux/master/Documentation/devicetree/bindings
mkdir -p "$DIR/power/supply"
for f in power-supply.yaml battery.yaml; do
    curl -sfL -o "$DIR/power/supply/$f" "$BASE/power/supply/$f"
done
echo "schemas in $DIR"
