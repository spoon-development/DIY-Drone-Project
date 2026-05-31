#include <httplib.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <ardupilotmega/mavlink.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using json = nlohmann::json;
using clock = std::chrono::steady_clock;

std::string getenv_or(const char* key, const std::string& fallback) {
  const char* v = std::getenv(key);
  return (v && *v) ? std::string(v) : fallback;
}

void trim_inplace(std::string& s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
}

struct TelemetryState {
  std::mutex mu;
  bool connected = false;
  uint8_t system_id = 0;
  uint8_t component_id = 0;
  uint8_t mavlink_version = 0;
  uint8_t autopilot = 0;
  uint8_t base_mode = 0;
  uint32_t custom_mode = 0;
  uint8_t system_status = 0;
  uint16_t load = 0;
  uint16_t voltage_battery = 0;
  int16_t current_battery = 0;
  int8_t battery_remaining = -1;
  int32_t lat = 0;
  int32_t lon = 0;
  int32_t alt = 0;
  uint16_t eph = 0;
  uint8_t fix_type = 0;
  uint8_t satellites_visible = 0;
  float roll = 0.f;
  float pitch = 0.f;
  float yaw = 0.f;
  bool attitude_valid = false;
  std::string attitude_source;
  float airspeed = 0.f;
  float groundspeed = 0.f;
  float alt_msl = 0.f;
  int32_t home_lat = 0;
  int32_t home_lon = 0;
  int32_t home_alt = 0;
  bool home_valid = false;
  int32_t pos_lat = 0;
  int32_t pos_lon = 0;
  int32_t pos_alt = 0;
  int32_t pos_relative_alt = 0;
  uint16_t pos_heading = 65535;
  int16_t pos_vz = 0;
  bool position_valid = false;
  uint16_t wp_distance = 0;
  int16_t target_bearing = 0;
  bool nav_valid = false;
  uint64_t messages_total = 0;
  uint64_t bytes_total = 0;
  std::unordered_map<uint16_t, uint32_t> msg_counts;
  std::unordered_map<std::string, float> sr_params;
  std::chrono::steady_clock::time_point last_packet{};
  std::chrono::steady_clock::time_point last_fc_activity{};
  std::chrono::steady_clock::time_point last_stream_request{};
  uint32_t stream_poll_requests = 0;
  sockaddr_in mav_router_peer{};
  socklen_t mav_router_peer_len = 0;
  bool mav_router_peer_valid = false;
  bool sr_params_applied = false;
};

struct StreamRatesConfig {
  int sr_port = 1;
  float extra1_hz = 10.f;
  float extra2_hz = 5.f;
  float extra3_hz = 5.f;
  float ext_stat_hz = 2.f;
  float raw_sens_hz = 0.f;
  float rc_chan_hz = 0.f;
  float position_hz = 0.f;
};

struct SerialConfig {
  std::string device = "/dev/ttyAMA0";
  int baud = 921600;
};

std::string config_dir() { return getenv_or("TELEMETRY_CONFIG_DIR", "/config"); }

std::string desired_config_path() { return config_dir() + "/desired.json"; }

std::string reload_flag_path() { return config_dir() + "/reload.request"; }

bool valid_device_path(const std::string& s) {
  if (s.rfind("/dev/", 0) != 0 || s.size() < 6 || s.size() > 128) return false;
  for (unsigned char c : s) {
    if (std::isalnum(c) != 0 || c == '/' || c == '_' || c == '-' || c == '.') continue;
    return false;
  }
  return true;
}

bool valid_baud(int baud) {
  static const int kRates[] = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
  for (int r : kRates) {
    if (r == baud) return true;
  }
  return false;
}

SerialConfig default_serial_config() {
  SerialConfig cfg;
  cfg.device = getenv_or("FC_SERIAL_DEVICE", "/dev/ttyAMA0");
  try {
    cfg.baud = std::stoi(getenv_or("FC_SERIAL_BAUD", "921600"));
  } catch (...) {
    cfg.baud = 921600;
  }
  if (!valid_baud(cfg.baud)) cfg.baud = 921600;
  if (!valid_device_path(cfg.device)) cfg.device = "/dev/ttyAMA0";
  return cfg;
}

SerialConfig read_serial_config() {
  SerialConfig cfg = default_serial_config();
  std::ifstream in(desired_config_path());
  if (!in) return cfg;
  try {
    json j;
    in >> j;
    if (j.contains("device") && j["device"].is_string()) {
      const auto d = j["device"].get<std::string>();
      if (valid_device_path(d)) cfg.device = d;
    }
    if (j.contains("baud") && j["baud"].is_number_integer()) {
      const int b = j["baud"].get<int>();
      if (valid_baud(b)) cfg.baud = b;
    }
  } catch (...) {
  }
  return cfg;
}

StreamRatesConfig read_stream_config() {
  StreamRatesConfig cfg;
  try {
    cfg.sr_port = std::stoi(getenv_or("FC_MAVLINK_SR_PORT", "1"));
  } catch (...) {
    cfg.sr_port = 1;
  }
  if (cfg.sr_port < 0 || cfg.sr_port > 6) cfg.sr_port = 1;
  auto f = [](const char* key, const char* fallback) {
    try {
      return std::stof(getenv_or(key, fallback));
    } catch (...) {
      return std::stof(fallback);
    }
  };
  cfg.extra1_hz = f("FC_SR_EXTRA1_HZ", "10");
  cfg.extra2_hz = f("FC_SR_EXTRA2_HZ", "5");
  cfg.extra3_hz = f("FC_SR_EXTRA3_HZ", "5");
  cfg.ext_stat_hz = f("FC_SR_EXT_STAT_HZ", "2");
  cfg.raw_sens_hz = f("FC_SR_RAW_SENS_HZ", "0");
  cfg.rc_chan_hz = f("FC_SR_RC_CHAN_HZ", "0");
  cfg.position_hz = f("FC_SR_POSITION_HZ", "0");
  return cfg;
}

std::string sr_param_name(int sr_port, const char* suffix) {
  return "SR" + std::to_string(sr_port) + "_" + suffix;
}

bool write_serial_config(const SerialConfig& cfg, std::string& err) {
  std::error_code ec;
  std::filesystem::create_directories(config_dir(), ec);
  const auto path = desired_config_path();
  const auto tmp = path + ".tmp";
  json j = {{"device", cfg.device}, {"baud", cfg.baud}, {"updated_at", std::time(nullptr)}};
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      err = "cannot write config";
      return false;
    }
    out << j.dump(2);
    if (!out.flush()) {
      err = "cannot flush config";
      return false;
    }
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    err = ec.message();
    return false;
  }
  {
    std::ofstream flag(reload_flag_path(), std::ios::trunc);
    if (!flag) {
      err = "config saved but reload flag missing";
      return false;
    }
    flag << "1\n";
  }
  return true;
}

const char* autopilot_name(uint8_t ap) {
  switch (ap) {
    case MAV_AUTOPILOT_ARDUPILOTMEGA:
      return "ArduPilot";
    case MAV_AUTOPILOT_PX4:
      return "PX4";
    case MAV_AUTOPILOT_GENERIC:
      return "Generic";
    default:
      return "Unknown";
  }
}

const char* fix_name(uint8_t fix) {
  switch (fix) {
    case 0:
      return "No GPS";
    case 1:
      return "No fix";
    case 2:
      return "2D fix";
    case 3:
      return "3D fix";
    case 4:
      return "DGPS";
    case 5:
      return "RTK float";
    case 6:
      return "RTK fixed";
    default:
      return "Unknown";
  }
}

double haversine_m(double lat1_deg, double lon1_deg, double lat2_deg, double lon2_deg) {
  constexpr double kEarthRadiusM = 6371000.0;
  const auto rad = [](double deg) { return deg * static_cast<double>(M_PI) / 180.0; };
  const double dlat = rad(lat2_deg - lat1_deg);
  const double dlon = rad(lon2_deg - lon1_deg);
  const double a =
      std::sin(dlat / 2.0) * std::sin(dlat / 2.0) +
      std::cos(rad(lat1_deg)) * std::cos(rad(lat2_deg)) * std::sin(dlon / 2.0) * std::sin(dlon / 2.0);
  return kEarthRadiusM * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

json state_to_json(const TelemetryState& st, const SerialConfig& cfg, const StreamRatesConfig& stream_cfg,
                   double fc_age_sec) {
  json j;
  j["connected"] = st.connected;
  j["heartbeat_age_sec"] = fc_age_sec;
  j["router"] = {{"name", "mavlink-router"},
                 {"protocol", "MAVLink2"},
                 {"serial_device", cfg.device},
                 {"serial_baud", cfg.baud},
                 {"udp_listen_port", std::stoi(getenv_or("MAVLINK_UDP_PORT", "14551"))}};
  j["link"] = {{"system_id", st.system_id},
               {"component_id", st.component_id},
               {"mavlink_version", st.mavlink_version},
               {"messages_total", st.messages_total},
               {"bytes_total", st.bytes_total}};
  j["heartbeat"] = {{"autopilot", autopilot_name(st.autopilot)},
                    {"autopilot_id", st.autopilot},
                    {"base_mode", st.base_mode},
                    {"custom_mode", st.custom_mode},
                    {"system_status", st.system_status},
                    {"armed", (st.base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0}};
  j["sys_status"] = {{"load_pct", st.load / 10.0},
                     {"voltage_v", st.voltage_battery > 0 ? st.voltage_battery / 1000.0 : 0.0},
                     {"current_a", st.current_battery != -1 ? st.current_battery / 100.0 : 0.0},
                     {"battery_remaining_pct", st.battery_remaining}};
  j["gps"] = {{"lat", st.lat / 1e7},
              {"lon", st.lon / 1e7},
              {"alt_m", st.alt / 1000.0},
              {"fix_type", st.fix_type},
              {"fix_name", fix_name(st.fix_type)},
              {"eph", st.eph},
              {"satellites", st.satellites_visible}};
  j["attitude"] = {{"roll_deg", st.roll * 180.0 / static_cast<float>(M_PI)},
                   {"pitch_deg", st.pitch * 180.0 / static_cast<float>(M_PI)},
                   {"yaw_deg", st.yaw * 180.0 / static_cast<float>(M_PI)},
                   {"valid", st.attitude_valid},
                   {"source", st.attitude_source.empty() ? json(nullptr) : json(st.attitude_source)}};
  j["vfr_hud"] = {{"airspeed_m_s", st.airspeed}, {"groundspeed_m_s", st.groundspeed}, {"alt_msl_m", st.alt_msl}};
  const double cur_lat = st.position_valid ? st.pos_lat / 1e7 : st.lat / 1e7;
  const double cur_lon = st.position_valid ? st.pos_lon / 1e7 : st.lon / 1e7;
  const double cur_alt_m = st.position_valid ? st.pos_alt / 1000.0 : st.alt / 1000.0;
  const bool pos_ok = st.position_valid || st.fix_type >= 2;
  json dist_home = nullptr;
  if (st.home_valid && pos_ok) {
    dist_home = haversine_m(st.home_lat / 1e7, st.home_lon / 1e7, cur_lat, cur_lon);
  }
  j["home"] = {{"lat", st.home_lat / 1e7},
               {"lon", st.home_lon / 1e7},
               {"alt_m", st.home_alt / 1000.0},
               {"valid", st.home_valid}};
  json position = {{"lat", cur_lat}, {"lon", cur_lon}, {"alt_m", cur_alt_m}, {"valid", pos_ok}};
  if (st.position_valid) position["relative_alt_m"] = st.pos_relative_alt / 1000.0;
  if (st.pos_heading != 65535) position["heading_deg"] = st.pos_heading / 100.0;
  if (st.position_valid) position["climb_m_s"] = -st.pos_vz / 100.0;
  j["position"] = std::move(position);
  j["navigation"] = {{"distance_from_home_m", dist_home},
                     {"wp_distance_m", st.nav_valid ? json(st.wp_distance) : json(nullptr)},
                     {"target_bearing_deg", st.nav_valid ? json(st.target_bearing / 100.0) : json(nullptr)},
                     {"valid", st.nav_valid || dist_home.is_number()}};
  json sr = json::object();
  for (const auto& [name, hz] : st.sr_params) sr[name] = hz;
  j["stream_rates"] = {{"sr_port", stream_cfg.sr_port},
                       {"params", std::move(sr)},
                       {"poll_requests", st.stream_poll_requests},
                       {"waiting_attitude", st.connected && !st.attitude_valid}};
  json counts = json::object();
  for (const auto& [id, n] : st.msg_counts) counts[std::to_string(id)] = n;
  j["msg_counts"] = std::move(counts);
  return j;
}

void set_attitude(TelemetryState& st, float roll, float pitch, float yaw, const char* source) {
  st.roll = roll;
  st.pitch = pitch;
  st.yaw = yaw;
  st.attitude_valid = true;
  st.attitude_source = source;
}

void quat_to_euler(float w, float x, float y, float z, float& roll, float& pitch, float& yaw) {
  const float sinr_cosp = 2.f * (w * x + y * z);
  const float cosr_cosp = 1.f - 2.f * (x * x + y * y);
  roll = std::atan2(sinr_cosp, cosr_cosp);
  const float sinp = 2.f * (w * y - z * x);
  pitch = std::abs(sinp) >= 1.f ? std::copysign(static_cast<float>(M_PI / 2.0), sinp) : std::asin(sinp);
  const float siny_cosp = 2.f * (w * z + x * y);
  const float cosy_cosp = 1.f - 2.f * (y * y + z * z);
  yaw = std::atan2(siny_cosp, cosy_cosp);
}

bool is_fc_source(uint8_t sysid) { return sysid > 0 && sysid < 250; }

bool is_fc_heartbeat(uint8_t sysid, const mavlink_heartbeat_t& hb) {
  if (!is_fc_source(sysid)) return false;
  if (hb.autopilot == MAV_AUTOPILOT_ARDUPILOTMEGA || hb.autopilot == MAV_AUTOPILOT_PX4) return true;
  if (hb.type == MAV_TYPE_GCS || hb.type == MAV_TYPE_ONBOARD_CONTROLLER) return false;
  return hb.autopilot != MAV_AUTOPILOT_INVALID;
}

std::string param_id_string(const char* raw, size_t len = MAVLINK_MSG_PARAM_VALUE_FIELD_PARAM_ID_LEN) {
  std::string s(raw, len);
  const auto nul = s.find('\0');
  if (nul != std::string::npos) s.resize(nul);
  return s;
}

bool send_mavlink_udp(int fd, const sockaddr_in& dest, socklen_t dest_len, const mavlink_message_t& m) {
  uint8_t buf[MAVLINK_MAX_PACKET_LEN];
  const uint16_t len = mavlink_msg_to_send_buffer(buf, &m);
  return sendto(fd, buf, len, 0, reinterpret_cast<const sockaddr*>(&dest), dest_len) >= 0;
}

bool send_param_set(int fd, const sockaddr_in& dest, socklen_t dest_len, uint8_t target_sys, uint8_t target_comp,
                    const std::string& name, float value) {
  if (name.size() > MAVLINK_MSG_PARAM_SET_FIELD_PARAM_ID_LEN) return false;
  mavlink_message_t m{};
  mavlink_param_set_t p{};
  p.target_system = target_sys;
  p.target_component = target_comp;
  std::memset(p.param_id, 0, sizeof(p.param_id));
  std::memcpy(p.param_id, name.c_str(), name.size());
  p.param_value = value;
  p.param_type = MAV_PARAM_TYPE_REAL32;
  mavlink_msg_param_set_encode(255, MAV_COMP_ID_MISSIONPLANNER, &m, &p);
  return send_mavlink_udp(fd, dest, dest_len, m);
}

bool send_param_request_read(int fd, const sockaddr_in& dest, socklen_t dest_len, uint8_t target_sys,
                             uint8_t target_comp, const std::string& name) {
  if (name.size() > MAVLINK_MSG_PARAM_REQUEST_READ_FIELD_PARAM_ID_LEN) return false;
  mavlink_message_t m{};
  mavlink_param_request_read_t req{};
  req.target_system = target_sys;
  req.target_component = target_comp;
  req.param_index = -1;
  std::memset(req.param_id, 0, sizeof(req.param_id));
  std::memcpy(req.param_id, name.c_str(), name.size());
  mavlink_msg_param_request_read_encode(255, MAV_COMP_ID_MISSIONPLANNER, &m, &req);
  return send_mavlink_udp(fd, dest, dest_len, m);
}

bool send_set_message_interval(int fd, const sockaddr_in& dest, socklen_t dest_len, uint8_t target_sys,
                               uint8_t target_comp, uint32_t msg_id, float interval_us) {
  mavlink_message_t m{};
  mavlink_command_long_t cmd{};
  cmd.target_system = target_sys;
  cmd.target_component = target_comp;
  cmd.command = MAV_CMD_SET_MESSAGE_INTERVAL;
  cmd.param1 = static_cast<float>(msg_id);
  cmd.param2 = interval_us;
  mavlink_msg_command_long_encode(255, MAV_COMP_ID_MISSIONPLANNER, &m, &cmd);
  return send_mavlink_udp(fd, dest, dest_len, m);
}

void apply_sr_params_once(int fd, const sockaddr_in& dest, socklen_t dest_len, uint8_t sys, uint8_t comp,
                          const StreamRatesConfig& cfg) {
  struct ParamRate {
    const char* suffix;
    float hz;
  };
  const ParamRate params[] = {
      {"EXTRA1", cfg.extra1_hz},   {"EXTRA2", cfg.extra2_hz},   {"EXTRA3", cfg.extra3_hz},
      {"EXT_STAT", cfg.ext_stat_hz}, {"RAW_SENS", cfg.raw_sens_hz}, {"RC_CHAN", cfg.rc_chan_hz},
      {"POSITION", cfg.position_hz},
  };
  for (const auto& p : params) {
    send_param_set(fd, dest, dest_len, sys, comp, sr_param_name(cfg.sr_port, p.suffix), p.hz);
  }
}

void poll_refresh_streams(int fd, const sockaddr_in& dest, socklen_t dest_len, uint8_t sys, uint8_t comp,
                          const StreamRatesConfig& cfg, bool apply_params) {
  const uint8_t target_comp = comp ? comp : MAV_COMP_ID_AUTOPILOT1;

  if (apply_params) {
    apply_sr_params_once(fd, dest, dest_len, sys, target_comp, cfg);
  }

  const char* suffixes[] = {"EXTRA1", "EXTRA2", "EXTRA3", "EXT_STAT", "RAW_SENS", "RC_CHAN", "POSITION"};
  for (const char* suffix : suffixes) {
    send_param_request_read(fd, dest, dest_len, sys, target_comp, sr_param_name(cfg.sr_port, suffix));
  }

  send_set_message_interval(fd, dest, dest_len, sys, target_comp, MAVLINK_MSG_ID_ATTITUDE, 100000.f);
  send_set_message_interval(fd, dest, dest_len, sys, target_comp, MAVLINK_MSG_ID_AHRS2, 100000.f);
  send_set_message_interval(fd, dest, dest_len, sys, target_comp, MAVLINK_MSG_ID_SYS_STATUS, 500000.f);
  send_set_message_interval(fd, dest, dest_len, sys, target_comp, MAVLINK_MSG_ID_VFR_HUD, 500000.f);
  send_set_message_interval(fd, dest, dest_len, sys, target_comp, MAVLINK_MSG_ID_GLOBAL_POSITION_INT, 500000.f);
  send_set_message_interval(fd, dest, dest_len, sys, target_comp, MAVLINK_MSG_ID_HOME_POSITION, 1000000.f);
  send_set_message_interval(fd, dest, dest_len, sys, target_comp, MAVLINK_MSG_ID_NAV_CONTROLLER_OUTPUT, 500000.f);
}

void note_fc_activity(TelemetryState& st, const mavlink_message_t& msg, const clock::time_point& now) {
  if (!is_fc_source(msg.sysid)) return;
  if (st.system_id != 0 && st.system_id != msg.sysid) {
    st.sr_params_applied = false;
    st.sr_params.clear();
  }
  st.system_id = msg.sysid;
  st.component_id = msg.compid;
  st.last_fc_activity = now;
  st.connected = true;
}

void apply_message(TelemetryState& st, const mavlink_message_t& msg, const clock::time_point& now) {
  st.messages_total++;
  st.msg_counts[msg.msgid]++;
  st.last_packet = now;
  switch (msg.msgid) {
    case MAVLINK_MSG_ID_HEARTBEAT: {
      mavlink_heartbeat_t hb{};
      mavlink_msg_heartbeat_decode(&msg, &hb);
      if (!is_fc_heartbeat(msg.sysid, hb)) break;
      note_fc_activity(st, msg, now);
      st.mavlink_version = hb.mavlink_version;
      st.autopilot = hb.autopilot;
      st.base_mode = hb.base_mode;
      st.custom_mode = hb.custom_mode;
      st.system_status = hb.system_status;
      break;
    }
    case MAVLINK_MSG_ID_SYS_STATUS: {
      note_fc_activity(st, msg, now);
      mavlink_sys_status_t ss{};
      mavlink_msg_sys_status_decode(&msg, &ss);
      st.load = ss.load;
      st.voltage_battery = ss.voltage_battery;
      st.current_battery = ss.current_battery;
      st.battery_remaining = ss.battery_remaining;
      break;
    }
    case MAVLINK_MSG_ID_GPS_RAW_INT: {
      note_fc_activity(st, msg, now);
      mavlink_gps_raw_int_t gps{};
      mavlink_msg_gps_raw_int_decode(&msg, &gps);
      st.lat = gps.lat;
      st.lon = gps.lon;
      st.alt = gps.alt;
      st.eph = gps.eph;
      st.fix_type = gps.fix_type;
      st.satellites_visible = gps.satellites_visible;
      break;
    }
    case MAVLINK_MSG_ID_ATTITUDE: {
      note_fc_activity(st, msg, now);
      mavlink_attitude_t att{};
      mavlink_msg_attitude_decode(&msg, &att);
      set_attitude(st, att.roll, att.pitch, att.yaw, "ATTITUDE");
      break;
    }
    case MAVLINK_MSG_ID_AHRS2: {
      note_fc_activity(st, msg, now);
      mavlink_ahrs2_t ahrs2{};
      mavlink_msg_ahrs2_decode(&msg, &ahrs2);
      set_attitude(st, ahrs2.roll, ahrs2.pitch, ahrs2.yaw, "AHRS2");
      break;
    }
    case MAVLINK_MSG_ID_ATTITUDE_QUATERNION: {
      note_fc_activity(st, msg, now);
      mavlink_attitude_quaternion_t aq{};
      mavlink_msg_attitude_quaternion_decode(&msg, &aq);
      float roll = 0.f, pitch = 0.f, yaw = 0.f;
      quat_to_euler(aq.q1, aq.q2, aq.q3, aq.q4, roll, pitch, yaw);
      set_attitude(st, roll, pitch, yaw, "ATTITUDE_QUATERNION");
      break;
    }
    case MAVLINK_MSG_ID_PARAM_VALUE: {
      if (!is_fc_source(msg.sysid)) break;
      note_fc_activity(st, msg, now);
      mavlink_param_value_t pv{};
      mavlink_msg_param_value_decode(&msg, &pv);
      const std::string name = param_id_string(pv.param_id);
      if (name.rfind("SR", 0) == 0) st.sr_params[name] = pv.param_value;
      break;
    }
    case MAVLINK_MSG_ID_BATTERY_STATUS: {
      mavlink_battery_status_t bat{};
      mavlink_msg_battery_status_decode(&msg, &bat);
      if (bat.voltages[0] != UINT16_MAX && bat.voltages[0] > 0) {
        st.voltage_battery = bat.voltages[0];
      }
      if (bat.current_battery != -1) st.current_battery = bat.current_battery;
      if (bat.battery_remaining != UINT8_MAX) st.battery_remaining = static_cast<int8_t>(bat.battery_remaining);
      break;
    }
    case MAVLINK_MSG_ID_VFR_HUD: {
      note_fc_activity(st, msg, now);
      mavlink_vfr_hud_t hud{};
      mavlink_msg_vfr_hud_decode(&msg, &hud);
      st.airspeed = hud.airspeed;
      st.groundspeed = hud.groundspeed;
      st.alt_msl = hud.alt;
      break;
    }
    case MAVLINK_MSG_ID_HOME_POSITION: {
      note_fc_activity(st, msg, now);
      mavlink_home_position_t home{};
      mavlink_msg_home_position_decode(&msg, &home);
      st.home_lat = home.latitude;
      st.home_lon = home.longitude;
      st.home_alt = home.altitude;
      st.home_valid = true;
      break;
    }
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
      note_fc_activity(st, msg, now);
      mavlink_global_position_int_t pos{};
      mavlink_msg_global_position_int_decode(&msg, &pos);
      st.pos_lat = pos.lat;
      st.pos_lon = pos.lon;
      st.pos_alt = pos.alt;
      st.pos_relative_alt = pos.relative_alt;
      st.pos_heading = pos.hdg;
      st.pos_vz = pos.vz;
      st.position_valid = true;
      break;
    }
    case MAVLINK_MSG_ID_NAV_CONTROLLER_OUTPUT: {
      note_fc_activity(st, msg, now);
      mavlink_nav_controller_output_t nav{};
      mavlink_msg_nav_controller_output_decode(&msg, &nav);
      st.wp_distance = nav.wp_dist;
      st.target_bearing = nav.target_bearing;
      st.nav_valid = true;
      break;
    }
    default:
      break;
  }
}

void mavlink_udp_loop(std::shared_ptr<TelemetryState> state, std::shared_ptr<std::atomic<bool>> poll_refresh,
                      std::atomic<bool>& stop) {
  const StreamRatesConfig stream_cfg = read_stream_config();
  const bool apply_sr_on_connect = getenv_or("FC_SR_APPLY_ON_CONNECT", "1") != "0";
  const int port = [&] {
    try {
      return std::stoi(getenv_or("MAVLINK_UDP_PORT", "14551"));
    } catch (...) {
      return 14551;
    }
  }();
  const int timeout_sec = [&] {
    try {
      return std::stoi(getenv_or("MAVLINK_HEARTBEAT_TIMEOUT_SEC", "8"));
    } catch (...) {
      return 8;
    }
  }();

  int rx_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (rx_fd < 0) {
    std::cerr << "[telemetry] socket failed: " << std::strerror(errno) << "\n";
    return;
  }
  int reuse = 1;
  setsockopt(rx_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (bind(rx_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::cerr << "[telemetry] bind udp :" << port << " failed: " << std::strerror(errno) << "\n";
    close(rx_fd);
    return;
  }

  std::cout << "[telemetry] listening for mavlink-router UDP on :" << port << " (SR" << stream_cfg.sr_port
            << " params refreshed on HMI poll)\n"
            << std::flush;

  uint8_t buf[2048];
  mavlink_message_t msg{};
  mavlink_status_t status{};

  while (!stop.load()) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(rx_fd, &rfds);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    const int sel = select(rx_fd + 1, &rfds, nullptr, nullptr, &tv);
    const auto now = clock::now();

    if (sel > 0 && FD_ISSET(rx_fd, &rfds)) {
      sockaddr_in src{};
      socklen_t src_len = sizeof(src);
      const ssize_t n = recvfrom(rx_fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&src), &src_len);
      if (n > 0) {
        std::lock_guard<std::mutex> lk(state->mu);
        state->mav_router_peer = src;
        state->mav_router_peer_len = src_len;
        state->mav_router_peer_valid = true;
        state->bytes_total += static_cast<uint64_t>(n);
        for (ssize_t i = 0; i < n; ++i) {
          if (mavlink_parse_char(MAVLINK_COMM_0, buf[i], &msg, &status)) {
            apply_message(*state, msg, now);
          }
        }
      }
    }

    bool do_poll = poll_refresh->exchange(false);
    sockaddr_in peer{};
    socklen_t peer_len = 0;
    bool peer_ok = false;
    bool connected = false;
    uint8_t sys = 0;
    uint8_t comp = 0;
    bool apply_sr = false;
    {
      std::lock_guard<std::mutex> lk(state->mu);
      if (state->connected) {
        const auto age =
            std::chrono::duration_cast<std::chrono::seconds>(now - state->last_fc_activity).count();
        if (age > timeout_sec) {
          state->connected = false;
          state->attitude_valid = false;
          state->attitude_source.clear();
          state->sr_params_applied = false;
        }
      }
      connected = state->connected;
      peer_ok = state->mav_router_peer_valid;
      if (peer_ok) {
        peer = state->mav_router_peer;
        peer_len = state->mav_router_peer_len;
      }
      if (do_poll && connected && peer_ok && state->system_id != 0) {
        sys = state->system_id;
        comp = state->component_id;
        apply_sr = apply_sr_on_connect && !state->sr_params_applied;
      } else {
        do_poll = false;
      }
    }

    if (do_poll) {
      poll_refresh_streams(rx_fd, peer, peer_len, sys, comp, stream_cfg, apply_sr);
      std::lock_guard<std::mutex> lk(state->mu);
      if (state->system_id == sys) {
        state->last_stream_request = clock::now();
        state->stream_poll_requests++;
        if (apply_sr) state->sr_params_applied = true;
      }
    }
  }

  close(rx_fd);
}

}  // namespace

int main() {
  const std::string host = getenv_or("TELEMETRY_BIND", "0.0.0.0");
  int port = 9102;
  try {
    port = std::stoi(getenv_or("TELEMETRY_PORT", "9102"));
  } catch (...) {
  }

  auto state = std::make_shared<TelemetryState>();
  auto poll_refresh = std::make_shared<std::atomic<bool>>(false);
  std::atomic<bool> stop{false};
  std::thread rx_thread(mavlink_udp_loop, state, poll_refresh, std::ref(stop));

  httplib::Server svr;
  svr.new_task_queue = [] {
    const unsigned n = std::thread::hardware_concurrency();
    return new httplib::ThreadPool(n ? n : 4u);
  };

  svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("{\"status\":\"ok\"}\n", "application/json; charset=utf-8");
  });

  svr.Get("/api/config", [](const httplib::Request&, httplib::Response& res) {
    const SerialConfig cfg = read_serial_config();
    json j = {{"device", cfg.device},
              {"baud", cfg.baud},
              {"valid_bauds", {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600}},
              {"router", "mavlink-router"},
              {"protocol", "MAVLink2"}};
    res.set_content(j.dump(), "application/json; charset=utf-8");
    res.set_header("Cache-Control", "no-store");
  });

  svr.Post("/api/config", [](const httplib::Request& req, httplib::Response& res) {
    json incoming;
    try {
      incoming = json::parse(req.body);
    } catch (...) {
      res.status = 400;
      res.set_content(R"({"ok":false,"error":"invalid JSON"})", "application/json; charset=utf-8");
      return;
    }
    SerialConfig cfg = read_serial_config();
    if (incoming.contains("device") && incoming["device"].is_string()) {
      const auto d = incoming["device"].get<std::string>();
      if (!valid_device_path(d)) {
        res.status = 400;
        res.set_content(R"({"ok":false,"error":"invalid serial device path"})", "application/json; charset=utf-8");
        return;
      }
      cfg.device = d;
    }
    if (incoming.contains("baud")) {
      if (!incoming["baud"].is_number_integer()) {
        res.status = 400;
        res.set_content(R"({"ok":false,"error":"baud must be an integer"})", "application/json; charset=utf-8");
        return;
      }
      const int b = incoming["baud"].get<int>();
      if (!valid_baud(b)) {
        res.status = 400;
        res.set_content(R"({"ok":false,"error":"unsupported baud rate"})", "application/json; charset=utf-8");
        return;
      }
      cfg.baud = b;
    }
    std::string err;
    if (!write_serial_config(cfg, err)) {
      res.status = 500;
      json j = {{"ok", false}, {"error", err}};
      res.set_content(j.dump(), "application/json; charset=utf-8");
      return;
    }
    json j = {{"ok", true},
              {"device", cfg.device},
              {"baud", cfg.baud},
              {"reload", "mavlink-router restarting serial link"}};
    res.set_content(j.dump(), "application/json; charset=utf-8");
  });

  svr.Get("/api/telemetry", [state, poll_refresh](const httplib::Request&, httplib::Response& res) {
    poll_refresh->store(true);
    const SerialConfig cfg = read_serial_config();
    const StreamRatesConfig stream_cfg = read_stream_config();
    const auto now = clock::now();
    json j;
    {
      std::lock_guard<std::mutex> lk(state->mu);
      double fc_age = -1.0;
      if (state->connected) {
        fc_age = std::chrono::duration<double>(now - state->last_fc_activity).count();
      }
      j = state_to_json(*state, cfg, stream_cfg, fc_age);
    }
    res.set_content(j.dump(), "application/json; charset=utf-8");
    res.set_header("Cache-Control", "no-store");
  });

  std::cout << "[telemetry] listening on http://" << host << ":" << port << "\n" << std::flush;
  const bool ok = svr.listen(host.c_str(), port);
  stop = true;
  if (rx_thread.joinable()) rx_thread.join();
  return ok ? 0 : 1;
}
