#!/usr/bin/env bash

set -euo pipefail

runner=$1
test_dir=$(mktemp -d "$PWD/gateway-pc-source.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT

cat >"$test_dir/mediamtx" <<'EOF'
#!/usr/bin/env bash
if [[ ${1:-} == --version ]]; then
    printf 'v1.20.0\n'
    exit 0
fi
exit 1
EOF

cat >"$test_dir/ffmpeg" <<'EOF'
#!/usr/bin/env bash
exit 1
EOF

cat >"$test_dir/ffprobe" <<'EOF'
#!/usr/bin/env bash
exit 1
EOF

cat >"$test_dir/ip" <<'EOF'
#!/usr/bin/env bash
printf '192.168.1.45 via 192.168.1.1 dev wlan0 src 192.168.1.16 uid 1000\n'
EOF

cat >"$test_dir/ss" <<'EOF'
#!/usr/bin/env bash
if [[ ${FAKE_PORT_BUSY:-0} == 1 ]]; then
    printf 'LISTEN 0 4096 0.0.0.0:8554 0.0.0.0:*\n'
fi
EOF

cat >"$test_dir/v4l2-ctl" <<'EOF'
#!/usr/bin/env bash
printf "ioctl: VIDIOC_ENUM_FMT\n"
if [[ ${FAKE_CAMERA_BAD:-0} == 1 ]]; then
    printf "\t[0]: 'YUYV'\n\t\tSize: Discrete 640x480\n"
    printf "\t\t\tInterval: Discrete 0.067s (15.000 fps)\n"
else
    printf "\t[0]: 'MJPG'\n\t\tSize: Discrete 1280x720\n"
    printf "\t\t\tInterval: Discrete 0.033s (30.000 fps)\n"
fi
EOF

chmod +x "$test_dir/mediamtx" "$test_dir/ffmpeg" "$test_dir/ffprobe" \
    "$test_dir/ip" "$test_dir/ss" "$test_dir/v4l2-ctl"

PATH="$test_dir:$PATH" "$runner" --board-ip 192.168.1.45 \
    --device /dev/null --mediamtx "$test_dir/mediamtx" --check-only \
    >"$test_dir/success.log" 2>&1
grep -q 'PC source address: 192.168.1.16' "$test_dir/success.log"
grep -q 'rtsp://192.168.1.16:8554/source' "$test_dir/success.log"
grep -q 'PC source preflight completed' "$test_dir/success.log"

if FAKE_CAMERA_BAD=1 PATH="$test_dir:$PATH" "$runner" \
    --board-ip 192.168.1.45 --device /dev/null \
    --mediamtx "$test_dir/mediamtx" --check-only \
    >"$test_dir/bad-camera.log" 2>&1; then
    printf 'unsupported camera format unexpectedly passed\n' >&2
    exit 1
fi
grep -q 'does not advertise MJPEG 1280x720 at 30 fps' \
    "$test_dir/bad-camera.log"

if FAKE_PORT_BUSY=1 PATH="$test_dir:$PATH" "$runner" \
    --board-ip 192.168.1.45 --device /dev/null \
    --mediamtx "$test_dir/mediamtx" --check-only \
    >"$test_dir/busy-port.log" 2>&1; then
    printf 'occupied TCP 8554 unexpectedly passed\n' >&2
    exit 1
fi
grep -q 'TCP 8554 is already in use' "$test_dir/busy-port.log"

if grep -Eq 'pkill|killall' "$runner"; then
    printf 'PC source runner must not use broad process termination\n' >&2
    exit 1
fi

printf 'PC camera source preflight tests passed.\n'
