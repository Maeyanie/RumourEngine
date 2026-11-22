#include "GossipStarter.h"
#include "Api.h"
#include "Delivery.h"
#include "Gossip.h"
#include "Harvest.h"
#include "Mills.h"
#include "Placements.h"
#include "Promote.h"
#include "Quests.h"
#include "Retell.h"
#include "Rumours.h"
#include "Session.h"
#include "Settings.h"
#include "SkyrimNet.h"
#include "Spread.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <set>
#include <thread>

namespace RumourEngine::GossipStarter {

    namespace {

        using Clock = std::chrono::steady_clock;

        constexpr int    kPollMs = 4000;
        constexpr double kQuietSeconds = 20.0;   // real seconds since anyone spoke
        constexpr double kSettleSeconds = 30.0;  // after a load, before the first telling
        constexpr int    kSweepEvery = 15;       // polls between retirement sweeps, about a minute
        constexpr float  kEarshot = 1200.0f;     // gossip to player, ~17 m
        constexpr float  kCloseEnough = 400.0f;  // gossip to listener, ~6 m
        constexpr float  kSameFloor = 250.0f;    // |dZ| for either
        constexpr RE::FormID kActorTypeNPC = 0x00013794;

        // What counts as someone speaking. npc_thoughts aren't said aloud.
        constexpr const char* kSpeechEvents =
            "dialogue,dialogue_npc,dialogue_player,dialogue_player_text,dialogue_background,"
            "gamemaster_dialogue,direct_narration";

        std::atomic<bool> g_running = false;

        std::mutex                                   g_lock;
        std::set<std::pair<RE::FormID, std::int32_t>> g_told;  // (gossip, rumour) already passed on
        Clock::time_point                            g_nextAllowed{};

        bool CoolingDown() {
            std::lock_guard lk(g_lock);
            return Clock::now() < g_nextAllowed;
        }

        void CoolDown(double seconds) {
            std::lock_guard lk(g_lock);
            g_nextAllowed = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
        }

        /** Nobody, player, NPC or GameMaster, has spoken for a while. Worker thread only.
         *  False when SkyrimNet can't say: interrupting is the thing to avoid. */
        bool RoomIsQuiet() {
            if (!SkyrimNet::MemoryReady()) return false;
            const auto events = nlohmann::json::parse(SkyrimNet::RecentEvents(1, kSpeechEvents), nullptr, false);
            if (events.is_discarded() || !events.is_array()) return false;
            if (events.empty()) return true;
            const auto& last = events.front();
            if (!last.contains("localTime") || !last["localTime"].is_number()) return false;
            const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
            return now - last["localTime"].get<double>() >= kQuietSeconds;
        }

        bool Allowed(Settings::TellWhere rule, const Mills::MillSet& set, RE::BGSLocation* here) {
            switch (rule) {
            case Settings::TellWhere::Never: return false;
            case Settings::TellWhere::Anywhere: return true;
            case Settings::TellWhere::Towns: return here && set.LocationIs(here->GetFormID(), Mills::kSettlement);
            case Settings::TellWhere::Taverns: return here && set.LocationIs(here->GetFormID(), Mills::kInn);
            }
            return false;
        }

        std::string NameOf(RE::Actor* a) {
            const char* n = a ? a->GetDisplayFullName() : nullptr;
            return n ? n : "";
        }

        void Tick() {
            if (!g_running || !Session::IsReady() || !Settings::StarterEnabled() || CoolingDown()) return;
            if (!SkyrimNet::CanRegisterEvents()) return;
            auto set = Mills::Current();
            if (!set) return;

            auto* ui = RE::UI::GetSingleton();
            if (!ui || ui->GameIsPaused() || ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME)) return;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->Is3DLoaded() || player->IsInCombat()) return;
            auto* playerCell = player->GetParentCell();
            if (!playerCell) return;
            const bool interior = playerCell->IsInteriorCell();
            auto*      playerWorld = player->GetWorldspace();
            const auto playerPos = player->GetPosition();
            auto*      here = player->GetCurrentLocation();

            const bool mayTellPlayer = Allowed(Settings::TellPlayer(), *set, here) && !Delivery::InIntimateScene(player);
            const bool mayTellParty = Allowed(Settings::TellParty(), *set, here);
            auto*      kwNPC = RE::TESForm::LookupByID<RE::BGSKeyword>(kActorTypeNPC);

            // Somebody who could be talking: here with us, at ease, and free.
            auto available = [&](RE::Actor* a) {
                if (!a || a == player || a->IsDead() || a->IsDisabled() || !a->Is3DLoaded()) return false;
                if (interior ? a->GetParentCell() != playerCell : a->GetWorldspace() != playerWorld) return false;
                if (kwNPC && !a->HasKeyword(kwNPC)) return false;
                if (a->IsChild() || a->IsInCombat() || a->IsHostileToActor(player)) return false;
                if (a->GetCurrentScene()) return false;  // a vanilla scene is already a conversation
                if (Delivery::InIntimateScene(a)) return false;
                if (auto* state = a->AsActorState(); state && state->GetSitSleepState() == RE::SIT_SLEEP_STATE::kIsSleeping) {
                    return false;
                }
                return !SkyrimNet::IsActorBusy(a->GetFormID());
            };

            struct Near {
                RE::Actor*   actor;
                RE::NiPoint3 pos;
                float        toPlayer;
                bool         party;
            };
            std::vector<Near> nearby;
            if (auto* lists = RE::ProcessLists::GetSingleton()) {
                for (auto& handle : lists->highActorHandles) {
                    auto a = handle.get();
                    if (!a || !available(a.get())) continue;
                    const auto pos = a->GetPosition();
                    if (std::fabs(pos.z - playerPos.z) > kSameFloor) continue;
                    const float d = pos.GetDistance(playerPos);
                    if (d <= kEarshot + kCloseEnough) nearby.push_back({ a.get(), pos, d, Delivery::InPlayersCompany(a.get()) });
                }
            }
            if (nearby.empty()) return;
            std::sort(nearby.begin(), nearby.end(), [](const Near& x, const Near& y) { return x.toPlayer < y.toPlayer; });

            const double now = Rumours::Now();
            for (const auto& g : nearby) {
                if (g.toPlayer > kEarshot) break;  // sorted: nobody further is in earshot
                if (g.party) continue;             // the party is in no mill
                const auto pick = Delivery::PickTelling(g.actor, now);
                if (!pick) continue;
                {
                    std::lock_guard lk(g_lock);
                    if (g_told.contains({ g.actor->GetFormID(), pick->id })) continue;
                }

                // Whom to tell: someone who hasn't heard it, else anyone; the nearest of those.
                RE::Actor* listener = nullptr;
                bool       listenerHeard = true;
                float      best = kCloseEnough;
                auto consider = [&](RE::Actor* l, const RE::NiPoint3& pos) {
                    if (std::fabs(pos.z - g.pos.z) > kSameFloor) return;
                    const float d = pos.GetDistance(g.pos);
                    if (d > kCloseEnough) return;
                    const bool heard = Rumours::HasHeard(pick->id, l->GetFormID());
                    if (listener && (heard > listenerHeard || (heard == listenerHeard && d >= best))) return;
                    listener = l;
                    listenerHeard = heard;
                    best = d;
                };
                for (const auto& l : nearby) {
                    if (l.actor == g.actor || (l.party && !mayTellParty)) continue;
                    consider(l.actor, l.pos);
                }
                if (mayTellPlayer) consider(player, playerPos);
                if (!listener) continue;

                const std::string gn = NameOf(g.actor);
                const std::string ln = NameOf(listener);
                if (gn.empty() || ln.empty()) continue;

                // Asked, not told, when the listener is someone this version still names.
                const auto* lbase = listener->GetActorBase();
                const bool  ask = std::any_of(pick->named.begin(), pick->named.end(), [&](RE::FormID f) {
                    return f && (f == listener->GetFormID() || (lbase && f == lbase->GetFormID()));
                });
                const std::string place = Delivery::PlaceOf(pick->mill);
                const std::string narration =
                    ask ? gn + " turns to " + ln + " to ask whether it's true, what people in " + place + " are saying about " +
                              (listener == player ? std::string("them") : ln) + ": " + pick->text
                        : gn + " turns to " + ln + " to pass on some of the talk going round " + place + ".";

                // Party members who may not be told are left out of the audience too, so
                // SkyrimNet doesn't pick them to answer what they overheard.
                std::vector<RE::FormID> audience;
                std::vector<RE::FormID> earshot{ player->GetFormID() };
                std::vector<RE::FormID> carriers;  // who may take it home: in earshot, and not travelling with us
                const bool              restrict = !mayTellParty && std::any_of(nearby.begin(), nearby.end(), [](auto& n) { return n.party; });
                for (const auto& n : nearby) {
                    if (n.pos.GetDistance(g.pos) > kEarshot) continue;
                    if (n.party && !mayTellParty) continue;
                    earshot.push_back(n.actor->GetFormID());
                    if (!n.party && n.actor != g.actor) carriers.push_back(n.actor->GetFormID());
                }
                if (restrict) audience = earshot;

                const int id = SkyrimNet::RegisterEvent("direct_narration", narration, g.actor->GetFormID(),
                                                        listener->GetFormID(), audience);
                {
                    std::lock_guard lk(g_lock);
                    g_told.insert({ g.actor->GetFormID(), pick->id });
                }
                // Cool down either way: a refusal tried again every few seconds would only fill the log.
                CoolDown(Settings::StarterCooldownSeconds());
                if (id != 0) {
                    Rumours::NoteAiring(pick->id, pick->mill, g.actor->GetFormID(), listener->GetFormID(), earshot, now);
                    logger::info("Gossip: {} ({:08X}) {} rumour #{} {} {} ({:08X}), {:.0f}u from the player{}: {}", gn,
                                 g.actor->GetFormID(), ask ? "asks about" : "passes on", pick->id, ask ? "with" : "to", ln,
                                 listener->GetFormID(), g.toPlayer, restrict ? ", party left out" : "",
                                 pick->text.substr(0, 120));
                    // Anyone who overheard it and lives elsewhere takes it home.
                    Rumours::CarryHome(pick->id, pick->mill, carriers, now);
                } else {
                    logger::warn("Gossip: SkyrimNet stored nothing for '{}' -- event type disabled or filtered?", narration);
                }
                return;
            }
        }

        void WorkerLoop() {
            int polls = 0;
            while (g_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
                if (!g_running) break;
                Settings::Refresh();
                if (!Session::IsReady()) continue;
                Api::PollInbox();
                Spread::Tick();
                Retell::Pump();
                Placements::Pump();
                Harvest::Pump();
                Gossip::Pump();
                Promote::Pump();
                Quests::Pump();
                if (++polls % kSweepEvery == 0) Rumours::Sweep(Rumours::Now());
                if (!Settings::StarterEnabled() || CoolingDown() || !RoomIsQuiet()) continue;
                if (auto* task = SKSE::GetTaskInterface()) task->AddTask([]() { Tick(); });
            }
        }
    }

    void Start() {
        bool expected = false;
        if (!g_running.compare_exchange_strong(expected, true)) return;
        // Detached: a joinable thread still alive at exit would abort the game on quit.
        std::thread(WorkerLoop).detach();
        logger::info("GossipStarter: started (poll {} ms)", kPollMs);
    }

    void Stop() { g_running = false; }

    void Reset() {
        {
            std::lock_guard lk(g_lock);
            g_told.clear();
        }
        CoolDown(kSettleSeconds);
    }
}
