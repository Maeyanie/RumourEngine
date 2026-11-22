#pragma once

// RumourEngine's view of SkyrimNet's public API. SkyrimNet's own header
// (skyrimnet/PublicAPI.h, MIT) defines its function pointers as globals, so it
// may be included by one translation unit only: SkyrimNet.cpp. Everything else
// goes through these wrappers, which are safe to call when SkyrimNet is absent
// or older than the function needs.

namespace RumourEngine::SkyrimNet {

    /** Resolve SkyrimNet's exports. Call once, at kDataLoaded. */
    bool Bind();

    /** SkyrimNet's API version, or 0 if it isn't loaded. */
    int Version();

    /** SkyrimNet's id for the loaded save; empty before one is loaded. v7+. */
    std::string SaveUniqueID();

    /** 0 none, 1 pending, 2 kept, 3 cleared; -1 if SkyrimNet can't say (needs v11). */
    int TimelineState();

    /** A plugin setting from RumourEngine's SkyrimNet settings page, or `fallback`. */
    std::string Setting(const char* path, const char* fallback);

    /** Register a prompt decorator, {{ name(actorUUID) }}. False if SkyrimNet can't (v5+). */
    bool RegisterDecorator(const char* name, const char* description, std::function<std::string(RE::Actor*)> fn);

    /** SkyrimNet's UUID for an actor (reference or base form), or 0 if it doesn't know them. */
    std::uint64_t UUIDOf(RE::FormID formId);

    /** The name of an actor's bio file, without its extension ("hulda_66E"); empty if none. */
    std::string BioTemplateName(RE::FormID formId);

    /** The newest events of these types, as SkyrimNet's JSON array; "[]" if it can't say. */
    std::string RecentEvents(int maxCount, const char* typesCSV);

    bool MemoryReady();
    bool IsActorBusy(RE::FormID formId);
    bool CanRegisterEvents();

    /** Render a prompt template from the layer and send it to the LLM through a variant (v8+).
     *  `done` runs on a SkyrimNet worker thread with the answer, or the error when !ok; it must not
     *  touch RE::. False if it couldn't be queued. */
    bool SendCustomPrompt(const char* prompt, const char* variant, const std::string& contextJson,
                          std::function<void(std::string text, bool ok)> done);

    /** Register an event (v11+). An empty audience means everyone nearby. Returns its id, or 0. */
    int RegisterEvent(const char* type, const std::string& content, RE::FormID originator, RE::FormID target,
                      const std::vector<RE::FormID>& audience);

    /** The form SkyrimNet last saw for this UUID, or 0. */
    RE::FormID FormOf(std::uint64_t uuid);

    /** Who has been having memories and events lately, as SkyrimNet's JSON array (player left out);
     *  "[]" if it can't say. Windows are in game-seconds. */
    std::string ActorEngagement(int maxCount, double shortWindowSeconds, double mediumWindowSeconds);

    /** What the harvest asks of an actor's memories; SkyrimNet filters in SQL before the cut. */
    struct MemoryFilter {
        int    maxCount = 10;
        float  minImportance = 0.0f;
        double afterSeconds = 0.0;   // game-seconds, as the rows' game_time
        double beforeSeconds = 0.0;
        bool   byImportance = false;  // most important first, instead of newest
    };

    /** An actor's active memories, newest (or most important) first, as SkyrimNet's JSON array; "[]" before v10. */
    std::string QueryMemories(RE::FormID formId, const MemoryFilter& filter);

    /** Store a memory for an actor (v5+). Blocks while SkyrimNet embeds it, so not on the main
     *  thread. Returns its id, or 0. */
    std::int64_t AddMemory(RE::FormID formId, const std::string& text, float importance, const char* type,
                           const std::string& location, const std::string& tagsJson);

    /** Ask a decision template under prompts/decisions/ (v12+). `done` runs on a SkyrimNet worker
     *  thread with the first answer and the whole result JSON (the error JSON when !ok); it must not
     *  touch RE::. False if it couldn't be queued. */
    bool SendCustomDecision(const char* templateName, const std::string& contextJson,
                            std::function<void(std::string answer, std::string json, bool ok)> done);

    /** Add a world-knowledge entry (v7+), retrieved for any NPC whose condition expression holds.
     *  Embeds it, so not on the main thread. Returns its id, or 0. */
    int AddWorldKnowledge(const std::string& content, const std::string& condition, bool alwaysInject, float importance,
                          const std::string& displayName);

    /** Remove a world-knowledge entry (v7+). False if it is gone already. */
    bool RemoveWorldKnowledge(int id);
}
