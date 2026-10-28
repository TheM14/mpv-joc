#include "config.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>

namespace joc_mpv {
namespace {

std::string trim(const std::string& value) {
    std::size_t first = 0;
    std::size_t last = value.size();
    while (first < last && (value[first] == ' ' || value[first] == '\t')) ++first;
    while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t')) --last;
    return value.substr(first, last - first);
}

std::string unquote(const std::string& value) {
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

bool truthy(const std::string& value) {
    return value == "1" || value == "yes" || value == "true" || value == "on";
}

// The layout names the renderer accepts, in the order its reference lists them.
const char* const kLayouts[] = {"2.0",   "3.1",   "4.0",   "5.1",   "7.1",   "5.1.2",
                                "5.1.4", "7.1.2", "7.1.4", "9.1.4", "9.1.6"};

}  // namespace

bool valid_layout(const std::string& layout) {
    for (const char* name : kLayouts) {
        if (layout == name) return true;
    }
    return false;
}

Config parse_config(const std::string& script_opts, std::string* warnings) {
    Config config;
    std::map<std::string, std::string> options;
    std::size_t start = 0;
    while (start <= script_opts.size()) {
        std::size_t comma = script_opts.find(',', start);
        if (comma == std::string::npos) comma = script_opts.size();
        const std::string item = trim(script_opts.substr(start, comma - start));
        const std::size_t equals = item.find('=');
        if (equals != std::string::npos) {
            options[trim(item.substr(0, equals))] = unquote(trim(item.substr(equals + 1)));
        }
        if (comma == script_opts.size()) break;
        start = comma + 1;
    }

    auto unsupported = [warnings](const std::string& key) {
        if (warnings != nullptr) {
            if (!warnings->empty()) warnings->append("; ");
            warnings->append("unknown option " + key);
        }
    };

    for (const auto& entry : options) {
        const std::string& key = entry.first;
        const std::string& value = entry.second;
        if (key == "joc-enabled") {
            config.enabled = truthy(value);
        } else if (key == "joc-output") {
            if (value == "binaural" || value == "2ch" || value == "0") {
                config.output = 0;
            } else if (value == "speaker" || value == "1") {
                config.output = 1;
            } else {
                unsupported(key + "=" + value);
            }
        } else if (key == "joc-layout") {
            if (valid_layout(value)) {
                config.layout = value;
            } else {
                unsupported(key + "=" + value);
            }
        } else if (key == "joc-hrtf-source") {
            if (value == "sofa") {
                config.hrtf_source = 0;
            } else if (value == "rosella" || value == "personalized") {
                config.hrtf_source = 1;
            } else {
                unsupported(key + "=" + value);
            }
        } else if (key == "joc-hrtf") {
            config.hrtf_file = value;
        } else if (key == "joc-mode") {
            if (value == "near") config.binaural_mode = 1;
            else if (value == "far") config.binaural_mode = 2;
            else if (value == "mid") config.binaural_mode = 3;
            else unsupported(key + "=" + value);
        } else if (key == "joc-gain-db") {
            config.gain_db = std::atof(value.c_str());
        } else if (key == "joc-tail") {
            config.tail_seconds = std::atof(value.c_str());
        } else if (key == "joc-int24") {
            config.force_int24 = truthy(value);
        } else if (key == "joc-object-delay") {
            config.object_delay_samples = static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "joc-threads") {
            config.native_threads = static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "joc-ffmpeg") {
            config.ffmpeg = value;
        } else if (key == "joc-core") {
            config.core_dll = value;
        } else if (key == "joc-wasapi") {
            if (value == "auto" || value == "yes" || value == "no") config.wasapi = value;
            else unsupported(key + "=" + value);
        } else if (key == "joc-exclusive") {
            config.exclusive = truthy(value);
        } else if (key == "joc-verbose") {
            config.verbose = truthy(value);
        } else if (key == "joc-attach") {
            config.attach = truthy(value);
        } else if (key == "joc-log") {
            config.log_path = value;
        } else if (key == "joc-mute-until-ready") {
            config.mute_until_ready = truthy(value);
        } else if (key == "joc-hrtf-cache") {
            if (value == "auto" || value == "memory" || value == "disk" || value == "none") {
                config.hrtf_cache = value;
            } else {
                unsupported(key + "=" + value);
            }
        } else if (key == "joc-hrtf-cache-dir") {
            config.hrtf_cache_dir = value;
        } else if (key == "joc-chunk") {
            const double seconds = std::atof(value.c_str());
            config.chunk_seconds = (seconds >= 0.25 && seconds <= 10.0) ? seconds : 2.0;
        } else {
            unsupported(key);
        }
    }
    return config;
}

}  // namespace joc_mpv
