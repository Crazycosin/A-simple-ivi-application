#include "model/AppConfig.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace aa {

namespace {

std::string trim(const std::string& s) {
    static const char* kWs = " \t\r\n";
    const auto b = s.find_first_not_of(kWs);
    if (b == std::string::npos) {
        return {};
    }
    const auto e = s.find_last_not_of(kWs);
    return s.substr(b, e - b + 1);
}

std::string toLower(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

bool parseU32(const std::string& v, std::uint32_t& out) {
    if (v.empty()) {
        return false;
    }
    char* end = nullptr;
    const unsigned long r = std::strtoul(v.c_str(), &end, 0);
    if (end == v.c_str() || *end != '\0' || r > 0xFFFFFFFFul) {
        return false;
    }
    out = static_cast<std::uint32_t>(r);
    return true;
}

bool parseI32(const std::string& v, std::int32_t& out) {
    if (v.empty()) {
        return false;
    }
    char* end = nullptr;
    const long r = std::strtol(v.c_str(), &end, 0);
    if (end == v.c_str() || *end != '\0' ||
        r < -2147483648L || r > 2147483647L) {
        return false;
    }
    out = static_cast<std::int32_t>(r);
    return true;
}

bool parseBool(const std::string& v, bool& out) {
    const std::string l = toLower(v);
    if (l == "1" || l == "true" || l == "yes" || l == "on") {
        out = true;
        return true;
    }
    if (l == "0" || l == "false" || l == "no" || l == "off") {
        out = false;
        return true;
    }
    return false;
}

/// 分发单个键值到 Config；未知键返回 false
bool apply(const std::string& section,
           const std::string& key,
           const std::string& val,
           Config& c) {
    const auto setU16 = [&val](std::uint16_t& dst) {
        std::uint32_t t = 0;
        if (parseU32(val, t) && t <= 0xFFFFu) {
            dst = static_cast<std::uint16_t>(t);
            return true;
        }
        return false;
    };
    const auto setU32 = [&val](std::uint32_t& dst) {
        std::uint32_t t = 0;
        if (parseU32(val, t)) {
            dst = t;
            return true;
        }
        return false;
    };

    if (section == "app") {
        if (key == "surface_id") return setU32(c.surfaceId);
        if (key == "log_path")   { c.logPath = val; return true; }
        return false;
    }
    if (section == "window") {
        if (key == "title") {
            c.title = val;
            return true;
        }
        if (key == "default_width")  return parseI32(val, c.defaultWidth);
        if (key == "default_height") return parseI32(val, c.defaultHeight);
        if (key == "logo_path")     { c.logoPath = val; return true; }
        return false;
    }
    if (section == "player") {
        if (key == "enabled")           return parseBool(val, c.player.enabled);
        if (key == "audio_enable")      return parseBool(val, c.player.audioEnable);
        if (key == "video_uri") { c.player.videoUri = val; return true; }
        if (key == "audio_uri") { c.player.audioUri = val; return true; }
        if (key == "loop")              return parseBool(val, c.player.loop);
        if (key == "fullscreen_layout") return parseBool(val, c.player.fullscreenLayout);
        if (key == "restore_layout")    return parseBool(val, c.player.restoreLayout);
        if (key == "latency_trace")     return parseBool(val, c.player.latencyTrace);
        if (key == "latency_interval")  return setU32(c.player.latencyInterval);
        if (key == "drift_interval_sec") return setU32(c.player.driftIntervalSec);
        if (key == "queue_max_ms")      return setU32(c.player.queueMaxMs);
        if (key == "stall_threshold_ms") return setU32(c.player.stallThresholdMs);
        if (key == "stall_detect")      return parseBool(val, c.player.stallDetect);
        if (key == "render_path") {
            const std::string l = toLower(val);
            if (l == "drm")     { c.player.drmRender = true; return true; }
            if (l == "shm")     { c.player.drmRender = false; return true; }
            return false;
        }
        if (key == "drm_device") { c.player.drmDevice = val; return true; }
        if (key == "drm_plane_id") return setU32(c.player.drmPlaneId);
        if (key == "drm_viewport") {
            // "x,y,w,h"
            std::vector<std::string> parts;
            std::size_t start = 0;
            while (true) {
                const auto comma = val.find(',', start);
                parts.push_back(trim(val.substr(start, comma - start)));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            std::int32_t v[4] = {0, 0, 0, 0};
            if (parts.size() != 4) return false;
            for (int i = 0; i < 4; ++i) {
                if (!parseI32(parts[i], v[i])) return false;
            }
            if (v[2] <= 0 || v[3] <= 0) return false;
            c.player.drmX = v[0]; c.player.drmY = v[1];
            c.player.drmW = v[2]; c.player.drmH = v[3];
            return true;
        }
        if (key == "evdev_screen_w") return parseI32(val, c.player.evdevScreenW);
        if (key == "evdev_screen_h") return parseI32(val, c.player.evdevScreenH);
        if (key == "drm_modifier") {
            if (val.empty()) return false;
            char* end = nullptr;
            const unsigned long long r = std::strtoull(val.c_str(), &end, 0);
            if (end == val.c_str() || *end != '\0') return false;
            c.player.drmModifier = r;
            return true;
        }
        if (key == "transport") {
            const std::string l = toLower(val);
            if (l == "usb")      { c.player.transport = Transport::Usb; return true; }
            if (l == "wifi")     { c.player.transport = Transport::Wifi; return true; }
            return false;
        }
        return false;
    }
    if (section == "ui") {
        if (key == "title_bar_height")    return setU16(c.ui.titleBarHeight);
        if (key == "close_button_size")   return setU16(c.ui.closeButtonSize);
        if (key == "close_button_margin")  return setU16(c.ui.closeButtonMargin);
        if (key == "video_margin")         return setU32(c.ui.videoMargin);
        if (key == "video_border_width")   return setU16(c.ui.videoBorderWidth);
        if (key == "video_border_radius")  return setU16(c.ui.videoBorderRadius);
        if (key == "video_aspect_w")       return setU16(c.ui.videoAspectW);
        if (key == "video_aspect_h")       return setU16(c.ui.videoAspectH);
        if (key == "font_size")            return setU16(c.ui.fontSize);
        if (key == "color_background")      return setU32(c.ui.colorBackground);
        if (key == "color_title_bar")      return setU32(c.ui.colorTitleBar);
        if (key == "color_border")         return setU32(c.ui.colorBorder);
        if (key == "color_video_fill")      return setU32(c.ui.colorVideoFill);
        if (key == "color_close_glyph")     return setU32(c.ui.colorCloseGlyph);
        if (key == "color_text")           return setU32(c.ui.colorText);
        if (key == "placeholder_text")     { c.ui.placeholderText = val; return true; }
        return false;
    }
    return false;
}

} // namespace

bool AppConfig::load(const std::string& exeDir,
                     Config& out,
                     std::string& iniPathOut,
                     std::size_t& errCount) {
    iniPathOut.clear();
    errCount = 0;

    const std::vector<std::string> candidates = {
        exeDir + "/androidauto.ini",
        exeDir + "/../config/androidauto.ini",
        "./androidauto.ini",
    };

    std::string ini;
    for (const auto& c : candidates) {
        if (std::FILE* f = std::fopen(c.c_str(), "r")) {
            std::fclose(f);
            ini = c;
            break;
        }
    }
    if (ini.empty()) {
        return false;
    }

    std::FILE* f = std::fopen(ini.c_str(), "r");
    if (f == nullptr) {
        return false;
    }

    std::string section;
    char linebuf[512];
    while (std::fgets(linebuf, sizeof linebuf, f) != nullptr) {
        std::string line = trim(linebuf);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = toLower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            ++errCount;
            continue;
        }
        const std::string key = toLower(trim(line.substr(0, eq)));
        const std::string val = trim(line.substr(eq + 1));
        if (key.empty() || !apply(section, key, val, out)) {
            ++errCount;
        }
    }
    std::fclose(f);
    iniPathOut = ini;

    // 资源相对路径 → 基于 ini 所在目录的绝对路径
    if (!out.logoPath.empty() && out.logoPath[0] != '/') {
        std::string dir = ini;
        const auto slash = dir.find_last_of('/');
        dir = (slash == std::string::npos)
                  ? std::string(".")
                  : dir.substr(0, slash == 0 ? 1 : slash);
        out.logoPath = dir + "/" + out.logoPath;
    }

    // 播放源：绝对路径规范化为 file:// URI（已带 scheme 的原样保留）
    const auto toUri = [](std::string& uri) {
        if (!uri.empty() && uri[0] == '/' &&
            uri.compare(0, 7, "file://") != 0) {
            uri = "file://" + uri;
        }
    };
    toUri(out.player.videoUri);
    toUri(out.player.audioUri);
    if (out.player.audioUri.empty()) {
        out.player.audioUri = out.player.videoUri;
    }
    return true;
}

} // namespace aa
