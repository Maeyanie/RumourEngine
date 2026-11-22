#pragma once

#include <set>

// What the mill builder needs from the plugin files that the game doesn't keep
// in memory at kDataLoaded:
//   - placed actors (ACHR). Temporary references aren't loaded until their cell
//     is: on vanilla, 496 of the 706 generic placements and 390 of the named
//     NPCs placed without a location row exist only as temporary references;
//   - cells' locations and grid positions, for where those actors live and
//     where settlements sit on the map;
//   - editor IDs of NPCs, locations and factions, which the game discards;
//   - locations' unique-NPC rows. The game has them in memory, but CommonLibSSE's
//     layout of them is uncertain, and reading them as pointers would guess;
//   - quests' journal entries, which the game reads from the files only when the
//     journal shows them.
// Every active plugin is read once, in load order; a later file's record
// replaces an earlier one's, as in the game.

namespace RumourEngine::PluginScan {

    struct Cell {
        RE::FormID    world = 0;     // 0 for interiors
        RE::FormID    location = 0;  // XLCN
        std::int32_t  x = 0, y = 0;  // XCLC, exteriors only
        bool          hasGrid = false;
        bool          interior = false;
    };

    struct Placement {
        RE::FormID base = 0;          // NAME
        RE::FormID cell = 0;          // the cell whose children group holds it
        RE::FormID location = 0;      // XLCN: the reference's own persistent location
        RE::FormID enableParent = 0;  // XESP
        bool       opposite = false;  // enabled when the parent is disabled
        bool       disabled = false;  // initially disabled
        bool       persistent = false;
        float      pos[3]{};
    };

    /** A location's row naming a unique NPC who lives there (LCUN, or ACUN in an override). */
    struct UniqueRow {
        RE::FormID npc = 0;
        RE::FormID ref = 0;
        RE::FormID location = 0;  // the row's own location, else the record's
        auto operator<=>(const UniqueRow&) const = default;
    };

    /** One journal entry of a quest stage, as the plugin has it. */
    struct LogEntry {
        std::uint8_t  flags = 0;     // QSDT: 1 completes the quest, 2 fails it
        std::uint32_t stringId = 0;  // CNAM in a localized plugin: an id in its DLSTRINGS
        std::string   text;          // CNAM in one that isn't
    };

    struct QuestStage {
        std::uint16_t         index = 0;
        std::vector<LogEntry> entries;  // in the record's order, which is each entry's index in game
    };

    /** A quest's journal, from the last plugin to change the quest. */
    struct QuestJournal {
        std::string             file;  // that plugin, whose strings a localized one's ids are in
        bool                    localized = false;
        std::vector<QuestStage> stages;
    };

    struct Result {
        std::unordered_map<RE::FormID, Cell>        cells;
        std::unordered_map<RE::FormID, Placement>   actors;  // by reference
        std::unordered_map<RE::FormID, std::string> npcEditorIDs;
        std::unordered_map<RE::FormID, std::string> locationEditorIDs;
        std::unordered_map<RE::FormID, RE::FormID>  locationMarkers;  // LCTN MNAM
        std::unordered_map<RE::FormID, std::string> factionEditorIDs;
        std::set<UniqueRow>                         uniqueRows;  // merged across files, as the engine does
        std::unordered_map<RE::FormID, QuestJournal> quests;     // quests with any journal entry

        std::size_t files = 0;
        std::size_t failedFiles = 0;
        std::uint64_t bytes = 0;
        double      seconds = 0.0;
    };

    /** Read every loaded plugin. Safe off the main thread once data is loaded. */
    std::shared_ptr<const Result> Run();
}
