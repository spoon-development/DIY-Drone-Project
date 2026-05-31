#include <httplib.h>
#include <nlohmann/json.hpp>
#include <turbojpeg.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>

using json = nlohmann::json;

namespace {

std::string getenv_or(const char* key, const std::string& fallback) {
  const char* v = std::getenv(key);
  return (v && *v) ? std::string(v) : fallback;
}

std::filesystem::path profiles_dir() {
  return std::filesystem::path(getenv_or("VIDEO_PROFILES_DIR", "/app/profiles"));
}

enum class PixelFormat { Mjpeg, Yuyv };

PixelFormat parse_pixel_format(const std::string& s) {
  std::string lower = s;
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (lower == "yuyv" || lower == "yuy2") return PixelFormat::Yuyv;
  return PixelFormat::Mjpeg;
}

const char* pixel_format_name(PixelFormat fmt) {
  return fmt == PixelFormat::Yuyv ? "yuyv" : "mjpeg";
}

struct CameraProfile {
  std::string id;
  std::string label;
  std::string interface_type{"usb"};
  std::string manufacturer;
  std::string model;
  std::string device{"/dev/video0"};
  std::string device_match;
  std::string capture;  // empty = v4l2; "rpicam-vid" = Pi libcamera pipeline
  int width{1280};
  int height{720};
  int fps{30};
  PixelFormat pixel_format{PixelFormat::Mjpeg};
  int jpeg_quality{85};
};

bool uses_rpicam(const CameraProfile& p) {
  std::string c = p.capture;
  for (char& ch : c) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return c == "rpicam-vid" || c == "rpicam";
}

json profile_to_json(const CameraProfile& p) {
  json j = {{"id", p.id},
            {"label", p.label},
            {"interface", p.interface_type},
            {"device", p.device},
            {"width", p.width},
            {"height", p.height},
            {"fps", p.fps},
            {"pixel_format", pixel_format_name(p.pixel_format)}};
  if (!p.manufacturer.empty()) j["manufacturer"] = p.manufacturer;
  if (!p.model.empty()) j["model"] = p.model;
  return j;
}

bool read_profile_file(const std::filesystem::path& p, CameraProfile& out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  json j;
  try {
    in >> j;
  } catch (...) {
    return false;
  }
  if (!j.is_object() || !j.contains("id") || !j["id"].is_string()) return false;
  out = CameraProfile{};
  out.id = j["id"].get<std::string>();
  if (j.contains("label") && j["label"].is_string()) out.label = j["label"].get<std::string>();
  else out.label = out.id;
  if (j.contains("interface") && j["interface"].is_string()) out.interface_type = j["interface"].get<std::string>();
  if (j.contains("manufacturer") && j["manufacturer"].is_string()) out.manufacturer = j["manufacturer"].get<std::string>();
  if (j.contains("model") && j["model"].is_string()) out.model = j["model"].get<std::string>();
  if (j.contains("device") && j["device"].is_string()) out.device = j["device"].get<std::string>();
  if (j.contains("device_match") && j["device_match"].is_string())
    out.device_match = j["device_match"].get<std::string>();
  if (j.contains("capture") && j["capture"].is_string()) out.capture = j["capture"].get<std::string>();
  if (j.contains("width")) out.width = static_cast<int>(j["width"].get<int64_t>());
  if (j.contains("height")) out.height = static_cast<int>(j["height"].get<int64_t>());
  if (j.contains("fps")) out.fps = static_cast<int>(j["fps"].get<int64_t>());
  if (j.contains("jpeg_quality")) out.jpeg_quality = static_cast<int>(j["jpeg_quality"].get<int64_t>());
  if (j.contains("pixel_format") && j["pixel_format"].is_string())
    out.pixel_format = parse_pixel_format(j["pixel_format"].get<std::string>());
  if (out.manufacturer.empty() && out.model.empty() && out.label.find(" - ") != std::string::npos) {
    const auto sep = out.label.find(" - ");
    out.manufacturer = out.label.substr(0, sep);
    out.model = out.label.substr(sep + 3);
  }
  out.jpeg_quality = std::clamp(out.jpeg_quality, 40, 95);
  return !out.id.empty();
}

std::vector<CameraProfile> load_all_profiles() {
  std::vector<CameraProfile> list;
  const auto dir = profiles_dir();
  if (!std::filesystem::is_directory(dir)) return list;
  for (const auto& ent : std::filesystem::directory_iterator(dir)) {
    if (!ent.is_regular_file()) continue;
    if (ent.path().extension() != ".json") continue;
    CameraProfile cp;
    if (read_profile_file(ent.path(), cp)) list.push_back(std::move(cp));
  }
  std::sort(list.begin(), list.end(), [](const CameraProfile& a, const CameraProfile& b) {
    if (a.interface_type != b.interface_type) return a.interface_type < b.interface_type;
    if (a.manufacturer != b.manufacturer) return a.manufacturer < b.manufacturer;
    return a.model < b.model;
  });
  return list;
}

const CameraProfile* find_profile(const std::vector<CameraProfile>& all, const std::string& id) {
  for (const auto& p : all) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

bool safe_id(const std::string& s) {
  if (s.empty() || s.size() > 64) return false;
  for (unsigned char c : s) {
    if (std::isalnum(c) != 0 || c == '_' || c == '-') continue;
    return false;
  }
  return true;
}

bool icontains(const std::string& hay, const std::string& needle) {
  if (needle.empty()) return true;
  auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
  std::string h = hay;
  std::string n = needle;
  std::transform(h.begin(), h.end(), h.begin(), lower);
  std::transform(n.begin(), n.end(), n.begin(), lower);
  return h.find(n) != std::string::npos;
}

__u32 v4l2_pixfmt(PixelFormat fmt) {
  return fmt == PixelFormat::Yuyv ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_MJPEG;
}

std::string v4l2_node_name(const std::string& dev_path) {
  const auto base = std::filesystem::path(dev_path).filename().string();
  const auto p = std::filesystem::path("/sys/class/video4linux") / base / "name";
  std::ifstream in(p);
  std::string name;
  if (in >> name) return name;
  return {};
}

/** Pi 5 rp1-cfe exposes many /dev/video* nodes; only some accept STREAMON for capture. */
bool v4l2_quick_stream_probe(int fd) {
  v4l2_requestbuffers req{};
  req.count = 1;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1) return false;

  v4l2_buffer buf{};
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = 0;
  if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) return false;

  void* ptr = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
  if (ptr == MAP_FAILED) return false;

  bool ok = false;
  if (ioctl(fd, VIDIOC_QBUF, &buf) == 0) {
    int t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &t) == 0) {
      ioctl(fd, VIDIOC_STREAMOFF, &t);
      ok = true;
    }
  }
  munmap(ptr, buf.length);
  v4l2_requestbuffers rel{};
  rel.count = 0;
  rel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  rel.memory = V4L2_MEMORY_MMAP;
  ioctl(fd, VIDIOC_REQBUFS, &rel);
  return ok;
}

struct DeviceCandidate {
  std::string path;
  std::string card;
  int score{0};
};

bool probe_v4l2_device(const std::string& path, const CameraProfile& prof, DeviceCandidate& out) {
  const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK, 0);
  if (fd < 0) return false;

  v4l2_capability cap{};
  if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
    ::close(fd);
    return false;
  }

  __u32 devcaps = cap.capabilities;
  if (devcaps & V4L2_CAP_DEVICE_CAPS) devcaps = cap.device_caps;
  if (!(devcaps & V4L2_CAP_VIDEO_CAPTURE)) {
    ::close(fd);
    return false;
  }

  const std::string card = reinterpret_cast<const char*>(cap.card);
  const std::string driver = reinterpret_cast<const char*>(cap.driver);
  const std::string node_name = v4l2_node_name(path);
  const auto matches = [&](const std::string& needle) {
    return icontains(node_name, needle) || icontains(card, needle) || icontains(driver, needle);
  };
  if (!prof.device_match.empty() && !matches(prof.device_match)) {
    ::close(fd);
    return false;
  }

  v4l2_format fmt{};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = static_cast<__u32>(prof.width);
  fmt.fmt.pix.height = static_cast<__u32>(prof.height);
  fmt.fmt.pix.pixelformat = v4l2_pixfmt(prof.pixel_format);
  fmt.fmt.pix.field = V4L2_FIELD_NONE;
  if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
    ::close(fd);
    return false;
  }

  if (!v4l2_quick_stream_probe(fd)) {
    ::close(fd);
    return false;
  }

  ::close(fd);
  out.path = path;
  out.card = !node_name.empty() ? node_name : card;
  out.score = 10;
  if (icontains(node_name, "fe_image0") || icontains(card, "fe-image0")) out.score += 50;
  else if (icontains(node_name, "fe_image1") || icontains(card, "fe-image1")) out.score += 40;
  else if (icontains(node_name, "fe_image") || icontains(card, "fe-image")) out.score += 30;
  else if (icontains(node_name, "csi2_ch0") || icontains(card, "csi2-ch0")) out.score += 20;
  if (icontains(driver, "uvcvideo") || icontains(card, "uvc") || icontains(node_name, "uvc")) out.score += 80;
  if (prof.interface_type == "usb") {
    if (icontains(driver, "uvcvideo")) out.score += 40;
    if (icontains(node_name, "rp1") || icontains(node_name, "csi2") || icontains(node_name, "fe_image") ||
        icontains(node_name, "pispbe") || icontains(node_name, "embedded") || icontains(card, "rp1-cfe"))
      out.score -= 200;
  }
  if (prof.interface_type == "mipi") {
    if (icontains(driver, "uvcvideo") || icontains(card, "uc60")) out.score -= 200;
  }
  if (!prof.device_match.empty() && matches(prof.device_match)) out.score += 100;
  return true;
}

std::vector<std::string> list_video_devices() {
  std::vector<std::pair<int, std::string>> numbered;
  for (const auto& ent : std::filesystem::directory_iterator("/dev")) {
    const auto name = ent.path().filename().string();
    if (name.rfind("video", 0) != 0 || name.size() <= 5) continue;
    const std::string num = name.substr(5);
    if (num.empty() || !std::all_of(num.begin(), num.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
      continue;
    try {
      numbered.emplace_back(std::stoi(num), ent.path().string());
    } catch (...) {
    }
  }
  std::sort(numbered.begin(), numbered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<std::string> paths;
  for (const auto& p : numbered) paths.push_back(p.second);
  return paths;
}

struct ResolvedDevice {
  std::string path;
  std::string error;
};

ResolvedDevice resolve_device(const CameraProfile& prof) {
  ResolvedDevice out;
  if (prof.device != "auto") {
    DeviceCandidate cand;
    if (!probe_v4l2_device(prof.device, prof, cand)) {
      out.error = std::string("camera not ready at ") + prof.device +
                  " (wrong device or busy — try another /dev/video* or unplug/replug USB)";
      return out;
    }
    out.path = prof.device;
    return out;
  }

  std::vector<DeviceCandidate> hits;
  for (const auto& path : list_video_devices()) {
    DeviceCandidate cand;
    if (probe_v4l2_device(path, prof, cand)) hits.push_back(std::move(cand));
  }
  if (hits.empty()) {
    out.error = "no V4L2 capture device matched profile (device=auto";
    if (!prof.device_match.empty()) out.error += ", match=" + prof.device_match;
    out.error += "). Run v4l2-ctl --list-devices on the Pi and confirm the camera is connected.";
    return out;
  }
  std::sort(hits.begin(), hits.end(), [](const DeviceCandidate& a, const DeviceCandidate& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.path < b.path;
  });
  out.path = hits.front().path;
  std::cout << "[video] autodetect " << prof.id << " -> " << out.path << " (" << hits.front().card << ")\n"
            << std::flush;
  return out;
}

bool rpicam_camera_ready(std::string& err) {
  if (::access("/host/usr/bin/rpicam-vid", X_OK) != 0) {
    err = "rpicam-vid not found on host (mount / at /host for MIPI profiles)";
    return false;
  }
  FILE* fp = popen("chroot /host /usr/bin/rpicam-hello --list-cameras 2>&1", "r");
  if (!fp) {
    err = "failed to run rpicam-hello";
    return false;
  }
  std::string out;
  char buf[512];
  while (fgets(buf, sizeof(buf), fp)) out += buf;
  const int rc = pclose(fp);
  if (out.find("No cameras available") != std::string::npos) {
    err = "rpicam reports no cameras";
    return false;
  }
  if (out.find("Available cameras") == std::string::npos) {
    err = "rpicam-hello failed (exit " + std::to_string(rc) + ")";
    return false;
  }
  return true;
}

/** Pi MIPI: libcamera pipeline via rpicam-vid stdout (MJPEG). */
class RpicamCapture {
 public:
  static constexpr unsigned char kJpegSoi[] = {0xff, 0xd8};
  static constexpr unsigned char kJpegEoi[] = {0xff, 0xd9};

  RpicamCapture() = default;
  RpicamCapture(const RpicamCapture&) = delete;
  RpicamCapture& operator=(const RpicamCapture&) = delete;
  ~RpicamCapture() { shutdown(); }

  bool start(int want_w, int want_h, int fps, std::string& err) {
    shutdown();
    const int fr = fps > 0 ? fps : 30;
    const std::string cmd = std::string("chroot /host /usr/bin/rpicam-vid -n --codec mjpeg -o - --width ") +
                            std::to_string(want_w) + " --height " + std::to_string(want_h) + " --framerate " +
                            std::to_string(fr) + " -t 0 2>/dev/null";
    fp_ = popen(cmd.c_str(), "r");
    if (!fp_) {
      err = "popen rpicam-vid failed";
      return false;
    }
    return true;
  }

  bool read_jpeg(std::vector<unsigned char>& jpeg, std::string& err) {
    if (!fp_) {
      err = "rpicam not running";
      return false;
    }
    for (int tries = 0; tries < 600; ++tries) {
      if (extract_next_jpeg(carry_, jpeg)) return true;
      char buf[65536];
      const size_t n = fread(buf, 1, sizeof(buf), fp_);
      if (n > 0) {
        carry_.insert(carry_.end(), buf, buf + n);
        continue;
      }
      if (feof(fp_)) {
        err = "rpicam-vid ended";
        return false;
      }
      if (ferror(fp_)) {
        err = "rpicam-vid read error";
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    err = "timeout waiting for MJPEG frame from rpicam-vid";
    return false;
  }

 private:
  static bool extract_next_jpeg(std::vector<unsigned char>& carry, std::vector<unsigned char>& jpeg) {
    auto it = std::search(carry.begin(), carry.end(), std::begin(kJpegSoi), std::end(kJpegSoi));
    if (it == carry.end()) {
      if (carry.size() > 1024 * 1024) carry.clear();
      return false;
    }
    if (it != carry.begin()) carry.erase(carry.begin(), it);
    auto end = std::search(carry.begin() + 2, carry.end(), std::begin(kJpegEoi), std::end(kJpegEoi));
    if (end == carry.end()) return false;
    end += 2;
    jpeg.assign(carry.begin(), end);
    carry.erase(carry.begin(), end);
    return !jpeg.empty();
  }

  void shutdown() {
    if (fp_) {
      pclose(fp_);
      fp_ = nullptr;
    }
    carry_.clear();
  }

  FILE* fp_ = nullptr;
  std::vector<unsigned char> carry_;
};

void yuyv_to_rgb24(const unsigned char* yuyv, int width, int height, std::vector<unsigned char>& rgb) {
  rgb.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 3);
  for (int y = 0; y < height; ++y) {
    const unsigned char* row = yuyv + static_cast<size_t>(y) * static_cast<size_t>(width) * 2;
    unsigned char* out = rgb.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 3;
    for (int x = 0; x < width; x += 2) {
      const int y0 = row[x * 2 + 0];
      const int u = row[x * 2 + 1] - 128;
      const int y1 = row[x * 2 + 2];
      const int v = row[x * 2 + 3] - 128;
      for (int px = 0; px < 2; ++px) {
        const int yy = (px == 0 ? y0 : y1) - 16;
        const int r = (298 * yy + 409 * v + 128) >> 8;
        const int g = (298 * yy - 100 * u - 208 * v + 128) >> 8;
        const int b = (298 * yy + 516 * u + 128) >> 8;
        const size_t o = static_cast<size_t>(x + px) * 3;
        out[o + 0] = static_cast<unsigned char>(std::clamp(r, 0, 255));
        out[o + 1] = static_cast<unsigned char>(std::clamp(g, 0, 255));
        out[o + 2] = static_cast<unsigned char>(std::clamp(b, 0, 255));
      }
    }
  }
}

bool yuyv_to_jpeg(const unsigned char* yuyv, int width, int height, int quality, std::vector<unsigned char>& jpeg,
                  std::string& err) {
  std::vector<unsigned char> rgb;
  yuyv_to_rgb24(yuyv, width, height, rgb);
  tjhandle h = tjInitCompress();
  if (!h) {
    err = "tjInitCompress failed";
    return false;
  }
  unsigned char* jpeg_buf = nullptr;
  unsigned long jpeg_size = 0;
  const int rc = tjCompress2(h, rgb.data(), width, 0, height, TJPF_RGB, &jpeg_buf, &jpeg_size, TJSAMP_444, quality,
                             TJFLAG_FASTDCT);
  tjDestroy(h);
  if (rc != 0) {
    err = std::string("tjCompress2: ") + tjGetErrorStr2(nullptr);
    if (jpeg_buf) tjFree(jpeg_buf);
    return false;
  }
  jpeg.assign(jpeg_buf, jpeg_buf + jpeg_size);
  tjFree(jpeg_buf);
  return !jpeg.empty();
}

/** V4L2 capture; MJPEG passthrough or YUYV encoded to JPEG. */
class V4l2Capture {
 public:
  V4l2Capture() = default;
  V4l2Capture(const V4l2Capture&) = delete;
  V4l2Capture& operator=(const V4l2Capture&) = delete;

  ~V4l2Capture() { shutdown(); }

  bool start(const std::string& path, int want_w, int want_h, PixelFormat fmt, std::string& err) {
    shutdown();
    fmt_ = fmt;
    fd_ = ::open(path.c_str(), O_RDWR | O_NONBLOCK, 0);
    if (fd_ < 0) {
      err = "open " + path + ": " + std::string(std::strerror(errno));
      return false;
    }
    v4l2_capability cap{};
    if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
      err = "VIDIOC_QUERYCAP: " + std::string(std::strerror(errno));
      return false;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
      err = "device does not support video capture";
      return false;
    }

    v4l2_format vfmt{};
    vfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vfmt.fmt.pix.width = static_cast<__u32>(want_w);
    vfmt.fmt.pix.height = static_cast<__u32>(want_h);
    vfmt.fmt.pix.pixelformat = fmt_ == PixelFormat::Yuyv ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_MJPEG;
    vfmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(fd_, VIDIOC_S_FMT, &vfmt) < 0) {
      err = std::string("VIDIOC_S_FMT ") + pixel_format_name(fmt_) + " " + std::to_string(want_w) + "x" +
            std::to_string(want_h) + ": " + std::strerror(errno);
      return false;
    }
    width_ = static_cast<int>(vfmt.fmt.pix.width);
    height_ = static_cast<int>(vfmt.fmt.pix.height);

    v4l2_requestbuffers req{};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
      err = "VIDIOC_REQBUFS: " + std::string(std::strerror(errno));
      return false;
    }
    if (req.count < 2) {
      err = "insufficient mmap buffers";
      return false;
    }

    bufs_.resize(req.count);
    for (unsigned i = 0; i < req.count; ++i) {
      v4l2_buffer buf{};
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = i;
      if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
        err = "VIDIOC_QUERYBUF: " + std::string(std::strerror(errno));
        shutdown();
        return false;
      }
      bufs_[i].len = buf.length;
      bufs_[i].ptr = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
      if (bufs_[i].ptr == MAP_FAILED) {
        err = "mmap: " + std::string(std::strerror(errno));
        shutdown();
        return false;
      }
    }
    for (unsigned i = 0; i < req.count; ++i) {
      v4l2_buffer buf{};
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = i;
      if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        err = "VIDIOC_QBUF: " + std::string(std::strerror(errno));
        shutdown();
        return false;
      }
    }
    int t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_STREAMON, &t) < 0) {
      err = "VIDIOC_STREAMON: " + std::string(std::strerror(errno));
      shutdown();
      return false;
    }
    streaming_ = true;
    return true;
  }

  bool read_jpeg(std::vector<unsigned char>& jpeg, int jpeg_quality, std::string& err) {
    if (!streaming_ || fd_ < 0) {
      err = "not streaming";
      return false;
    }
    for (;;) {
      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(fd_, &fds);
      timeval tv;
      tv.tv_sec = 2;
      tv.tv_usec = 0;
      const int rsel = select(fd_ + 1, &fds, nullptr, nullptr, &tv);
      if (rsel < 0) {
        if (errno == EINTR) continue;
        err = std::string("select: ") + std::strerror(errno);
        return false;
      }
      if (rsel == 0) {
        err = "select timeout";
        return false;
      }
      v4l2_buffer buf{};
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      const int dq = ioctl(fd_, VIDIOC_DQBUF, &buf);
      if (dq < 0) {
        if (errno == EAGAIN) continue;
        err = std::string("VIDIOC_DQBUF: ") + std::strerror(errno);
        return false;
      }
      if (buf.index >= bufs_.size() || !bufs_[buf.index].ptr) {
        err = "bad buffer index";
        return false;
      }
      auto* p = static_cast<const unsigned char*>(bufs_[buf.index].ptr);
      const size_t used = buf.bytesused;
      if (fmt_ == PixelFormat::Mjpeg) {
        jpeg.assign(p, p + used);
      } else {
        if (!yuyv_to_jpeg(p, width_, height_, jpeg_quality, jpeg, err)) {
          if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            err = std::string("VIDIOC_QBUF: ") + std::strerror(errno);
          }
          return false;
        }
      }
      if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        err = std::string("VIDIOC_QBUF: ") + std::strerror(errno);
        return false;
      }
      return !jpeg.empty();
    }
  }

 private:
  struct MmapBuf {
    void* ptr = nullptr;
    size_t len = 0;
  };

  void shutdown() {
    if (fd_ >= 0 && streaming_) {
      int t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      ioctl(fd_, VIDIOC_STREAMOFF, &t);
    }
    streaming_ = false;
    for (auto& b : bufs_) {
      if (b.ptr && b.ptr != MAP_FAILED) {
        munmap(b.ptr, b.len);
        b.ptr = nullptr;
        b.len = 0;
      }
    }
    bufs_.clear();
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    width_ = 0;
    height_ = 0;
  }

  int fd_ = -1;
  bool streaming_ = false;
  PixelFormat fmt_{PixelFormat::Mjpeg};
  int width_ = 0;
  int height_ = 0;
  std::vector<MmapBuf> bufs_;
};

std::mutex g_stream_mutex;

}  // namespace

int main() {
  const std::string host = getenv_or("VIDEO_BIND", "0.0.0.0");
  int port = 8765;
  try {
    port = std::stoi(getenv_or("VIDEO_PORT", "8765"));
  } catch (...) {
  }

  httplib::Server svr;
  {
    const auto n = load_all_profiles().size();
    std::cout << "[video] " << n << " camera profile(s) under " << profiles_dir().string() << "\n" << std::flush;
  }
  svr.new_task_queue = [] {
    const unsigned n = std::thread::hardware_concurrency();
    return new httplib::ThreadPool(n ? n : 4u);
  };

  svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
    json j = {{"status", "ok"}};
    res.set_content(j.dump(), "application/json; charset=utf-8");
  });

  svr.Get("/api/profiles", [](const httplib::Request&, httplib::Response& res) {
    auto all = load_all_profiles();
    json arr = json::array();
    for (const auto& p : all) arr.push_back(profile_to_json(p));
    json out = {{"profiles", arr}};
    res.set_content(out.dump(), "application/json; charset=utf-8");
    res.set_header("Cache-Control", "no-store");
  });

  svr.Get("/api/ready", [](const httplib::Request& req, httplib::Response& res) {
    const std::string default_id = getenv_or("VIDEO_DEFAULT_PROFILE", "uc60");
    std::string pid = req.get_param_value("profile");
    if (pid.empty()) pid = default_id;
    if (!safe_id(pid)) pid = "uc60";
    auto all = load_all_profiles();
    const CameraProfile* prof = find_profile(all, pid);
    if (!prof) {
      json j = {{"ok", false}, {"reason", "unknown profile"}};
      res.status = 404;
      res.set_content(j.dump(), "application/json; charset=utf-8");
      return;
    }
    if (uses_rpicam(*prof)) {
      std::string err;
      if (!rpicam_camera_ready(err)) {
        json j = {{"ok", false}, {"reason", err}};
        res.set_content(j.dump(), "application/json; charset=utf-8");
        return;
      }
      json j = {{"ok", true}, {"device", "rpicam-vid"}, {"profile", profile_to_json(*prof)}};
      res.set_content(j.dump(), "application/json; charset=utf-8");
      res.set_header("Cache-Control", "no-store");
      return;
    }
    const ResolvedDevice dev = resolve_device(*prof);
    if (!dev.error.empty()) {
      json j = {{"ok", false}, {"reason", dev.error}};
      res.set_content(j.dump(), "application/json; charset=utf-8");
      return;
    }
    json j = {{"ok", true}, {"device", dev.path}, {"profile", profile_to_json(*prof)}};
    res.set_content(j.dump(), "application/json; charset=utf-8");
    res.set_header("Cache-Control", "no-store");
  });

  svr.Get("/stream", [](const httplib::Request& req, httplib::Response& res) {
    const std::string default_id = getenv_or("VIDEO_DEFAULT_PROFILE", "uc60");
    std::string pid = req.get_param_value("profile");
    if (pid.empty()) pid = default_id;
    if (!safe_id(pid)) pid = "uc60";

    auto all = load_all_profiles();
    const CameraProfile* prof = find_profile(all, pid);
    if (!prof) {
      res.status = 404;
      res.set_content("{\"error\":\"unknown profile\"}", "application/json; charset=utf-8");
      return;
    }

    const CameraProfile prof_copy = *prof;

    struct StreamSession {
      std::unique_ptr<std::unique_lock<std::mutex>> lock_holder;
      std::unique_ptr<V4l2Capture> v4l2;
      std::unique_ptr<RpicamCapture> rpicam;
      bool use_rpicam{false};
    };
    auto sess = std::make_shared<StreamSession>();
    sess->lock_holder = std::make_unique<std::unique_lock<std::mutex>>(g_stream_mutex, std::defer_lock);
    if (!(*sess->lock_holder).try_lock()) {
      res.status = 503;
      res.set_content("Another client is already using the camera stream.", "text/plain; charset=utf-8");
      return;
    }

    std::string err;
    if (uses_rpicam(prof_copy)) {
      sess->use_rpicam = true;
      sess->rpicam = std::make_unique<RpicamCapture>();
      if (!sess->rpicam->start(prof_copy.width, prof_copy.height, prof_copy.fps, err)) {
        res.status = 503;
        json j = {{"error", err}};
        res.set_content(j.dump(), "application/json; charset=utf-8");
        sess->lock_holder.reset();
        return;
      }
    } else {
      const ResolvedDevice dev = resolve_device(prof_copy);
      if (!dev.error.empty()) {
        res.status = 503;
        json j = {{"error", dev.error}};
        res.set_content(j.dump(), "application/json; charset=utf-8");
        sess->lock_holder.reset();
        return;
      }
      sess->v4l2 = std::make_unique<V4l2Capture>();
      if (!sess->v4l2->start(dev.path, prof_copy.width, prof_copy.height, prof_copy.pixel_format, err)) {
        res.status = 503;
        json j = {{"error", err}};
        res.set_content(j.dump(), "application/json; charset=utf-8");
        sess->lock_holder.reset();
        return;
      }
    }

    const int fps = prof_copy.fps > 0 ? prof_copy.fps : 30;
    const int jpeg_quality = prof_copy.jpeg_quality;
    const auto frame_min = std::chrono::microseconds(1000000 / std::max(1, fps));

    res.set_header("Cache-Control", "no-store, no-cache, must-revalidate");
    res.set_content_provider(
        "multipart/x-mixed-replace; boundary=frame",
        [sess, frame_min, jpeg_quality](size_t /*offset*/, httplib::DataSink& sink) -> bool {
          static const char kHdr[] = "--frame\r\nContent-Type: image/jpeg\r\n\r\n";
          const auto t0 = std::chrono::steady_clock::now();
          std::vector<unsigned char> jpeg;
          std::string e;
          const bool ok = sess->use_rpicam ? sess->rpicam->read_jpeg(jpeg, e) : sess->v4l2->read_jpeg(jpeg, jpeg_quality, e);
          if (!ok) {
            std::cerr << "[video] frame read failed: " << e << "\n" << std::flush;
            return false;
          }
          if (!sink.write(kHdr, sizeof(kHdr) - 1)) return false;
          if (!sink.write(reinterpret_cast<const char*>(jpeg.data()), jpeg.size())) return false;
          if (!sink.write("\r\n", 2)) return false;
          const auto elapsed = std::chrono::steady_clock::now() - t0;
          if (elapsed < frame_min) std::this_thread::sleep_for(frame_min - elapsed);
          return true;
        },
        [sess](bool /*success*/) {
          sess->rpicam.reset();
          sess->v4l2.reset();
          sess->lock_holder.reset();
        });
  });

  std::cout << "[video] listening on http://" << host << ":" << port << " (V4L2 + rpicam-vid)\n" << std::flush;
  if (!svr.listen(host.c_str(), port)) {
    std::cerr << "[video] listen failed\n";
    return 1;
  }
  return 0;
}
