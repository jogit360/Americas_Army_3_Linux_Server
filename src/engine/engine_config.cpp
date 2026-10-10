#include "engine/engine.h"
#include "util/logging.h"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace aa3 {
namespace engine {

// Trim whitespace from string
static std::string Trim(const std::string& str) {
    auto start = str.begin();
    while (start != str.end() && std::isspace(*start)) {
        start++;
    }

    auto end = str.end();
    do {
        end--;
    } while (std::distance(start, end) > 0 && std::isspace(*end));

    return std::string(start, end + 1);
}

bool Engine::LoadConfig(const std::string& config_path) {
    std::ifstream config_file(config_path);
    if (!config_file.is_open()) {
        LOG_WARN("Config file not found: %s, using defaults", config_path.c_str());
        return true;
    }

    std::string line;
    int line_num = 0;

    while (std::getline(config_file, line)) {
        ++line_num;

        // Skip comments and empty lines
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }

        // Find = separator
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            LOG_WARN("Config line %d: invalid format (no '='): %s", line_num, line.c_str());
            continue;
        }

        std::string key = Trim(line.substr(0, eq_pos));
        std::string value = Trim(line.substr(eq_pos + 1));

        // Remove quotes if present
        if (!value.empty() && (value.front() == '"' || value.front() == '\'')) {
            value = value.substr(1);
        }
        if (!value.empty() && (value.back() == '"' || value.back() == '\'')) {
            value = value.substr(0, value.size() - 1);
        }

        if (!key.empty()) {
            config_values_[key] = value;
            LOG_DEBUG("Config: %s = %s", key.c_str(), value.c_str());
        }
    }

    config_file.close();
    LOG_INFO("Loaded config from: %s", config_path.c_str());
    return true;
}

std::string Engine::GetConfigValue(const std::string& key, const std::string& default_val) {
    auto it = config_values_.find(key);
    if (it != config_values_.end()) {
        return it->second;
    }
    return default_val;
}

int32_t Engine::GetConfigInt(const std::string& key, int32_t default_val) {
    auto it = config_values_.find(key);
    if (it != config_values_.end()) {
        try {
            return std::stoi(it->second);
        } catch (...) {
            LOG_WARN("Failed to parse config int: %s = %s", key.c_str(), it->second.c_str());
        }
    }
    return default_val;
}

double Engine::GetConfigDouble(const std::string& key, double default_val) {
    auto it = config_values_.find(key);
    if (it != config_values_.end()) {
        try {
            return std::stod(it->second);
        } catch (...) {
            LOG_WARN("Failed to parse config double: %s = %s", key.c_str(), it->second.c_str());
        }
    }
    return default_val;
}

} // namespace engine
} // namespace aa3
