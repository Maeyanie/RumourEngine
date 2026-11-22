// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 maeyanie
//
// RumourEngine's C API, resolved at runtime with GetProcAddress. Include this
// header in your SKSE plugin and call RumourEngineAPI::Bind() once, at
// kDataLoaded or later. RumourEngine stays an optional dependency: every
// pointer is null when it isn't installed.
//
// Conventions (ADR-001, "Public API"):
//   - Nothing crosses the boundary as std::string or std::function.
//   - Strings go in as UTF-8. Queries return a char* that RumourEngine
//     allocated; release it with Free(), never your own free(). A null return
//     means not found or failed; GetLastError() says why, on the same thread.
//   - Complex input and output is JSON. Unknown fields are ignored.
//   - Every function is safe from any thread.
//   - Rumour calls fail until a save's database is open. Wait for the "ready"
//     listener event or the RumourEngine_Ready ModEvent. Mill queries work
//     from kDataLoaded on.
//   - GetApiVersion() returns 1. Version 1 was frozen on 2026-11-12: later
//     versions only add functions, JSON fields and listener events, and what
//     is here keeps working as described. A 0 is a pre-release draft build.

#pragma once

#include <cstdint>
#include <memory>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

extern "C" {
typedef void (*RumourEngine_EventCallback)(const char* eventJson, void* user);
}

namespace RumourEngineAPI {

    using GetApiVersion_t = int (*)();
    using IsReady_t = bool (*)();
    using Free_t = void (*)(void* p);

    /** Add a rumour (JSON request, ADR-001). Returns its id (>0), or 0 when rejected. */
    using AddRumour_t = std::int32_t (*)(const char* requestJson);
    using RetireRumour_t = bool (*)(std::int32_t id);
    using RetireRumourByKey_t = bool (*)(const char* source, const char* key);

    /** One rumour as JSON, or null if unknown. */
    using GetRumour_t = char* (*)(std::int32_t id);
    /** The live rumour with this source and key, as GetRumour gives it, or null. */
    using GetRumourByKey_t = char* (*)(const char* source, const char* key);
    /** JSON array of the rumours an actor has heard, freshest first. */
    using GetRumoursForActor_t = char* (*)(std::uint32_t formId, int maxCount);
    /** JSON array of an actor's mill ids, the one they live in first. Accepts a reference or a base form. */
    using GetMillsForActor_t = char* (*)(std::uint32_t formId);
    /** JSON array of every mill: id, kind, settlement, hold, size, pos. */
    using GetMills_t = char* (*)();
    using HasHeard_t = bool (*)(std::int32_t id, std::uint32_t formId);
    /** This thread's last failure, or null. */
    using GetLastError_t = char* (*)();

    /** Events as JSON on a RumourEngine worker thread; don't call RE:: from the callback.
     *  The string is valid only during the call. Types: ready, seeded, arrived, aired, retired, promoted.
     *  An empty or null CSV means every type. Returns a listener id (>0), or 0 on failure. */
    using RegisterListener_t = std::uint64_t (*)(const char* eventTypesCSV, RumourEngine_EventCallback cb, void* user);
    using UnregisterListener_t = bool (*)(std::uint64_t listenerId);

    inline GetApiVersion_t      GetApiVersion = nullptr;
    inline IsReady_t            IsReady = nullptr;
    inline Free_t               Free = nullptr;
    inline AddRumour_t          AddRumour = nullptr;
    inline RetireRumour_t       RetireRumour = nullptr;
    inline RetireRumourByKey_t  RetireRumourByKey = nullptr;
    inline GetRumour_t          GetRumour = nullptr;
    inline GetRumourByKey_t     GetRumourByKey = nullptr;
    inline GetRumoursForActor_t GetRumoursForActor = nullptr;
    inline GetMillsForActor_t   GetMillsForActor = nullptr;
    inline GetMills_t           GetMills = nullptr;
    inline HasHeard_t           HasHeard = nullptr;
    inline GetLastError_t       GetLastError = nullptr;
    inline RegisterListener_t   RegisterListener = nullptr;
    inline UnregisterListener_t UnregisterListener = nullptr;

    /** Resolve every function. Returns false, leaving them all null, if RumourEngine isn't loaded. */
    inline bool Bind() {
        HMODULE dll = GetModuleHandleA("RumourEngine");
        if (!dll) return false;
        auto get = [dll](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(dll, name));
        };
        get(GetApiVersion, "RumourEngine_GetApiVersion");
        get(IsReady, "RumourEngine_IsReady");
        get(Free, "RumourEngine_Free");
        get(AddRumour, "RumourEngine_AddRumour");
        get(RetireRumour, "RumourEngine_RetireRumour");
        get(RetireRumourByKey, "RumourEngine_RetireRumourByKey");
        get(GetRumour, "RumourEngine_GetRumour");
        get(GetRumourByKey, "RumourEngine_GetRumourByKey");
        get(GetRumoursForActor, "RumourEngine_GetRumoursForActor");
        get(GetMillsForActor, "RumourEngine_GetMillsForActor");
        get(GetMills, "RumourEngine_GetMills");
        get(HasHeard, "RumourEngine_HasHeard");
        get(GetLastError, "RumourEngine_GetLastError");
        get(RegisterListener, "RumourEngine_RegisterListener");
        get(UnregisterListener, "RumourEngine_UnregisterListener");
        return GetApiVersion != nullptr && Free != nullptr;
    }

    /** Owns a string RumourEngine returned, and frees it with RumourEngine's Free. */
    struct FreeDeleter {
        void operator()(char* p) const {
            if (p && Free) Free(p);
        }
    };
    using Owned = std::unique_ptr<char, FreeDeleter>;
}
