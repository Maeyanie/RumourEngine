#include "Delivery.h"
#include "GossipStarter.h"
#include "Papyrus.h"
#include "Quests.h"
#include "Retell.h"
#include "Session.h"
#include "SkyrimNet.h"

namespace {

    constexpr int kLogsKept = 5;

    /** Keep the last few launches' logs: RumourEngine.log is the current one,
     *  RumourEngine.1.log the previous, up to .5.log as the oldest. As
     *  FactionEngine's. Runs before SKSE::Init, in case a CommonLib version
     *  opens and truncates the log in there. Nothing here throws: a failure to
     *  rotate must never cost us the log. */
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
        auto dir = logger::log_directory();
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
    try {
        if (auto dir = logger::log_directory()) RotateLogs(*dir / "RumourEngine.log", kLogsKept);
    } catch (...) {}

    SKSE::Init(skse);
    SetupLog();
    logger::info("RumourEngine v{} loading", RUMOURENGINE_VERSION);

    if (auto* messaging = SKSE::GetMessagingInterface()) messaging->RegisterListener(OnMessage);
    if (auto* papyrus = SKSE::GetPapyrusInterface()) papyrus->Register(RumourEngine::Papyrus::Register);
    return true;
}
