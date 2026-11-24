#include "Api.h"
#include "Delivery.h"
#include "Mills.h"
#include "Quests.h"
#include "Rumours.h"
#include "Runtime.h"
#include "Session.h"
#include "Text.h"

#include "RumourEngineAPI.h"
#ifdef GetObject
#    undef GetObject
#endif
#ifdef SendMessage
#    undef SendMessage
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <thread>

namespace RumourEngine::Api {

    namespace {
        thread_local std::string t_lastError;

        struct Listener {
            std::uint64_t                   id;
            std::unordered_set<std::string> types;  // empty: every type
            RumourEngine_EventCallback      cb;
            void*                           user;
        };
        std::mutex            g_listenersLock;
        std::vector<Listener> g_listeners;
        std::uint64_t         g_nextListener = 1;

        // Events go out on one worker thread of RumourEngine's, in order, so a
        // listener never runs on the thread of whoever caused the event.
        std::mutex                                       g_queueLock;
        std::condition_variable                          g_queueWake;
        std::deque<std::pair<std::string, std::string>> g_queue;
        std::once_flag                                   g_workerStarted;

        void Deliver(const std::string& type, const std::string& json) {
            std::vector<Listener> targets;
            {
                std::lock_guard lk(g_listenersLock);
                for (const auto& l : g_listeners) {
                    if (l.types.empty() || l.types.contains(type)) targets.push_back(l);
                }
            }
            for (const auto& l : targets) {
                try {
                    l.cb(json.c_str(), l.user);
                } catch (...) {
                    logger::warn("API: a {} listener threw", type);
                }
            }
        }

        void EventWorker() {
            for (;;) {
                std::pair<std::string, std::string> next;
                {
                    std::unique_lock lk(g_queueLock);
                    g_queueWake.wait(lk, [] { return !g_queue.empty(); });
                    next = std::move(g_queue.front());
                    g_queue.pop_front();
                }
                Deliver(next.first, next.second);
            }
        }
    }

    void DispatchEvent(std::string_view type, const std::string& json) {
        {
            std::lock_guard lk(g_listenersLock);
            if (g_listeners.empty()) return;
        }
        std::call_once(g_workerStarted, [] { std::thread(EventWorker).detach(); });
        {
            std::lock_guard lk(g_queueLock);
            g_queue.emplace_back(std::string(type), json);
        }
        g_queueWake.notify_one();
    }

    void SetLastError(std::string message) { t_lastError = std::move(message); }

    void PollInbox() {
        auto logDir = RumourEngine::Runtime::LogDirectory();
        if (!logDir) return;
        const auto      inbox = *logDir / "RumourEngine_Inbox";
        std::error_code ec;
        if (!std::filesystem::is_directory(inbox, ec)) return;
        for (const auto& entry : std::filesystem::directory_iterator(inbox, ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
            std::string text;
            {
                std::ifstream f(entry.path(), std::ios::binary);
                text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            }
            const auto j = nlohmann::json::parse(text, nullptr, false);
            bool       ok = !j.is_discarded();
            std::vector<nlohmann::json> requests;
            if (j.is_array()) requests.assign(j.begin(), j.end());
            else if (ok) requests.push_back(j);
            for (const auto& r : requests) {
                std::string error;
                if (r.is_object() && r.contains("retire")) {
                    const auto& what = r["retire"];
                    const bool  done = what.is_number_integer()
                                           ? Rumours::Retire(what.get<std::int32_t>(), error)
                                           : what.is_object() && Rumours::RetireByKey(what.value("source", ""), what.value("key", ""), error);
                    if (!done) {
                        ok = false;
                        logger::warn("Inbox {}: {}", entry.path().filename().string(), error);
                    }
                } else if (r.is_object() && r.contains("quest_preview") && r["quest_preview"].is_string()) {
                    Quests::Preview(r["quest_preview"].get<std::string>(), r.value("ask", false));
                } else if (!Rumours::Add(r.dump(), error)) {
                    ok = false;
                    logger::warn("Inbox {}: {}", entry.path().filename().string(), error);
                }
            }
            if (j.is_discarded()) logger::warn("Inbox {}: not JSON", entry.path().filename().string());
            auto done = entry.path();
            done.replace_extension(ok ? ".done" : ".failed");
            std::filesystem::remove(done, ec);
            std::filesystem::rename(entry.path(), done, ec);
        }
    }

    /** A copy the caller frees with RumourEngine_Free. */
    char* Give(const std::string& s) {
        auto* p = static_cast<char*>(std::malloc(s.size() + 1));
        if (p) std::memcpy(p, s.c_str(), s.size() + 1);
        return p;
    }
}

using namespace RumourEngine;
using Api::Give;

extern "C" {

__declspec(dllexport) int RumourEngine_GetApiVersion() { return kApiVersion; }

__declspec(dllexport) bool RumourEngine_IsReady() { return Session::IsReady(); }

__declspec(dllexport) void RumourEngine_Free(void* p) { std::free(p); }

__declspec(dllexport) std::int32_t RumourEngine_AddRumour(const char* requestJson) {
    std::string error;
    const auto  id = Rumours::Add(requestJson ? requestJson : "", error);
    if (!id) {
        Api::SetLastError(error);
        logger::warn("AddRumour: {}", error);
    }
    return id;
}

__declspec(dllexport) bool RumourEngine_RetireRumour(std::int32_t id) {
    std::string error;
    if (Rumours::Retire(id, error)) return true;
    Api::SetLastError(error);
    return false;
}

__declspec(dllexport) bool RumourEngine_RetireRumourByKey(const char* source, const char* key) {
    std::string error;
    if (Rumours::RetireByKey(Text::FromGame(source), Text::FromGame(key), error)) return true;
    Api::SetLastError(error);
    return false;
}

__declspec(dllexport) char* RumourEngine_GetRumour(std::int32_t id) {
    auto j = Rumours::Describe(id);
    if (!j) {
        Api::SetLastError("no live rumour #" + std::to_string(id));
        return nullptr;
    }
    return Give(j->dump());
}

__declspec(dllexport) char* RumourEngine_GetRumourByKey(const char* source, const char* key) {
    auto j = Rumours::DescribeByKey(Text::FromGame(source), Text::FromGame(key));
    if (!j) {
        Api::SetLastError(fmt::format("no live rumour from {} with key {}", Text::FromGame(source), Text::FromGame(key)));
        return nullptr;
    }
    return Give(j->dump());
}

__declspec(dllexport) char* RumourEngine_GetRumoursForActor(std::uint32_t formId, int maxCount) {
    auto set = Mills::Current();
    if (!set) {
        Api::SetLastError("mills are not built yet");
        return nullptr;
    }
    auto*      actor = RE::TESForm::LookupByID<RE::Actor>(formId);
    const auto mills = actor ? set->MillsFor(actor) : set->MillsFor(formId);
    const auto base = actor && actor->GetActorBase() ? actor->GetActorBase()->GetFormID() : formId;
    if (actor && Delivery::InPlayersCompany(actor)) return Give("[]");
    auto out = nlohmann::json::array();
    for (const auto& h : Rumours::InMills(mills, formId, base, Rumours::Now(), static_cast<std::size_t>(std::max(1, maxCount)))) {
        out.push_back({ { "id", h.id }, { "mill", h.mill }, { "text", h.text }, { "arrived", h.arrived },
                        { "notability", h.notability }, { "about_them", h.aboutThem }, { "fact", h.fact } });
    }
    return Give(out.dump());
}

__declspec(dllexport) char* RumourEngine_GetMillsForActor(std::uint32_t formId) {
    auto set = Mills::Current();
    if (!set) {
        Api::SetLastError("mills are not built yet");
        return nullptr;
    }
    return Give(nlohmann::json(set->MillsFor(formId)).dump());
}

__declspec(dllexport) char* RumourEngine_GetMills() {
    auto set = Mills::Current();
    if (!set) {
        Api::SetLastError("mills are not built yet");
        return nullptr;
    }
    auto out = nlohmann::json::array();
    for (const auto& m : set->mills) {
        out.push_back({ { "id", m.id },
                        { "kind", Mills::KindName(m.kind) },
                        { "settlement", m.settlement ? nlohmann::json(Mills::PlaceName(m.settlement)) : nlohmann::json(nullptr) },
                        { "hold", m.hold ? nlohmann::json(Mills::PlaceName(m.hold)) : nlohmann::json(nullptr) },
                        { "size", m.members.size() },
                        { "eager", set->IsEager(m.id) },
                        { "pos", m.pos ? nlohmann::json::array({ m.pos->x, m.pos->y }) : nlohmann::json(nullptr) } });
    }
    return Give(out.dump());
}

__declspec(dllexport) bool RumourEngine_HasHeard(std::int32_t id, std::uint32_t formId) {
    return Rumours::HasHeard(id, formId);
}

__declspec(dllexport) char* RumourEngine_GetLastError() {
    return Api::t_lastError.empty() ? nullptr : Give(Api::t_lastError);
}

__declspec(dllexport) std::uint64_t RumourEngine_RegisterListener(const char* eventTypesCSV,
                                                                   RumourEngine_EventCallback cb, void* user) {
    if (!cb) {
        Api::SetLastError("no callback");
        return 0;
    }
    Api::Listener l{ 0, {}, cb, user };
    std::string_view csv = eventTypesCSV ? eventTypesCSV : "";
    while (!csv.empty()) {
        const auto comma = csv.find(',');
        auto       part = csv.substr(0, comma);
        while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
        while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
        if (!part.empty()) l.types.emplace(part);
        csv = comma == std::string_view::npos ? std::string_view{} : csv.substr(comma + 1);
    }
    std::lock_guard lk(Api::g_listenersLock);
    l.id = Api::g_nextListener++;
    Api::g_listeners.push_back(l);
    return l.id;
}

__declspec(dllexport) bool RumourEngine_UnregisterListener(std::uint64_t listenerId) {
    std::lock_guard lk(Api::g_listenersLock);
    auto it = std::find_if(Api::g_listeners.begin(), Api::g_listeners.end(), [&](auto& l) { return l.id == listenerId; });
    if (it == Api::g_listeners.end()) return false;
    Api::g_listeners.erase(it);
    return true;
}
}

// The shipped header and the exports must agree.
static_assert(std::is_same_v<decltype(&RumourEngine_GetApiVersion), RumourEngineAPI::GetApiVersion_t>);
static_assert(std::is_same_v<decltype(&RumourEngine_IsReady), RumourEngineAPI::IsReady_t>);
static_assert(std::is_same_v<decltype(&RumourEngine_Free), RumourEngineAPI::Free_t>);
static_assert(std::is_same_v<decltype(&RumourEngine_AddRumour), RumourEngineAPI::AddRumour_t>);
static_assert(std::is_same_v<decltype(&RumourEngine_RetireRumour), RumourEngineAPI::RetireRumour_t>);
static_assert(std::is_same_v<decltype(&RumourEngine_RetireRumourByKey), RumourEngineAPI::RetireRumourByKey_t>);
static_assert(std::is_same_v<decltype(&RumourEngine_GetRumour), RumourEngineAPI::GetRumour_t>);
static_assert(std::is_same_v<decltype(&RumourEngine_GetRumourByKey), RumourEngineAPI::GetRumourByKey_t>);
static_assert(std::is_same_v<decltype(&RumourEngine_GetRumoursForActor), RumourEngineAPI::GetRumoursForActor_t>);
static_assert(std::is_same_v<decltype(&RumourEngine_GetMillsForActor), RumourEngineAPI::GetMillsForActor_t>);
static_assert(std::is_same_v<decltype(&RumourEngine_GetMills), RumourEngineAPI::GetMills_t>);
static_assert(std::is_same_v<decltype(&RumourEngine_HasHeard), RumourEngineAPI::HasHeard_t>);
static_assert(std::is_same_v<decltype(&RumourEngine_GetLastError), RumourEngineAPI::GetLastError_t>);
static_assert(std::is_same_v<decltype(&RumourEngine_RegisterListener), RumourEngineAPI::RegisterListener_t>);
static_assert(std::is_same_v<decltype(&RumourEngine_UnregisterListener), RumourEngineAPI::UnregisterListener_t>);
