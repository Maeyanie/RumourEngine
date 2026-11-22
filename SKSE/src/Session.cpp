#include "Session.h"
#include "Api.h"
#include "Database.h"
#include "Gossip.h"
#include "GossipStarter.h"
#include "Harvest.h"
#include "Mills.h"
#include "Placements.h"
#include "Promote.h"
#include "Quests.h"
#include "Rumours.h"
#include "PluginScan.h"
#include "Retell.h"
#include "Settings.h"
#include "SkyrimNet.h"
#include "Spread.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <thread>

namespace RumourEngine::Session {

    namespace {

        std::atomic<bool>     g_ready = false;
        std::atomic<bool>     g_gameLoaded = false;
        std::atomic<unsigned> g_generation = 0;  // bumped on every load; stale workers stop

        // Held to bump the generation, and by a worker to act on the database,
        // so a worker from an earlier load can never act after a later one starts.
        std::mutex g_sessionLock;

        /** Run f under the session lock if `gen` is still the current load. */
        template <class F>
        bool Current(unsigned gen, F&& f) {
            std::lock_guard lk(g_sessionLock);
            if (g_generation.load() != gen) return false;
            f();
            return true;
        }

        // Wakes the opener when SkyrimNet settles its timeline.
        std::mutex              g_wakeLock;
        std::condition_variable g_wake;

        std::filesystem::path MillsJsonPath() {
            if (auto dir = logger::log_directory()) return *dir / "RumourEngine_Mills.json";
            return std::filesystem::temp_directory_path() / "RumourEngine_Mills.json";
        }

        void BuildMills(const char* reason) {
            auto set = Mills::Build(reason);
            if (!set) return;
            Mills::Log(*set, Settings::DumpMills());
            if (Settings::DumpMills()) Mills::WriteJson(*set, MillsJsonPath());
        }

        void SendModEvent(const char* name) {
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([name]() {
                    SKSE::ModCallbackEvent ev{ name, "", 0.0f, nullptr };
                    if (auto* source = SKSE::GetModCallbackEventSource()) source->SendEvent(&ev);
                });
            }
        }

        /** Sleep up to `ms`, or less if woken. False if a newer load has started. */
        bool Wait(unsigned gen, int ms) {
            std::unique_lock lk(g_wakeLock);
            g_wake.wait_for(lk, std::chrono::milliseconds(ms));
            return g_generation.load() == gen;
        }

        /** Open this save's database and settle its timeline. Runs on its own thread. */
        void Open(unsigned gen, double loadDays, bool newGame) {
            if (!SkyrimNet::Version()) {
                logger::info("Session: SkyrimNet isn't running, so there is no save database");
                return;
            }

            // SkyrimNet names the save once it has loaded its own state; on a new
            // game that can take a while.
            std::string saveId;
            for (int waited = 0;; waited += 250) {
                saveId = SkyrimNet::SaveUniqueID();
                if (!saveId.empty()) break;
                if (waited == 5000) logger::info("Session: still waiting for SkyrimNet's save id");
                if (!Wait(gen, 250)) return;
            }
            bool opened = false;
            if (!Current(gen, [&] { opened = Database::Open(Database::PathFor(saveId)); }) || !opened) return;

            // Hold everything until SkyrimNet's keep/clear prompt is answered.
            bool waitLogged = false;
            int  state = SkyrimNet::TimelineState();
            while (state == 1) {
                if (!waitLogged) {
                    logger::info("Session: waiting on SkyrimNet's keep/clear prompt");
                    waitLogged = true;
                }
                if (!Wait(gen, 1000)) return;
                state = SkyrimNet::TimelineState();
            }

            const bool still = Current(gen, [&] {
                if (state == 2) {
                    logger::info("Session: SkyrimNet kept the history after day {:.2f}, and so does RumourEngine", loadDays);
                } else if (!newGame) {
                    // Cleared, or SkyrimNet had nothing after this save (or is too old
                    // to say): anything of ours from later belongs to a timeline that's gone.
                    Promote::TakeBack(loadDays);  // first: it needs the rows that are about to go
                    const int n = Database::DeleteAfter(loadDays);
                    if (n > 0 || state == 3) {
                        logger::info("Session: {} -- took back {} rows after day {:.2f}",
                                     state == 3   ? "SkyrimNet cleared the history"
                                     : state == 0 ? "SkyrimNet had no history after this save"
                                                  : "SkyrimNet can't say what it kept",
                                     n, loadDays);
                    }
                }
                Placements::Load();  // first: who lives where decides everyone's mills
                Gossip::Load();  // before the rumours: who has heard one depends on these
                Rumours::Load();
                Harvest::Load();
                Quests::Load();
                g_ready = true;
            });
            if (!still) return;

            logger::info("Session: ready (save {}, day {:.2f}), {} live rumours; rows: {}", saveId, loadDays, Rumours::Count(),
                         Database::Counts());
            Api::DispatchEvent("ready", fmt::format(R"({{"type":"ready","save":"{}","day":{:.4f}}})", saveId, loadDays));
            SendModEvent("RumourEngine_Ready");
        }

        class ModEventSink final : public RE::BSTEventSink<SKSE::ModCallbackEvent> {
        public:
            RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* e,
                                                  RE::BSTEventSource<SKSE::ModCallbackEvent>*) override {
                if (!e) return RE::BSEventNotifyControl::kContinue;
                const std::string_view name = e->eventName.c_str();
                if (name == "SkyrimNet_TimelineResolved") {
                    g_wake.notify_all();
                } else if (name == "RumourEngine_AddRumour") {
                    std::string error;
                    if (!Rumours::Add(e->strArg.c_str(), error)) logger::warn("RumourEngine_AddRumour: {}", error);
                } else if (name == "RumourEngine_RetireRumour") {
                    const auto  j = nlohmann::json::parse(e->strArg.c_str(), nullptr, false);
                    std::string error;
                    if (!j.is_object() || !j.contains("source") || !j.contains("key") || !j["source"].is_string() ||
                        !j["key"].is_string()) {
                        logger::warn("RumourEngine_RetireRumour: needs {{\"source\", \"key\"}}, got {}", e->strArg.c_str());
                    } else if (!Rumours::RetireByKey(j["source"].get<std::string>(), j["key"].get<std::string>(), error)) {
                        logger::warn("RumourEngine_RetireRumour: {}", error);
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // Back at the main menu, by quitting to it or because a load failed: what was open belongs
        // to no save now. SKSE sends nothing for this, so the menu opening is the signal. Without
        // it, a failed load left the hourly look reading an empty world, and it took every
        // follower for dismissed.
        class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e,
                                                  RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                if (e && e->opening && e->menuName == RE::MainMenu::MENU_NAME && g_gameLoaded.exchange(false)) {
                    logger::info("Session: back at the main menu, so the save's database is closed");
                    OnPreLoad();
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void OnDataLoaded() {
        static ModEventSink sink;
        if (auto* source = SKSE::GetModCallbackEventSource()) source->AddEventSink(&sink);
        static MenuSink menus;
        if (auto* ui = RE::UI::GetSingleton()) ui->AddEventSink<RE::MenuOpenCloseEvent>(&menus);

        Settings::Refresh();
        if (!Settings::Enabled()) {
            logger::info("RumourEngine is turned off in its settings");
            return;
        }
        // Reading 600-odd plugins takes a second or two; the main menu needn't wait.
        std::thread([]() {
            auto scan = PluginScan::Run();
            Mills::SetScan(scan);
            Quests::SetScan(scan);
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([]() { BuildMills(g_gameLoaded ? "save loaded" : "data loaded"); });
            }
        }).detach();
    }

    void OnPreLoad() {
        {
            std::lock_guard lk(g_sessionLock);
            ++g_generation;
            g_ready = false;
            Rumours::Clear();
            Retell::Reset();
            Spread::Reset();
            Harvest::Reset();
            Placements::Reset();
            Gossip::Reset();
            Promote::Reset();
            Quests::Reset();
            Database::Close();
        }
        g_wake.notify_all();
    }

    void OnGameLoaded(bool newGame) {
        Settings::Refresh();
        if (!Settings::Enabled()) return;
        GossipStarter::Reset();
        unsigned gen;
        {
            std::lock_guard lk(g_sessionLock);
            gen = ++g_generation;
            g_ready = false;
        }
        g_gameLoaded = true;
        g_wake.notify_all();

        // Who exists depends on this save's enable state (the civil war swaps
        // whole garrisons), so the mills are rebuilt for every save.
        if (Mills::HasScan()) BuildMills(newGame ? "new game" : "save loaded");
        Retell::Reset();
        Spread::Reset();
        Harvest::Reset();
        Placements::Reset();
        Gossip::Reset();
        Promote::Reset();
        Quests::Reset();
        if (auto* player = RE::PlayerCharacter::GetSingleton()) Retell::PlayerMoved(player->GetCurrentLocation());

        auto*        calendar = RE::Calendar::GetSingleton();
        const double loadDays = calendar ? calendar->GetCurrentGameTime() : 0.0;
        std::thread(Open, gen, loadDays, newGame).detach();
    }

    bool IsReady() { return g_ready; }
}
