#include "SkyrimNet.h"

// windows.h, which PublicAPI.h pulls in, defines macros that collide with
// CommonLibSSE names. This is the only file that includes it.
#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include "skyrimnet/PublicAPI.h"
#ifdef GetObject
#    undef GetObject
#endif
#ifdef SendMessage
#    undef SendMessage
#endif

namespace RumourEngine::SkyrimNet {

    namespace {
        int g_version = 0;
    }

    bool Bind() {
        if (!FindFunctions() || !PublicGetVersion) {
            logger::warn("SkyrimNet is not loaded; RumourEngine will build its mills but cannot do anything with them");
            return false;
        }
        g_version = PublicGetVersion();
        logger::info("SkyrimNet API v{}", g_version);
        if (g_version < 12) {
            logger::warn("SkyrimNet API v12 or later is required (beta 26+); some features will be missing");
        }
        return true;
    }

    int Version() { return g_version; }

    std::string SaveUniqueID() {
        return PublicGetSaveUniqueID ? PublicGetSaveUniqueID() : std::string{};
    }

    int TimelineState() {
        return PublicGetTimelineState ? PublicGetTimelineState() : -1;
    }

    std::string Setting(const char* path, const char* fallback) {
        if (!PublicGetPluginConfigValue) return fallback;
        try {
            return PublicGetPluginConfigValue("RumourEngine", path, fallback);
        } catch (...) {
            return fallback;
        }
    }

    bool RegisterDecorator(const char* name, const char* description, std::function<std::string(RE::Actor*)> fn) {
        if (!PublicRegisterDecorator) return false;
        return PublicRegisterDecorator(name, description, std::move(fn));
    }

    std::uint64_t UUIDOf(RE::FormID formId) {
        return PublicFormIDToUUID && formId ? PublicFormIDToUUID(formId) : 0;
    }

    std::string BioTemplateName(RE::FormID formId) {
        if (!PublicGetBioTemplateName || !formId) return {};
        try {
            return PublicGetBioTemplateName(formId);
        } catch (...) {
            return {};
        }
    }

    std::string RecentEvents(int maxCount, const char* typesCSV) {
        return PublicGetRecentEvents ? PublicGetRecentEvents(0, maxCount, typesCSV) : std::string("[]");
    }

    bool MemoryReady() { return PublicIsMemorySystemReady && PublicIsMemorySystemReady(); }

    bool IsActorBusy(RE::FormID formId) { return PublicIsActorBusy && PublicIsActorBusy(formId); }

    bool SendCustomPrompt(const char* prompt, const char* variant, const std::string& contextJson,
                          std::function<void(std::string text, bool ok)> done) {
        if (!PublicSendCustomPromptToLLM) return false;
        return PublicSendCustomPromptToLLM(prompt, variant, contextJson.c_str(),
                                           [done = std::move(done)](const char* response, int success) {
                                               // The pointer dies with this call.
                                               done(response ? response : "", success != 0);
                                           });
    }

    bool CanRegisterEvents() { return PublicRegisterEvent != nullptr; }

    int RegisterEvent(const char* type, const std::string& content, RE::FormID originator, RE::FormID target,
                      const std::vector<RE::FormID>& audience) {
        if (!PublicRegisterEvent) return 0;
        return PublicRegisterEvent(type, content.c_str(), originator, target, audience.empty() ? nullptr : audience.data(),
                                   static_cast<std::uint32_t>(audience.size()));
    }

    RE::FormID FormOf(std::uint64_t uuid) { return PublicUUIDToFormID && uuid ? PublicUUIDToFormID(uuid) : 0; }

    std::string ActorEngagement(int maxCount, double shortWindowSeconds, double mediumWindowSeconds) {
        if (!PublicGetActorEngagement) return "[]";
        return PublicGetActorEngagement(maxCount, true, false, shortWindowSeconds, mediumWindowSeconds);
    }

    std::string QueryMemories(RE::FormID formId, const MemoryFilter& filter) {
        MemoryQuery q;
        q.maxCount = filter.maxCount;
        q.minImportance = filter.minImportance;
        q.gameTimeAfter = filter.afterSeconds;
        q.gameTimeBefore = filter.beforeSeconds;
        // Newest first: the floor has already picked what's worth a look, and news is news. A
        // follower's time away is the exception: its best stories, however old.
        q.orderBy = filter.byImportance ? MemoryOrder::ImportanceDesc : MemoryOrder::GameTimeDesc;
        return QueryMemoriesForActor(formId, q);
    }

    std::int64_t AddMemory(RE::FormID formId, const std::string& text, float importance, const char* type,
                           const std::string& location, const std::string& tagsJson) {
        if (!PublicAddMemory) return 0;
        return PublicAddMemory(formId, text.c_str(), importance, type, nullptr, location.empty() ? nullptr : location.c_str(),
                               tagsJson.c_str(), nullptr);
    }

    bool SendCustomDecision(const char* templateName, const std::string& contextJson,
                            std::function<void(std::string answer, std::string json, bool ok)> done) {
        if (!PublicSendCustomDecisionToLLM) return false;
        return PublicSendCustomDecisionToLLM(templateName, contextJson.c_str(), 0,
                                             [done = std::move(done)](const char* answer, const char* json, int success) {
                                                 // The pointers die with this call.
                                                 done(answer ? answer : "", json ? json : "", success != 0);
                                             });
    }

    int AddWorldKnowledge(const std::string& content, const std::string& condition, bool alwaysInject, float importance,
                          const std::string& displayName) {
        if (!PublicAddWorldKnowledge) return 0;
        return PublicAddWorldKnowledge(content.c_str(), condition.c_str(), alwaysInject, importance, displayName.c_str());
    }

    bool RemoveWorldKnowledge(int id) { return PublicRemoveWorldKnowledge && id > 0 && PublicRemoveWorldKnowledge(id); }
}
