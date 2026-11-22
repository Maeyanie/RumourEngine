#pragma once

#include "PluginScan.h"

// Rumours of how the player's quests ended (ADR-001, "Quest endings"). When a quest ends, its
// journal (the first entry and the one the journal shows at the end, as the game fills them in)
// goes to the decision model: will word of it get beyond the people who took part, and will other
// holds talk of it? If word gets out, a telling is written and seeded where the player is, at a
// notability set by how far it would go. quests.ini says always or never for a quest, or for one
// of its endings, and is the whole rule without a decision model. Each quest's ending is handled
// once per save; the ones a save had finished before RumourEngine saw it are never news.

namespace RumourEngine::Quests {

    /** The plugins' journal entries, once the scan has read them. Any thread. */
    void SetScan(std::shared_ptr<const PluginScan::Result> scan);

    /** Listen for quest stages. Once, at kDataLoaded. */
    void Install();

    /** quests.ini and the save's endings. Once the database is open; the opening thread. */
    void Load();

    /** A different save is being loaded: forget its endings and anything under way. */
    void Reset();

    /** Look for quests that have ended, and take each through its decision, telling and seeding.
     *  The worker thread. */
    void Pump();

    /** Debugging (the inbox's {"quest_preview": "DA16"}): log what this quest's ending would send,
     *  and with `ask` the decision's answers and the telling too. Nothing is stored or seeded. */
    void Preview(const std::string& editorID, bool ask);
}
