#!/usr/bin/env bash

set -uo pipefail

channels=0
mode=mpp-rga
input=''
output_dir=''
output_width=1280
output_height=720
fps=25
bitrate_kbps=3000
warmup_sec=2
sample_sec=10
interval_ms=1000
validation_sec=3
runner=${BENCHMARK_RUNNER:-/usr/local/share/rk-media-gateway/scripts/run_transcode_benchmark.sh}
runner_pids=()

usage() {
    cat <<'EOF'
Usage: run_capacity_benchmark.sh --channels N --input FILE --output-dir DIR [options]

Options:
  --mode MODE            software, mpp, or mpp-rga; default: mpp-rga
  --output-width N       default: 1280
  --output-height N      default: 720
  --fps N                default: 25
  --bitrate-kbps N       default: 3000
  --warmup-sec N         default: 2
  --sample-sec N         default: 10
  --interval-ms N        default: 1000
  --validation-sec N     default: 3
  --runner PATH          single-channel benchmark runner

The same fixed local file is intentionally repeated across 1 to 8 concurrent channels.
The output directory must not already exist.
EOF
}

fail() {
    printf 'capacity benchmark error: %s\n' "$1" >&2
    exit 1
}

parse_positive() {
    local name=$1
    local value=$2

    [[ $value =~ ^[0-9]+$ ]] || fail "$name must be an integer"
    ((value > 0)) || fail "$name must be greater than zero"
}

parse_nonnegative() {
    local name=$1
    local value=$2

    [[ $value =~ ^[0-9]+$ ]] || fail "$name must be an integer"
}

stop_runners() {
    local pid

    for pid in "${runner_pids[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
        fi
    done
    for pid in "${runner_pids[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
    runner_pids=()
}

record_temperature() {
    if [[ -r /sys/class/thermal/thermal_zone0/temp ]]; then
        cat /sys/class/thermal/thermal_zone0/temp
    else
        printf 'unavailable\n'
    fi
}

while (($# > 0)); do
    case $1 in
        --channels|--mode|--input|--output-dir|--output-width|--output-height|--fps|\
        --bitrate-kbps|--warmup-sec|--sample-sec|--interval-ms|--validation-sec|--runner)
            (($# >= 2)) || fail "missing value for $1"
            option=$1
            value=$2
            shift 2
            case $option in
                --channels) channels=$value ;;
                --mode) mode=$value ;;
                --input) input=$value ;;
                --output-dir) output_dir=$value ;;
                --output-width) output_width=$value ;;
                --output-height) output_height=$value ;;
                --fps) fps=$value ;;
                --bitrate-kbps) bitrate_kbps=$value ;;
                --warmup-sec) warmup_sec=$value ;;
                --sample-sec) sample_sec=$value ;;
                --interval-ms) interval_ms=$value ;;
                --validation-sec) validation_sec=$value ;;
                --runner) runner=$value ;;
            esac
            ;;
        --help)
            usage
            exit 0
            ;;
        *) fail "unknown option: $1" ;;
    esac
done

parse_positive channels "$channels"
((channels <= 8)) || fail 'channels must not exceed 8'
case $mode in
    software | mpp | mpp-rga) ;;
    *) fail "unsupported mode: $mode" ;;
esac
[[ -f $input && -r $input ]] || fail 'input must be a readable local regular file'
[[ $output_dir == /* && $output_dir != / ]] ||
    fail 'output-dir must be an absolute non-root path'
[[ ! -e $output_dir ]] || fail 'output-dir already exists'
[[ -x $runner ]] || fail 'runner is not executable'
parse_positive output-width "$output_width"
parse_positive output-height "$output_height"
parse_positive fps "$fps"
parse_positive bitrate-kbps "$bitrate_kbps"
parse_nonnegative warmup-sec "$warmup_sec"
parse_positive sample-sec "$sample_sec"
parse_positive interval-ms "$interval_ms"
parse_positive validation-sec "$validation_sec"

mkdir -p "$output_dir" || fail 'cannot create output-dir'
{
    printf 'channels=%s\nmode=%s\n' "$channels" "$mode"
    printf 'input=%s\noutput=%sx%s@%s\n' "$input" "$output_width" "$output_height" "$fps"
    printf 'bitrate_kbps=%s\nwarmup_sec=%s\nsample_sec=%s\n' \
        "$bitrate_kbps" "$warmup_sec" "$sample_sec"
    printf 'start=%s\nstart_temperature_millicelsius=' "$(date +%FT%T%z)"
    record_temperature
} >"$output_dir/capacity-meta.log"

trap stop_runners EXIT
trap 'stop_runners; trap - EXIT INT TERM; exit 130' INT
trap 'stop_runners; trap - EXIT INT TERM; exit 143' TERM

for ((index = 1; index <= channels; index++)); do
    channel=$(printf 'channel-%02d' "$index")
    "$runner" --mode "$mode" --input "$input" --output-dir "$output_dir/$channel" \
        --output-width "$output_width" --output-height "$output_height" --fps "$fps" \
        --bitrate-kbps "$bitrate_kbps" --warmup-sec "$warmup_sec" \
        --sample-sec "$sample_sec" --interval-ms "$interval_ms" \
        --validation-sec "$validation_sec" \
        >"$output_dir/$channel-runner.log" 2>&1 &
    runner_pids+=("$!")
done

failed=0
for index in "${!runner_pids[@]}"; do
    if ! wait "${runner_pids[$index]}"; then
        printf 'channel-%02d failed\n' "$((index + 1))" >&2
        failed=1
    fi
done
runner_pids=()
trap - EXIT INT TERM
((failed == 0)) || fail 'one or more channel benchmarks failed'

printf 'channel,target,pid,available_samples,unavailable_samples,cpu_samples,cpu_avg_percent,cpu_peak_percent,rss_avg_kib,rss_peak_kib,fd_min,fd_max\n' \
    >"$output_dir/capacity-summary.csv"
for ((index = 1; index <= channels; index++)); do
    channel=$(printf 'channel-%02d' "$index")
    summary=$output_dir/$channel/$mode-${output_width}x${output_height}-metrics-summary.csv
    [[ -s $summary ]] || fail "missing summary for $channel"
    awk -F, -v channel="$channel" 'NR == 2 { print channel "," $0; found = 1 }
        END { if (!found) exit 1 }' "$summary" >>"$output_dir/capacity-summary.csv" ||
        fail "invalid summary for $channel"
done

awk -F, '
    NR > 1 {
        available += $4
        unavailable += $5
        cpu_samples += $6
        cpu_avg_sum += $7
        rss_avg_sum += $9
        rss_peak_sum += $10
        fd_max_sum += $12
        count++
    }
    END {
        print "channels,available_samples,unavailable_samples,cpu_samples,cpu_avg_sum_percent,rss_avg_sum_kib,rss_peak_sum_kib,fd_max_sum"
        printf "%d,%d,%d,%d,%.2f,%.2f,%.2f,%d\n", count, available, unavailable,
            cpu_samples, cpu_avg_sum, rss_avg_sum, rss_peak_sum, fd_max_sum
    }
' "$output_dir/capacity-summary.csv" >"$output_dir/capacity-total.csv" ||
    fail 'cannot aggregate capacity summary'

{
    printf 'end=%s\nend_temperature_millicelsius=' "$(date +%FT%T%z)"
    record_temperature
    printf 'result=PASS\n'
} >>"$output_dir/capacity-meta.log"

printf 'capacity benchmark complete: channels=%s mode=%s output=%s\n' \
    "$channels" "$mode" "$output_dir"
