#pragma once

#include <nlohmann/json_fwd.hpp>

// Text from the game is in the game's code page, not UTF-8: names, place names, journal entries,
// editor IDs and Papyrus strings. Windows-1252 for English and most others, so "Jónsi" in a plugin
// is the bytes J F3 n s i, and nlohmann::json throws at the F3 when that goes into a prompt. Every
// string read from the game goes through FromGame before RumourEngine keeps it or sends it on.

namespace RumourEngine::Text {

    /** Works out the game's code page from its language setting. At kDataLoaded, before anything
     *  reads names; until then, and when the language isn't one it knows, Windows-1252. */
    void Init();

    /** Whether `s` is well-formed UTF-8, by the same rules nlohmann::json checks. */
    bool IsUtf8(std::string_view s);

    /** A game string as UTF-8: as it is when it already is (ASCII, and the Japanese and Chinese
     *  strings files), else read in the game's code page. */
    std::string FromGame(std::string_view s);
    inline std::string FromGame(const char* s) { return s ? FromGame(std::string_view(s)) : std::string{}; }

    /** JSON text for a prompt or a file, where a string that slipped past FromGame gets U+FFFD in
     *  place of its bad bytes instead of an exception, which on the game's threads ends the game. */
    std::string Dump(const nlohmann::json& j, int indent = -1);
}
