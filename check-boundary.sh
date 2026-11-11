#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Fail if a platform API leaks into the portable core or public headers.
set -eu

cd "$(dirname "$0")"

if grep -rnE --include='*.c' --include='*.h' '\b(rt_|aicos_|aic_|esp_|vTask|xTask|FreeRTOS)\w*' core include; then
    echo "check-boundary: platform API found in core/ or include/" >&2
    exit 1
fi

if grep -rnE --include='*.c' --include='*.h' '#include[[:space:]]*[<"](rtthread|rtdevice|lwip|esp_|aic_)' core include; then
    echo "check-boundary: platform header included by core/ or include/" >&2
    exit 1
fi

echo "check-boundary: OK"
