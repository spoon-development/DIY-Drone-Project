# services/video-stream

USB webcam via **V4L2** and **ffmpeg**, serving **multipart MJPEG** over HTTP for the HMI (`<img src="...">`).

## Environment

| Variable | Default | Description |
|----------|---------|-------------|
| `V4L2_DEVICE` | `/dev/video0` | Capture device |
| `V4L2_INPUT_FORMAT` | `mjpeg` | `mjpeg` (copy) or `yuyv422` (transcode to MJPEG) |
| `VIDEO_WIDTH` / `VIDEO_HEIGHT` / `VIDEO_FPS` | `640` / `480` / `25` | Capture size |
| `FFMPEG_LOGLEVEL` | `info` | ffmpeg log level |
| `USE_TEST_PATTERN` | `0` | `1` = color bars, no camera |

## Notes

- Compose uses `privileged: true` so the container can see host `/dev/video*` before you tighten rules.
- If MJPEG mode fails, try `V4L2_INPUT_FORMAT=yuyv422` and often **640×480**.
