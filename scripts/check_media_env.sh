#!/usr/bin/env bash

set -u

failures=0
warnings=0

pass() {
    printf '[PASS] %s\n' "$1"
}

fail() {
    printf '[FAIL] %s\n' "$1"
    failures=$((failures + 1))
}

warn() {
    printf '[WARN] %s\n' "$1"
    warnings=$((warnings + 1))
}

check_device() {
    device_path=$1
    description=$2
    if [[ ! -e "$device_path" ]]; then
        fail "$description is missing: $device_path"
    elif [[ ! -r "$device_path" || ! -w "$device_path" ]]; then
        fail "$description is not readable and writable: $device_path"
    else
        pass "$description is accessible: $device_path"
    fi
}

architecture=$(uname -m)
kernel=$(uname -r)
printf 'RK media environment report\n'
printf 'architecture=%s kernel=%s\n' "$architecture" "$kernel"

if [[ "$architecture" == "aarch64" ]]; then
    pass 'running on aarch64'
else
    fail "expected aarch64 RK3588 host, found $architecture"
fi

check_device /dev/mpp_service 'MPP device'
check_device /dev/rga 'RGA device'

if [[ -d /dev/dri ]] && find /dev/dri -maxdepth 1 -type c -readable -writable \
    -print -quit 2>/dev/null | grep -q .; then
    pass 'at least one DRM device is readable and writable'
else
    fail 'no readable and writable DRM device found under /dev/dri'
fi

if compgen -G '/dev/video*' >/dev/null; then
    pass 'one or more V4L2 devices are present'
else
    warn 'no V4L2 device found (acceptable for RTSP/file-only tests)'
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
    fail 'ffmpeg is not installed'
else
    ffmpeg_version=$(ffmpeg -version 2>/dev/null | sed -n '1p')
    printf 'ffmpeg=%s\n' "$ffmpeg_version"

    if ffmpeg -hide_banner -decoders 2>/dev/null | grep -q 'h264_rkmpp'; then
        pass 'h264_rkmpp decoder is available'
    else
        fail 'h264_rkmpp decoder is unavailable'
    fi
    if ffmpeg -hide_banner -decoders 2>/dev/null | grep -q 'hevc_rkmpp'; then
        pass 'hevc_rkmpp decoder is available'
    else
        fail 'hevc_rkmpp decoder is unavailable'
    fi
    if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q 'h264_rkmpp'; then
        pass 'h264_rkmpp encoder is available'
    else
        fail 'h264_rkmpp encoder is unavailable'
    fi
    if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q 'hevc_rkmpp'; then
        pass 'hevc_rkmpp encoder is available'
    else
        fail 'hevc_rkmpp encoder is unavailable'
    fi
    if ffmpeg -hide_banner -filters 2>/dev/null | grep -q 'scale_rkrga'; then
        pass 'scale_rkrga filter is available'
    else
        fail 'scale_rkrga filter is unavailable'
    fi
fi

if command -v mediamtx >/dev/null 2>&1; then
    mediamtx_version=$(mediamtx --version 2>&1 | sed -n '1p')
    pass "MediaMTX is installed: $mediamtx_version"
else
    fail 'mediamtx is not installed or not in PATH'
fi

printf 'summary: failures=%d warnings=%d\n' "$failures" "$warnings"
if ((failures != 0)); then
    exit 1
fi
