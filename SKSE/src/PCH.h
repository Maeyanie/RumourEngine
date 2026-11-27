#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define RUMOURENGINE_VERSION "1.2.0"

using namespace std::literals;

namespace logger = SKSE::log;

namespace RumourEngine {
    // The public API's version. Version 1 was frozen on 2026-11-12 (tag api-v1):
    // later changes only add to it.
    inline constexpr int kApiVersion = 1;
}
