#!/usr/bin/env bash

set -euo pipefail

capacity_runner=$1
test_dir=$(mktemp -d /tmp/gateway-capacity-runner.XXXXXX)
trap 'rm -rf "$test_dir"' EXIT
input=$test_dir/input.mkv
fake_runner=$test_dir/fake-runner
ready_dir=$test_dir/ready
touch "$input"
mkdir "$ready_dir"

cat >"$fake_runner" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
mode=''
output_dir=''
width=''
height=''
while (($# > 0)); do
    case $1 in
        --mode) mode=$2; shift 2 ;;
        --output-dir) output_dir=$2; shift 2 ;;
        --output-width) width=$2; shift 2 ;;
        --output-height) height=$2; shift 2 ;;
        *) shift 2 ;;
    esac
done
mkdir -p "$output_dir"
touch "$GW_CAPACITY_READY/$$"
for ((attempt = 0; attempt < 100; attempt++)); do
    ready=$(find "$GW_CAPACITY_READY" -maxdepth 1 -type f | wc -l)
    ((ready >= GW_CAPACITY_EXPECTED)) && break
    sleep 0.01
done
((ready >= GW_CAPACITY_EXPECTED))
prefix=$output_dir/$mode-${width}x${height}
printf 'target,pid,available_samples,unavailable_samples,cpu_samples,cpu_avg_percent,cpu_peak_percent,rss_avg_kib,rss_peak_kib,fd_min,fd_max\n' >"$prefix-metrics-summary.csv"
printf 'ffmpeg,%s,11,0,10,25.00,30.00,1024.00,1100,5,6\n' "$$" >>"$prefix-metrics-summary.csv"
EOF
chmod +x "$fake_runner"
export GW_CAPACITY_READY=$ready_dir
export GW_CAPACITY_EXPECTED=4

output=$test_dir/results
"$capacity_runner" --channels 4 --mode mpp-rga --input "$input" \
    --output-dir "$output" --runner "$fake_runner" --warmup-sec 0 \
    --sample-sec 1 --interval-ms 100 --validation-sec 1

test "$(find "$output" -mindepth 1 -maxdepth 1 -type d | wc -l)" -eq 4
grep -qx '4,44,0,40,100.00,4096.00,4400.00,24' "$output/capacity-total.csv"
grep -qx 'result=PASS' "$output/capacity-meta.log"
if "$capacity_runner" --channels 4 --mode mpp-rga --input "$input" \
    --output-dir "$output" --runner "$fake_runner" >/dev/null 2>&1; then
    printf 'existing capacity output was unexpectedly reused\n' >&2
    exit 1
fi
if "$capacity_runner" --channels 9 --mode mpp-rga --input "$input" \
    --output-dir "$test_dir/too-many" --runner "$fake_runner" >/dev/null 2>&1; then
    printf 'more than eight channels unexpectedly succeeded\n' >&2
    exit 1
fi
if grep -q 'eval' "$capacity_runner"; then
    printf 'capacity runner must not use eval\n' >&2
    exit 1
fi

printf 'capacity runner tests passed\n'
