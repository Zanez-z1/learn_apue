#!/usr/bin/env bash

set -euo pipefail

metrics_binary=$1
test_dir=$(mktemp -d /tmp/gateway-metrics-summary.XXXXXX)
trap 'rm -rf "$test_dir"' EXIT
summary=$test_dir/summary.csv
samples=$test_dir/samples.csv

"$metrics_binary" --target self=self --duration-sec 1 --interval-ms 200 \
    --summary-output "$summary" >"$samples"

grep -qx 'elapsed_ms,target,pid,status,cpu_percent,rss_kib,fd_count' \
    "$samples"
grep -qx 'target,pid,available_samples,unavailable_samples,cpu_samples,cpu_avg_percent,cpu_peak_percent,rss_avg_kib,rss_peak_kib,fd_min,fd_max' \
    "$summary"
awk -F, '
    NR == 2 {
        if ($1 != "self" || $3 < 2 || $4 != 0 || $5 < 1 ||
            $6 == "" || $7 == "" || $8 <= 0 || $9 <= 0 ||
            $10 < 3 || $11 < $10) {
            exit 1
        }
        valid = 1
    }
    END { if (!valid) exit 1 }
' "$summary"

if "$metrics_binary" --target self=self --duration-sec 1 --interval-ms 200 \
    --summary-output "$summary" >/dev/null 2>"$test_dir/overwrite-error.log"; then
    printf 'existing summary was unexpectedly overwritten\n' >&2
    exit 1
fi
grep -q 'cannot create summary' "$test_dir/overwrite-error.log"

set +e
"$metrics_binary" --target missing=2147483647 --duration-sec 1 --interval-ms 200 \
    --summary-output "$test_dir/unavailable-summary.csv" \
    >"$test_dir/unavailable-samples.csv" 2>"$test_dir/unavailable-error.log"
unavailable_status=$?
set -e
[[ $unavailable_status -eq 2 ]]
awk -F, '
    NR == 2 {
        if ($1 != "missing" || $3 != 0 || $4 < 2 || $5 != 0 ||
            $6 != "" || $7 != "" || $8 != "" || $9 != "" ||
            $10 != "" || $11 != "") {
            exit 1
        }
        valid = 1
    }
    END { if (!valid) exit 1 }
' "$test_dir/unavailable-summary.csv"
grep -q 'one or more targets became unavailable' "$test_dir/unavailable-error.log"

printf 'metrics summary tests passed\n'
