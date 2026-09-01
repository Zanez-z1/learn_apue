#!/usr/bin/env bash

set -euo pipefail

runner=$1
ffmpeg_fixture=$2
test_dir=$(mktemp -d /tmp/gateway-benchmark-runner.XXXXXX)
trap 'rm -rf "$test_dir"' EXIT

input=$test_dir/input.mkv
output=$test_dir/results
touch "$input"

cat >"$test_dir/fake-ffprobe" <<'EOF'
#!/usr/bin/env bash
set -eu
last=''
for argument in "$@"; do
    last=$argument
done
if [[ $* == *stream=width,height* ]]; then
    printf '1920x1080\n'
elif [[ $last == *1280x720* ]]; then
    printf 'codec_name=h264\nwidth=1280\nheight=720\navg_frame_rate=25/1\n'
else
    printf 'codec_name=h264\nwidth=1920\nheight=1080\navg_frame_rate=25/1\n'
fi
EOF

cat >"$test_dir/fake-metrics" <<'EOF'
#!/usr/bin/env bash
set -eu
summary=''
while (($# > 0)); do
    if [[ $1 == --summary-output && $# -ge 2 ]]; then
        summary=$2
        shift 2
    else
        shift
    fi
done
[[ -n $summary ]]
printf 'target,pid,available_samples,unavailable_samples,cpu_samples,cpu_avg_percent,cpu_peak_percent,rss_avg_kib,rss_peak_kib,fd_min,fd_max\n' >"$summary"
printf 'ffmpeg,123,2,0,1,10.00,10.00,1000.00,1000,4,4\n' >>"$summary"
printf 'elapsed_ms,target,pid,status,cpu_percent,rss_kib,fd_count\n'
printf '0,ffmpeg,123,ok,,1000,4\n'
printf '1000,ffmpeg,123,ok,10.00,1000,4\n'
EOF

chmod +x "$test_dir/fake-ffprobe" "$test_dir/fake-metrics"
export GW_BENCH_FIXTURE_LOG=$test_dir/ffmpeg-argv.log

common=(--input "$input" --output-dir "$output" --warmup-sec 1 --sample-sec 1
    --interval-ms 100 --validation-sec 1 --software-ffmpeg "$ffmpeg_fixture"
    --rockchip-ffmpeg "$ffmpeg_fixture" --ffprobe "$test_dir/fake-ffprobe"
    --metrics "$test_dir/fake-metrics")

"$runner" --mode software --output-width 1280 --output-height 720 "${common[@]}"
"$runner" --mode mpp-rga --output-width 1280 --output-height 720 "${common[@]}"
if "$runner" --mode mpp --output-width 1280 --output-height 720 "${common[@]}"; then
    printf 'mpp resize unexpectedly succeeded\n' >&2
    exit 1
fi
if "$runner" --mode software --output-width 1280 --output-height 720 "${common[@]}"; then
    printf 'existing result was unexpectedly overwritten\n' >&2
    exit 1
fi

test -s "$output/software-1280x720-metrics.csv"
test -s "$output/mpp-rga-1280x720-metrics.csv"
test -s "$output/software-1280x720-metrics-summary.csv"
test -s "$output/mpp-rga-1280x720-metrics-summary.csv"
test -s "$output/software-1280x720-output.mkv"
test -s "$output/mpp-rga-1280x720-output.mkv"
grep -q 'scale=1280:720:flags=bicubic,format=yuv420p' "$test_dir/ffmpeg-argv.log"
grep -q 'scale_rkrga=w=1280:h=720:format=nv12' "$test_dir/ffmpeg-argv.log"
if grep -q 'eval' "$runner"; then
    printf 'runner must not use eval\n' >&2
    exit 1
fi

printf 'benchmark runner tests passed\n'
