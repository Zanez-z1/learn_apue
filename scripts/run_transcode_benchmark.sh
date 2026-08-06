#!/usr/bin/env bash

set -uo pipefail

mode=''
input=''
output_dir=''
output_width=1920
output_height=1080
fps=25
bitrate_kbps=6000
warmup_sec=2
sample_sec=10
interval_ms=1000
validation_sec=3
software_ffmpeg=${SOFTWARE_FFMPEG:-/usr/bin/ffmpeg}
rockchip_ffmpeg=${ROCKCHIP_FFMPEG:-/usr/local/bin/ffmpeg}
ffprobe_binary=${FFPROBE_BINARY:-/usr/local/bin/ffprobe}
metrics_binary=${METRICS_BINARY:-/usr/local/bin/gateway-metrics}
child_pid=''

usage() {
    cat <<'EOF'
Usage: run_transcode_benchmark.sh --mode MODE --input FILE --output-dir DIR [options]

Modes: software, mpp, mpp-rga
Options:
  --output-width N       default: 1920
  --output-height N      default: 1080
  --fps N                default: 25
  --bitrate-kbps N       default: 6000
  --warmup-sec N         default: 2
  --sample-sec N         default: 10
  --interval-ms N        default: 1000
  --validation-sec N     default: 3
  --software-ffmpeg PATH default: /usr/bin/ffmpeg
  --rockchip-ffmpeg PATH default: /usr/local/bin/ffmpeg
  --ffprobe PATH         default: /usr/local/bin/ffprobe
  --metrics PATH         default: /usr/local/bin/gateway-metrics

Only a local fixed input file is accepted. Existing mode result files are not overwritten.
EOF
}

fail() {
    printf 'benchmark error: %s\n' "$1" >&2
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

require_executable() {
    local value=$1
    local description=$2

    if [[ $value == */* ]]; then
        [[ -x $value ]] || fail "$description is not executable: $value"
    else
        command -v "$value" >/dev/null 2>&1 || fail "$description is unavailable: $value"
    fi
}

stop_child() {
    local status

    if [[ -n $child_pid ]] && kill -0 "$child_pid" 2>/dev/null; then
        kill -INT "$child_pid" 2>/dev/null || true
        wait "$child_pid" 2>/dev/null
        status=$?
        if ((status != 0 && status != 130 && status != 255)); then
            printf 'benchmark warning: FFmpeg cleanup status=%d\n' "$status" >&2
        fi
    fi
    child_pid=''
}

handle_interrupt() {
    local status=$1

    stop_child
    trap - EXIT INT TERM
    exit "$status"
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
        --mode|--input|--output-dir|--output-width|--output-height|--fps|\
        --bitrate-kbps|--warmup-sec|--sample-sec|--interval-ms|\
        --validation-sec|--software-ffmpeg|--rockchip-ffmpeg|--ffprobe|--metrics)
            (($# >= 2)) || fail "missing value for $1"
            option=$1
            value=$2
            shift 2
            case $option in
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
                --software-ffmpeg) software_ffmpeg=$value ;;
                --rockchip-ffmpeg) rockchip_ffmpeg=$value ;;
                --ffprobe) ffprobe_binary=$value ;;
                --metrics) metrics_binary=$value ;;
            esac
            ;;
        --help)
            usage
            exit 0
            ;;
        *)
            fail "unknown option: $1"
            ;;
    esac
done

case $mode in
    software | mpp | mpp-rga) ;;
    '') fail '--mode is required' ;;
    *) fail "unsupported mode: $mode" ;;
esac
[[ -n $input ]] || fail '--input is required'
[[ -f $input && -r $input ]] || fail "input is not a readable regular file: $input"
[[ -n $output_dir ]] || fail '--output-dir is required'
[[ $output_dir == /* && $output_dir != / ]] || fail '--output-dir must be an absolute non-root path'

parse_positive output-width "$output_width"
parse_positive output-height "$output_height"
parse_positive fps "$fps"
parse_positive bitrate-kbps "$bitrate_kbps"
parse_nonnegative warmup-sec "$warmup_sec"
parse_positive sample-sec "$sample_sec"
parse_positive interval-ms "$interval_ms"
parse_positive validation-sec "$validation_sec"
((interval_ms >= 100)) || fail 'interval-ms must be at least 100'

require_executable "$software_ffmpeg" 'software FFmpeg'
require_executable "$rockchip_ffmpeg" 'Rockchip FFmpeg'
require_executable "$ffprobe_binary" 'ffprobe'
require_executable "$metrics_binary" 'gateway-metrics'

input_dimensions=$(
    "$ffprobe_binary" -v error -select_streams v:0 \
        -show_entries stream=width,height -of csv=p=0:s=x "$input"
) || fail 'cannot probe input dimensions'
[[ $input_dimensions =~ ^[0-9]+x[0-9]+$ ]] || fail 'ffprobe returned invalid dimensions'
input_width=${input_dimensions%x*}
input_height=${input_dimensions#*x}
if [[ $mode == mpp ]] &&
    ((output_width != input_width || output_height != input_height)); then
    fail 'mpp mode cannot resize; use mpp-rga for a different output size'
fi

mkdir -p "$output_dir" || fail "cannot create output directory: $output_dir"
prefix=$output_dir/$mode-${output_width}x${output_height}
for suffix in command.txt meta.log metrics.csv progress.log stderr.log stdout.log \
    metrics-summary.csv output.mkv output-probe.txt output-decode.log; do
    [[ ! -e $prefix-$suffix ]] || fail "result already exists: $prefix-$suffix"
done

input_args=(-stream_loop -1)
video_args=()
case $mode in
    software)
        ffmpeg_binary=$software_ffmpeg
        input_args+=(-c:v h264 -i "$input")
        if ((output_width != input_width || output_height != input_height)); then
            video_args+=(-vf "scale=${output_width}:${output_height}:flags=bicubic,format=yuv420p")
        fi
        video_args+=(-an -c:v libx264 -preset veryfast -tune zerolatency
            -profile:v high -pix_fmt yuv420p -b:v "${bitrate_kbps}k"
            -r "$fps" -g $((fps * 2)))
        ;;
    mpp)
        ffmpeg_binary=$rockchip_ffmpeg
        input_args+=(-hwaccel rkmpp -hwaccel_output_format drm_prime
            -c:v h264_rkmpp -i "$input")
        video_args+=(-an -c:v h264_rkmpp -b:v "${bitrate_kbps}k" -r "$fps")
        ;;
    mpp-rga)
        ffmpeg_binary=$rockchip_ffmpeg
        input_args+=(-hwaccel rkmpp -hwaccel_output_format drm_prime
            -c:v h264_rkmpp -i "$input")
        video_args+=(-vf "scale_rkrga=w=${output_width}:h=${output_height}:format=nv12"
            -an -c:v h264_rkmpp -b:v "${bitrate_kbps}k" -r "$fps")
        ;;
esac

progress_args=(-progress "$prefix-progress.log")
if [[ $mode != software ]]; then
    progress_args+=(-stats_period 1)
fi
benchmark_command=("$ffmpeg_binary" -nostdin -hide_banner -loglevel warning
    "${progress_args[@]}" "${input_args[@]}" "${video_args[@]}" -f null -)

{
    printf 'mode=%s\n' "$mode"
    printf 'input=%s\n' "$input"
    printf 'input_dimensions=%sx%s\n' "$input_width" "$input_height"
    printf 'output_dimensions=%sx%s\n' "$output_width" "$output_height"
    printf 'fps=%s\nbitrate_kbps=%s\n' "$fps" "$bitrate_kbps"
    printf 'warmup_sec=%s\nsample_sec=%s\ninterval_ms=%s\n' \
        "$warmup_sec" "$sample_sec" "$interval_ms"
    printf 'start=%s\nstart_temperature_millicelsius=' "$(date +%FT%T%z)"
    record_temperature
} >"$prefix-meta.log"
printf '%q ' "${benchmark_command[@]}" >"$prefix-command.txt"
printf '\n' >>"$prefix-command.txt"

trap stop_child EXIT
trap 'handle_interrupt 130' INT
trap 'handle_interrupt 143' TERM
"${benchmark_command[@]}" >"$prefix-stdout.log" 2>"$prefix-stderr.log" &
child_pid=$!
sleep "$warmup_sec"
if ! kill -0 "$child_pid" 2>/dev/null; then
    wait "$child_pid"
    ffmpeg_status=$?
    child_pid=''
    fail "FFmpeg exited during warmup with status $ffmpeg_status"
fi

"$metrics_binary" --target "ffmpeg=$child_pid" --duration-sec "$sample_sec" \
    --interval-ms "$interval_ms" \
    --summary-output "$prefix-metrics-summary.csv" >"$prefix-metrics.csv"
metrics_status=$?
kill -INT "$child_pid" 2>/dev/null || true
wait "$child_pid"
ffmpeg_status=$?
child_pid=''
trap - EXIT INT TERM

((metrics_status == 0)) || fail "gateway-metrics failed with status $metrics_status"
if ((ffmpeg_status != 0 && ffmpeg_status != 130 && ffmpeg_status != 255)); then
    fail "FFmpeg stopped with unexpected status $ffmpeg_status"
fi

validation_input_args=("${input_args[@]:2}")
validation_command=("$ffmpeg_binary" -nostdin -hide_banner -loglevel error
    "${validation_input_args[@]}" -t "$validation_sec" "${video_args[@]}"
    -y "$prefix-output.mkv")
"${validation_command[@]}" >"$prefix-output-decode.log" 2>>"$prefix-stderr.log" ||
    fail 'validation encode failed'

"$ffprobe_binary" -v error -select_streams v:0 \
    -show_entries stream=codec_name,width,height,avg_frame_rate:format=duration,size \
    -of default=nw=1 "$prefix-output.mkv" >"$prefix-output-probe.txt" ||
    fail 'validation ffprobe failed'
grep -qx 'codec_name=h264' "$prefix-output-probe.txt" || fail 'validation codec is not H.264'
grep -qx "width=$output_width" "$prefix-output-probe.txt" || fail 'validation width mismatch'
grep -qx "height=$output_height" "$prefix-output-probe.txt" || fail 'validation height mismatch'
"$software_ffmpeg" -nostdin -v error -i "$prefix-output.mkv" -an -f null - \
    >>"$prefix-output-decode.log" 2>&1 || fail 'validation decode failed'

{
    printf 'end=%s\nend_temperature_millicelsius=' "$(date +%FT%T%z)"
    record_temperature
    printf 'metrics_status=%d\nffmpeg_status=%d\n' "$metrics_status" "$ffmpeg_status"
} >>"$prefix-meta.log"

printf 'benchmark complete: mode=%s output=%sx%s prefix=%s\n' \
    "$mode" "$output_width" "$output_height" "$prefix"
