#!/usr/bin/env bash

set -u
set -o pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
config_path="$script_dir/../config/mediamtx.pc-source.yml"
device=/dev/video0
board_ip=
mediamtx_binary=${MEDIAMTX_BINARY:-}
check_only=false
ffmpeg_pid=
mediamtx_pid=

usage() {
    cat <<'EOF'
Usage: run_pc_camera_source.sh --board-ip ADDRESS [options]

Options:
  --board-ip ADDRESS   RK3588 address used to select and print the PC source IP
  --device PATH        V4L2 camera device (default: /dev/video0)
  --mediamtx PATH      persistent MediaMTX v1.20 binary
  --config PATH        PC source configuration
  --check-only         run preflight checks without starting processes
  -h, --help           show this help
EOF
}

fail() {
    printf '[FAIL] %s\n' "$1" >&2
    exit 1
}

pass() {
    printf '[PASS] %s\n' "$1"
}

stop_child() {
    local pid=$1
    local name=$2
    local attempt

    if [[ -z "$pid" ]] || ! kill -0 "$pid" 2>/dev/null; then
        return
    fi
    printf '[STOP] %s pid=%s\n' "$name" "$pid"
    kill -TERM "$pid" 2>/dev/null || true
    for attempt in {1..30}; do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null || true
            return
        fi
        sleep 0.1
    done
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

cleanup() {
    stop_child "$ffmpeg_pid" FFmpeg
    stop_child "$mediamtx_pid" MediaMTX
}

trap cleanup EXIT INT TERM

while (($# > 0)); do
    case "$1" in
    --board-ip)
        (($# >= 2)) || fail '--board-ip requires a value'
        board_ip=$2
        shift 2
        ;;
    --device)
        (($# >= 2)) || fail '--device requires a value'
        device=$2
        shift 2
        ;;
    --mediamtx)
        (($# >= 2)) || fail '--mediamtx requires a value'
        mediamtx_binary=$2
        shift 2
        ;;
    --config)
        (($# >= 2)) || fail '--config requires a value'
        config_path=$2
        shift 2
        ;;
    --check-only)
        check_only=true
        shift
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        fail "unknown argument: $1"
        ;;
    esac
done

[[ -n "$board_ip" ]] || fail 'provide the RK3588 address with --board-ip'
[[ -r "$config_path" ]] || fail "MediaMTX source config is not readable: $config_path"
[[ -c "$device" && -r "$device" ]] ||
    fail "camera is not a readable character device: $device"

for command_name in awk ffmpeg ffprobe ip realpath ss v4l2-ctl; do
    command -v "$command_name" >/dev/null 2>&1 ||
        fail "required command is unavailable: $command_name"
done

if [[ -z "$mediamtx_binary" ]]; then
    if command -v mediamtx >/dev/null 2>&1; then
        mediamtx_binary=$(command -v mediamtx)
    elif [[ -x "${HOME}/mediamtx/mediamtx" ]]; then
        mediamtx_binary="${HOME}/mediamtx/mediamtx"
    else
        fail 'MediaMTX was not found; use --mediamtx with a persistent binary path'
    fi
fi
[[ -x "$mediamtx_binary" ]] || fail "MediaMTX is not executable: $mediamtx_binary"
mediamtx_binary=$(realpath -- "$mediamtx_binary") ||
    fail 'cannot resolve the MediaMTX binary path'
config_path=$(realpath -- "$config_path") ||
    fail 'cannot resolve the MediaMTX source config path'
[[ "$mediamtx_binary" != /tmp/* && "$config_path" != /tmp/* ]] ||
    fail 'MediaMTX binary and config must use persistent paths outside /tmp'

mediamtx_version=$("$mediamtx_binary" --version 2>&1) ||
    fail 'cannot execute MediaMTX --version'
[[ "$mediamtx_version" == v1.20.* ]] ||
    fail "this demo was verified with MediaMTX v1.20.x, found: $mediamtx_version"
pass "MediaMTX version: $mediamtx_version"

route_output=$(ip -4 route get "$board_ip" 2>&1) ||
    fail "cannot route to RK3588 address $board_ip: $route_output"
if [[ "$route_output" =~ src[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+) ]]; then
    pc_ip=${BASH_REMATCH[1]}
else
    fail "cannot determine the PC IPv4 source address for $board_ip"
fi
pass "PC source address: $pc_ip (route to $board_ip)"

camera_formats=$(v4l2-ctl --device "$device" --list-formats-ext 2>&1) || {
    printf '%s\n' "$camera_formats" >&2
    fail "cannot enumerate V4L2 formats for $device"
}
if ! printf '%s\n' "$camera_formats" | awk '
    /^\t\[[0-9]+\]:/ {
        in_mjpg = index($0, "'\''MJPG'\''") > 0
        in_size = 0
    }
    in_mjpg && /Size: Discrete 1280x720/ {
        in_size = 1
        next
    }
    in_mjpg && in_size && /Size: Discrete/ {
        in_size = 0
    }
    in_mjpg && in_size && /30(\.0+)? fps/ {
        found = 1
    }
    END {
        exit found ? 0 : 1
    }
'; then
    printf '%s\n' "$camera_formats" >&2
    fail "$device does not advertise MJPEG 1280x720 at 30 fps"
fi
pass "$device supports MJPEG 1280x720 at 30 fps"

if ss -H -ltn 'sport = :8554' | grep -q .; then
    fail 'TCP 8554 is already in use; stop or reuse that known MediaMTX explicitly'
fi
pass 'TCP 8554 is available'

printf '[INFO] RK3588 input URL must be rtsp://%s:8554/source\n' "$pc_ip"
if [[ "$check_only" == true ]]; then
    pass 'PC source preflight completed'
    exit 0
fi

state_root=${XDG_STATE_HOME:-${HOME}/.local/state}
state_dir="$state_root/rk-media-gateway-demo"
umask 077
mkdir -p "$state_dir" || fail "cannot create state directory: $state_dir"
mediamtx_log="$state_dir/pc-mediamtx.log"

"$mediamtx_binary" "$config_path" >"$mediamtx_log" 2>&1 &
mediamtx_pid=$!
for attempt in {1..50}; do
    if ! kill -0 "$mediamtx_pid" 2>/dev/null; then
        tail -n 20 "$mediamtx_log" >&2
        fail 'PC MediaMTX exited during startup'
    fi
    if ss -H -ltn 'sport = :8554' | grep -q .; then
        break
    fi
    sleep 0.1
done
ss -H -ltn 'sport = :8554' | grep -q . ||
    fail 'PC MediaMTX did not listen on TCP 8554 within five seconds'
pass "PC MediaMTX pid=$mediamtx_pid log=$mediamtx_log"

start_publisher() {
    local attempt
    local source_ready=false

    ffmpeg -hide_banner -nostdin \
        -f v4l2 \
        -input_format mjpeg \
        -video_size 1280x720 \
        -framerate 30 \
        -i "$device" \
        -vf 'scale=1920:1080,fps=25' \
        -an \
        -c:v libx264 \
        -preset veryfast \
        -tune zerolatency \
        -pix_fmt yuv420p \
        -b:v 8M \
        -g 50 \
        -f rtsp \
        -rtsp_transport tcp \
        rtsp://127.0.0.1:8554/source &
    ffmpeg_pid=$!

    for attempt in {1..30}; do
        if ! kill -0 "$ffmpeg_pid" 2>/dev/null; then
            fail 'FFmpeg camera publisher exited before the RTSP source became ready'
        fi
        if ffprobe -v error -rtsp_transport tcp \
            -show_entries stream=codec_name,width,height,avg_frame_rate \
            -of default=noprint_wrappers=1 \
            rtsp://127.0.0.1:8554/source >/dev/null 2>&1; then
            source_ready=true
            break
        fi
        sleep 0.5
    done
    [[ "$source_ready" == true ]] ||
        fail 'RTSP source did not become readable within 15 seconds'
    pass "PC camera source is ready: rtsp://$pc_ip:8554/source pid=$ffmpeg_pid"
}

print_controls() {
    printf '[READY] Enter s to stop only FFmpeg, r to publish again, or q to stop both.\n'
}

start_publisher
print_controls
while true; do
    command=
    if [[ -n "$ffmpeg_pid" ]] && ! kill -0 "$ffmpeg_pid" 2>/dev/null; then
        wait "$ffmpeg_pid" 2>/dev/null
        ffmpeg_status=$?
        ffmpeg_pid=
        fail "FFmpeg camera publisher exited unexpectedly with status $ffmpeg_status"
    fi
    kill -0 "$mediamtx_pid" 2>/dev/null || fail 'PC MediaMTX exited unexpectedly'
    if IFS= read -r -t 1 command; then
        case "${command,,}" in
        s|stop)
            if [[ -z "$ffmpeg_pid" ]]; then
                printf '[INFO] FFmpeg publisher is already stopped.\n'
            else
                stop_child "$ffmpeg_pid" FFmpeg
                ffmpeg_pid=
                pass 'FFmpeg publisher stopped; PC MediaMTX remains active'
            fi
            ;;
        r|start)
            if [[ -n "$ffmpeg_pid" ]]; then
                printf '[INFO] FFmpeg publisher is already running with pid=%s.\n' \
                    "$ffmpeg_pid"
            else
                start_publisher
            fi
            ;;
        q|quit)
            exit 0
            ;;
        '')
            ;;
        *)
            printf '[INFO] Unknown command. '
            ;;
        esac
        print_controls
    fi
done
