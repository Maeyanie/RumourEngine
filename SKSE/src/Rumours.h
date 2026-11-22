#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <set>

// The rumours going round, and which mills they are in (ADR-001, "Decision").
// Kept in memory for the prompt decorators, written through to the save's
// database. Rumours come from the API and the harvest (Harvest.cpp). A spread rumour
// hops from its origin (Spread.cpp), a proclaimed one reaches every mill after
// the travel time, and each mill's version is written when first needed
// (Retell.cpp). Not everyone in a mill has heard everything there: each person has a
// fixed roll per rumour, and a rumour's reach in a mill grows with time ("Who has heard it").

namespace RumourEngine::Rumours {

    enum class Scope { Spread, Proclaimed, Local };

    struct Subject {
        std::string key;       // SkyrimNet UUID, or "form:XXXXXXXX" if SkyrimNet doesn't know them
        RE::FormID  formId = 0;
        std::string name;
    };

    /** A rumour's stay in one mill. */
    struct Presence {
        double      arrived = 0.0;     // game days; later than now while on its way
        double      decided = 0.0;     // when the hop was decided
        double      freshUntil = 0.0;
        double      notability = 0.0;
        int         hops = 0;
        int         airings = 0;       // tellings in the player's hearing
        std::string parent;            // the mill it came from; empty at the origin
    };

    /** How a mill tells it. */
    struct Version {
        std::string              text;
        std::vector<std::string> names;  // keys of the subjects this version still names
        bool                     fact = false;  // the caller's as_fact version: what this mill knows, not talk
    };

    struct Rumour {
        std::int32_t                    id = 0;
        std::string                     source, key, text;
        nlohmann::json                  origin;
        std::vector<std::string>        topics;
        double                          notability = 0.5;
        double                          secrecy = 0.0;  // 0-1: a mill passes it on only if someone there gossips more readily
        Scope                           scope = Scope::Spread;
        bool                            retell = true;
        std::optional<double>           freshDays;
        std::string                     promote = "auto";
        double                          created = 0.0;
        std::vector<Subject>            subjects;
        std::map<std::string, Presence> mills;
        std::map<std::string, Version>  versions;
        std::map<std::string, std::string> given;  // the caller's versions: "mill:<id>", "hold:<name>" or "*" -> text
        std::set<std::string>              givenFact;  // keys of `given` marked as_fact
        bool                               retellOrigin = false;  // the origin's version is written, not taken as given
        std::vector<Subject>               knownBetterBy;  // people a client gives a fuller account: left out of their talk
        // Kept as world knowledge once it retired (ADR-001, "Lifecycle").
        double                             promoted = -1.0;  // the game day it retired a candidate; <0 if not
        double                             lasting = -1.0;   // the decision model's answer, once asked; <0 before
        std::string                        promotedText;     // the entry, once its settled account is written
        std::int32_t                       knowledgeId = 0;  // SkyrimNet's world-knowledge entry, once made
    };

    /** A hop the simulation decided on: the rumour reaches `mill` from `parent` at `arrives`. */
    struct Arrival {
        std::int32_t id = 0;
        std::string  mill, parent;
        double       decided = 0.0, arrives = 0.0, notability = 0.0;
        int          hops = 0;
    };

    /** The steps one retelling call writes: from the nearest mill with a version, on to `steps`. */
    struct Chain {
        std::int32_t             id = 0;
        std::string              from;      // a mill with a version, or empty to start from the first telling
        std::string              fromText;
        std::vector<std::string> steps;     // in order; the last is the mill that needed one
    };

    /** One rumour as an NPC has heard it. */
    struct Heard {
        std::int32_t id = 0;
        std::string  mill;
        std::string  text;
        double       arrived = 0.0;
        double       notability = 0.0;
        double       secrecy = 0.0;
        int          airings = 0;        // tellings in that mill in the player's hearing
        bool         fact = false;       // the caller's as_fact version
        bool         aboutThem = false;  // they are a subject this version still names
        std::vector<RE::FormID> named;   // subjects this version names, by form
    };

    /** Read the save's live rumours. Call once its database is open and settled. */
    void Load();

    /** Forget everything: a different save is being loaded. */
    void Clear();

    /** Add or update a rumour from an API request (ADR-001, "The AddRumour request"). Any thread.
     *  Returns its id, or 0 with the reason in `error`. */
    std::int32_t Add(const std::string& requestJson, std::string& error);

    bool Retire(std::int32_t id, std::string& error);
    bool RetireByKey(const std::string& source, const std::string& key, std::string& error);

    /** One rumour as JSON, or nullopt if it isn't live. */
    std::optional<nlohmann::json> Describe(std::int32_t id);

    /** The live rumour with this source and key as JSON, or nullopt. */
    std::optional<nlohmann::json> DescribeByKey(const std::string& source, const std::string& key);

    /** Fresh rumours in these mills, best first: about the listener, then their guild's or faction's
     *  telling, then fresher, then more notable, then about people from the same mills. `self` is the listener's reference or base, for "about them".
     *  A rumour whose version there is still unwritten is left out, and its retelling asked for. */
    std::vector<Heard> InMills(const std::vector<std::string>& mills, RE::FormID self, RE::FormID selfBase,
                               double now, std::size_t max);

    /** It has reached them in one of their mills, or they were told it, in earshot of a telling,
     *  or saw it themselves. */
    bool HasHeard(std::int32_t id, RE::FormID formId);

    /** A person's fixed roll for a rumour, in [0, 1): the same every time it is asked. `who` is
     *  Gossip::RollKey. */
    double HeardRoll(std::uint64_t who, std::int32_t id);

    /** The share of a mill's people like this one that a rumour has reached, `days` after it
     *  arrived there: min(1, notability + gossip) x (1 - exp(-rate x days)). Someone has heard
     *  it when their roll is below this. */
    double Reach(double notability, double gossip, double days);

    /** A telling in the player's hearing: logged with everyone in earshot, who have all now heard it. */
    void NoteAiring(std::int32_t id, const std::string& mill, RE::FormID teller, RE::FormID listener,
                    const std::vector<RE::FormID>& earshot, double now);

    /** Someone other than `asker` told this within the item cooldown. */
    bool AiredByOtherRecently(std::int32_t id, RE::FormID asker);

    /** Those who overheard a telling take it home (ADR-001, "Overhearing still counts"): each
     *  carrier's home mill that hasn't got it yet gets it from `mill`, after the travel time. */
    void CarryHome(std::int32_t id, const std::string& mill, const std::vector<RE::FormID>& carriers, double now);

    /** A witness's own memory of a live rumour: fresh again in their mills, and there first-hand
     *  (the first telling) where it hadn't reached. They have certainly heard it. */
    void Reinforce(std::int32_t id, const std::vector<std::string>& mills, RE::FormID witness, double now);

    /** What someone could be talking about: the live rumours in their mills, and those they were in
     *  earshot of, as (id, the version they'd know). For screening's "is this the same talk". */
    std::vector<std::pair<std::int32_t, std::string>> KnownBy(RE::FormID formId, const std::vector<std::string>& mills,
                                                              double now, std::size_t max);

    /** Not retired. */
    bool IsLive(std::int32_t id);

    /** Copies of the live rumours, for the hop simulation. */
    std::vector<Rumour> Snapshot();

    /** Record hops. A mill already holding the rumour, or past max_mills_per_rumour, is skipped.
     *  Where the caller gave a version for the mill, or asked for no retelling, it is stored now. */
    void AddArrivals(const std::vector<Arrival>& arrivals);

    /** Rumours that hopped into a mill in (after, upTo]: (id, mill). */
    std::vector<std::pair<std::int32_t, std::string>> ArrivalsBetween(double after, double upTo);

    /** Live, arrived rumours in these mills that have no version there yet: (id, mill). */
    std::vector<std::pair<std::int32_t, std::string>> Unwritten(const std::vector<std::string>& mills, double now);

    /** What to write so `mill` has a version, at most `maxSteps` steps (the earliest first);
     *  nullopt if it has one, or the rumour isn't there. */
    std::optional<Chain> ChainTo(std::int32_t id, const std::string& mill, std::size_t maxSteps);

    /** Store retold versions: (mill, text). A mill that already has one keeps it. */
    void SetVersions(std::int32_t id, const std::vector<std::pair<std::string, std::string>>& texts, const std::string& model);

    /** Retire rumours that are stale in every mill. Any thread. */
    void Sweep(double now);

    /** Live rumours, for the log. */
    std::size_t Count();

    // ---- Promotion to world knowledge (ADR-001, "Lifecycle") ----
    //
    // A rumour that retires stale, having reached at least promote.min_reach of the mills' people,
    // is a candidate. The decision model is asked whether people will still talk of it in a year
    // (without one, its notability decides); if so, or if its caller said "always", it is kept: its
    // settled account becomes a SkyrimNet world-knowledge entry for those it had reached, keyed by
    // get_rumourengine_heard. Promote.cpp asks, writes and makes the entries.

    /** A candidate, or a promoted rumour still without its world-knowledge entry. */
    struct Promotion {
        std::int32_t id = 0;
        std::string  source, key, text, promote;
        std::string  promotedText;  // empty until its settled account is written
        double       notability = 0.0, created = 0.0;
        double       lasting = -1.0;  // <0 until the decision model is asked
        std::vector<std::pair<std::string, std::string>> versions;  // (place, telling): one per hold, furthest travelled first
        std::vector<std::string> holds;  // the holds it reached, by display name
        std::size_t              allHolds = 0;
    };

    /** The candidates still to be decided, and the promoted rumours with no world-knowledge entry yet. */
    std::vector<Promotion> PromotionsToMake();

    /** The decision on a candidate: kept, or simply retired. */
    void Decide(std::int32_t id, double lasting, bool keep);

    /** Store the entry's text, once written. */
    void SetPromotionText(std::int32_t id, const std::string& text);

    /** Store the world-knowledge entry made for it. */
    void SetKnowledgeId(std::int32_t id, std::int32_t knowledgeId);

    /** Promotion was turned off: forget every entry made, and return them to remove. The texts
     *  stay, so turning it on again puts the same entries back. */
    std::vector<std::int32_t> TakeKnowledgeIds();

    /** The entries of promoted rumours brought back as news since the last call, to remove. */
    std::vector<std::int32_t> TakeWithdrawn();

    /** The promoted rumours this actor has heard, as "|57|132|" for a world-knowledge
     *  condition's contains(); empty if none. Any thread; cached until something it depends on
     *  changes. */
    std::string HeardPromoted(RE::FormID formId);

    /** Promoted rumours, and how many still wait for their entry. */
    std::pair<std::size_t, std::size_t> PromotedCount();

    /** The current game time, in days. */
    double Now();
}
