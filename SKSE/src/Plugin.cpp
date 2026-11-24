#include "Delivery.h"
#include "GossipStarter.h"
#include "Papyrus.h"
#include "Quests.h"
#include "Retell.h"
#include "Runtime.h"
#include "Session.h"
#include "SkyrimNet.h"
#include "Text.h"

// Declared here, not by CommonLib's CMake helper, which can't set SKSE's flag for Address Library format 5.
// The struct compatibility value is stored as SKSE's versionIndependenceEx: bit 0 says no version-specific
// structures are used, bit 1 that format 5 is understood. SKSE for 1.7 refuses a plugin without bit 1;
// older SKSEs ignore it.
namespace {
    constexpr std::uint32_t kAddressLibraryFormat5 = 1u << 1;
}

SKSEPluginInfo(
    .Version = REL::Version{RUMOURENGINE_VERSION_MAJOR, RUMOURENGINE_VERSION_MINOR, RUMOURENGINE_VERSION_PATCH, 0},
    .Name = "RumourEngine"sv,
    .Author = "maeyanie"sv,
    .SupportEmail = ""sv,
    .StructCompatibility = static_cast<SKSE::StructCompatibility>(
        static_cast<std::uint32_t>(SKSE::StructCompatibility::Independent) | kAddressLibraryFormat5),
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary,
    .MinimumSKSEVersion = REL::Version{0, 0, 0, 0})

namespace {

    constexpr int kLogsKept = 5;

    /** Keep the last few launches' logs: RumourEngine.log is the current one,
     *  RumourEngine.1.log the previous, up to .5.log as the oldest. As
     *  FactionEngine's. Nothing here throws: a failure to rotate must never
     *  cost us the log. */
    void RotateLogs(const std::filesystem::path& base, int keep) {
        std::error_code ec;
        if (!std::filesystem::exists(base, ec)) return;

        auto numbered = [&](int n) {
            auto p = base;
            p.replace_extension();
            p += "." + std::to_string(n) + ".log";
            return p;
        };

        // rename() with an error_code is a no-op when the source is absent.
        std::filesystem::remove(numbered(keep), ec);
        for (int i = keep - 1; i >= 1; --i) std::filesystem::rename(numbered(i), numbered(i + 1), ec);
        std::filesystem::rename(base, numbered(1), ec);
    }

    void SetupLog() {
        auto dir = RumourEngine::Runtime::LogDirectory();
        if (!dir) return;
        auto path = *dir / "RumourEngine.log";
        try {
            auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
            auto log = std::make_shared<spdlog::logger>("global log", std::move(sink));
            log->set_level(spdlog::level::info);
            log->flush_on(spdlog::level::info);
            spdlog::set_default_logger(std::move(log));
            spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        } catch (...) {
        }
    }

    void OnMessage(SKSE::MessagingInterface::Message* msg) {
        using namespace RumourEngine;
        switch (msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            Text::Init();  // before the plugin scan and anything else that reads names
            if (SkyrimNet::Bind()) Delivery::RegisterDecorators();
            Retell::Install();
            Quests::Install();
            Session::OnDataLoaded();
            GossipStarter::Start();
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            Session::OnPreLoad();
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            if (msg->data) Session::OnGameLoaded(false);
            break;
        case SKSE::MessagingInterface::kNewGame:
            Session::OnGameLoaded(true);
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    // Nothing that looks up a game address may come before Runtime::Prepare.
    // The log doesn't, so a refusal can be logged.
    try {
        if (auto dir = RumourEngine::Runtime::LogDirectory()) RotateLogs(*dir / "RumourEngine.log", kLogsKept);
    } catch (...) {}
    SetupLog();

    const auto game = skse->RuntimeVersion();
    if (auto why = RumourEngine::Runtime::Prepare(game)) {
        logger::error("RumourEngine v{} not loading: {}", RUMOURENGINE_VERSION, *why);
        return false;
    }

    SKSE::Init(skse);
    logger::info("RumourEngine v{} loading on Skyrim {}", RUMOURENGINE_VERSION, game.string("."sv));

    if (auto* messaging = SKSE::GetMessagingInterface()) messaging->RegisterListener(OnMessage);
    if (auto* papyrus = SKSE::GetPapyrusInterface()) papyrus->Register(RumourEngine::Papyrus::Register);
    return true;
}
