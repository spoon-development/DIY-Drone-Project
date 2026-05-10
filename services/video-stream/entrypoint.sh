#!/usr/bin/env bash
set -euo pipefail

DEVICE="${V4L2_DEVICE:-/dev/video0}"
FORMAT="${V4L2_INPUT_FORMAT:-mjpeg}"
WIDTH="${VIDEO_WIDTH:-640}"
HEIGHT="${VIDEO_HEIGHT:-480}"
FPS="${VIDEO_FPS:-25}"
FFMPEG_LOG="${FFMPEG_LOGLEVEL:-info}"
USE_TEST_PATTERN="${USE_TEST_PATTERN:-0}"

log() {
  echo "[video-stream] $(date -Iseconds) $*"
}

if [[ "${USE_TEST_PATTERN}" == "1" ]]; then
  log "USE_TEST_PATTERN=1 — serving synthetic MJPEG (no camera required)."
  exec ffmpeg -hide_banner -loglevel "${FFMPEG_LOG}" \
    -f lavfi -i "testsrc=size=${WIDTH}x${HEIGHT}:rate=${FPS}" \
    -c:v mjpeg -q:v 5 \
    -listen 1 -f mpjpeg "http://0.0.0.0:8080/cam.mjpg"
fi

if [[ ! -c "${DEVICE}" && ! -e "${DEVICE}" ]]; then
  log "ERROR: ${DEVICE} not found. Plug the USB camera, check V4L2_DEVICE, or set USE_TEST_PATTERN=1."
  exit 1
fi

log "device=${DEVICE} format=${FORMAT} size=${WIDTH}x${HEIGHT} fps=${FPS}"
log "v4l2 devices (best-effort):"
v4l2-ctl --list-devices 2>/dev/null || log "(v4l2-ctl listing skipped)"

if [[ "${FORMAT}" == "mjpeg" ]]; then
  exec ffmpeg -hide_banner -loglevel "${FFMPEG_LOG}" -stats \
    -f v4l2 -thread_queue_size 512 \
    -input_format mjpeg -video_size "${WIDTH}x${HEIGHT}" -framerate "${FPS}" \
    -i "${DEVICE}" \
    -c copy \
    -listen 1 -f mpjpeg "http://0.0.0.0:8080/cam.mjpg"
fi

if [[ "${FORMAT}" == "yuyv422" ]]; then
  exec ffmpeg -hide_banner -loglevel "${FFMPEG_LOG}" -stats \
    -f v4l2 -thread_queue_size 512 \
    -input_format yuyv422 -video_size "${WIDTH}x${HEIGHT}" -framerate "${FPS}" \
    -i "${DEVICE}" \
    -c:v mjpeg -q:v 4 \
    -listen 1 -f mpjpeg "http://0.0.0.0:8080/cam.mjpg"
fi

log "ERROR: unsupported V4L2_INPUT_FORMAT=${FORMAT} (use mjpeg or yuyv422)"
exit 1
