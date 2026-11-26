#pragma once

#include "PluginScan.h"

#include <map>

// Mills: the groups rumours live in (ADR-001, "Mills"). A port of the stage 0
// builder (tools/mills.py, strategy "merged"): places and strata from the
// location tree and factions, enclaves, institutions, the size floor, and the
// edges between mills. Built once the plugin scan is done, and rebuilt after
// every save load, when the save's enable state decides who exists.

namespace RumourEngine::Mills {

    enum class Kind { Settlement, Commons, Court, Garrison, Underworld, Enclave, Institution };
    // Ears is one-way, from a to b: an informant passing what they hear in a's town to b.
    enum class EdgeKind { Shared, Strata, Road, Carriage, Ship, Hold, Ears };

    const char* KindName(Kind k);
    const char* EdgeKindName(EdgeKind k);

    /** A place on the map: the root worldspace, and x/y in cells. */
    struct Point {
        RE::FormID root = 0;
        double     x = 0.0, y = 0.0;
    };

    struct Mill {
        std::string             id;
        std::string             placeName;  // for prompts: "Whiterun", "High Hrothgar", "the East Empire Company"
        Kind                    kind = Kind::Commons;
        RE::FormID              settlement = 0;  // 0 for institutions
        RE::FormID              hold = 0;
        std::vector<RE::FormID> members;         // person keys, sorted
        std::optional<Point>    pos;
    };

    /** A member. Named NPCs are keyed by their base form, everyone else by reference. */
    struct Person {
        RE::FormID               key = 0;
        RE::FormID               npc = 0;
        std::string              editorID;
        std::string              name;
        std::string              home;  // the place mill they live in
        RE::FormID               location = 0;
        RE::FormID               settlement = 0;
        RE::FormID               hold = 0;
        bool                     unique = false;
        std::vector<std::string> factions;  // editor IDs, for the dump
    };

    struct Edge {
        std::string a, b;
        EdgeKind    kind = EdgeKind::Shared;
        double      dist = 0.0;  // cells
        double      weight = 1.0;
    };

    /** What a location is, for placing rumours and the rules on who may be told. */
    enum LocationFlag : std::uint32_t {
        kSettlement = 1 << 0,
        kHold = 1 << 1,
        kInn = 1 << 2,
        kCastle = 1 << 3,
        kBarracks = 1 << 4,
    };

    /** A named NPC whose whereabouts are watched for a move (ADR-001, "Observed homes"). */
    struct Watched {
        RE::FormID ref = 0;   // their persistent reference: what quests and follower mods move
        RE::FormID base = 0;  // the person's key
        Kind       kind = Kind::Commons;  // their stratum, from their factions, for where they'd settle
        bool       byPlace = true;        // no faction says: a castle makes them court, a barracks garrison
        bool       placed = false;        // the builder gave them a home
    };

    struct MillSet {
        std::vector<Mill>                                         mills;  // sorted by id
        std::unordered_map<std::string, std::size_t>              byId;
        std::unordered_map<RE::FormID, Person>                    persons;
        std::unordered_map<RE::FormID, std::vector<std::string>>  millsOf;  // home first
        std::unordered_map<RE::FormID, RE::FormID>                baseOfRef; // placed references -> base
        std::vector<Edge>                                         edges;
        std::unordered_map<RE::FormID, std::pair<RE::FormID, std::uint32_t>> locations;    // parent, LocationFlag bits
        std::unordered_map<RE::FormID, std::map<Kind, std::string>>        placeMills;   // settlement -> its mills, after merging
        std::unordered_map<RE::FormID, std::string>                         locationHome; // where most people living there belong
        std::unordered_map<RE::FormID, std::string>                         holdCapital;  // a hold's biggest place mill
        std::unordered_map<RE::FormID, std::string>                         holdNames;    // hold -> display name
        std::unordered_map<RE::FormID, std::string>                         settlementNames; // settlement -> display name
        std::unordered_map<std::uint64_t, RE::FormID>                       exteriorLocations; // (worldspace, cell grid) -> that cell's location
        std::vector<Watched>                                      watch;  // named NPCs with a persistent reference
        std::unordered_map<std::string, double>                   secrecy;  // mills.ini [Secrecy]: how close a mill keeps what it hears
        std::unordered_map<std::string, std::string>              lines;    // mills.ini [Lines]: each mill described to the retell prompt
        std::vector<std::string>                                  eager;    // mills.ini [Retell]: written as soon as a rumour arrives
        std::vector<RE::FormID>                                   partyFactions;  // mills.ini [Party]: at rank 0 or more, following the player
        std::map<std::string, int>                                dropped;
        std::map<std::string, int>                                sources;
        std::string                                               reason;
        double                                                    milliseconds = 0.0;

        const Mill* Find(std::string_view id) const;

        /** How close a mill keeps what it hears (mills.ini [Secrecy]); 0 for most. */
        double SecrecyOf(std::string_view id) const;

        /** Its versions are written as soon as a rumour arrives (mills.ini [Retell] eager), not
         *  when someone there first needs one. */
        bool IsEager(std::string_view id) const;

        /** The mills of a person, by reference or base form ID; empty if none. A stored
         *  placement (SetPlacement) replaces their place mill; institutions stay. */
        std::vector<std::string> MillsFor(RE::FormID formId) const;
        std::vector<std::string> MillsFor(const RE::Actor* actor) const;

        /** The mill someone would belong to if they lived at `location`, by the builder's rules:
         *  their settlement's mill of their stratum, or, where no faction decides (`byPlace`), a
         *  castle's court or a barracks' garrison, else its commons. Empty on the road, in the
         *  wilds and in dungeons. */
        std::string SettleAt(RE::FormID location, Kind kind, bool byPlace) const;
        std::string SettleAt(RE::FormID location, const Watched& who) const { return SettleAt(location, who.kind, who.byPlace); }

        /** The location of the exterior cell at a spot, as the plugins set it, else the worldspace's
         *  own (a city's); 0 if none. Out of sight outdoors, the game keeps someone in no location,
         *  but they still stand in a cell. */
        RE::FormID LocationAt(RE::FormID world, const RE::NiPoint3& pos) const;

        /** In the wilderness, where cells carry no location: the location of the nearest exterior
         *  cell that has one, up to `cells` cells away; 0 if none. */
        RE::FormID LocationNear(RE::FormID world, const RE::NiPoint3& pos, int cells) const;

        /** The place mill the builder gave this person, before any stored placement; empty if none. */
        std::string BuiltHome(RE::FormID key) const;

        /** The mill a location's talk belongs to: its settlement's, by stratum (a castle's
         *  court, a barracks' garrison), else where its residents belong, else its hold's
         *  biggest. Empty if none. */
        std::vector<std::string> MillsForLocation(RE::FormID location) const;

        /** Every mill of the settlement or enclave a location is in, for retelling ahead of the
         *  player. Empty on the road, in the wilds and in dungeons. */
        std::vector<std::string> PlaceMillsAt(RE::FormID location) const;

        /** Every mill in a hold, by its name ("Whiterun", "Whiterun Hold" or "whiterunhold"). */
        std::vector<std::string> MillsInHold(std::string_view hold) const;

        /** Every mill of a settlement, by its name ("Solitude") or its id in mill ids ("solitude"):
         *  its commons, court, garrison and underworld, or the mill it was folded into. `matches`
         *  is how many settlements have that name; empty unless exactly one. */
        std::vector<std::string> MillsInSettlement(std::string_view name, int* matches = nullptr) const;

        /** The location or one of its parents has any of these flags. */
        bool LocationIs(RE::FormID location, std::uint32_t flags) const;

        /** The outermost hold-keyworded ancestor. */
        RE::FormID HoldOf(RE::FormID location) const;
    };

    /** Hand over the plugin scan. Builds need it. */
    void SetScan(std::shared_ptr<const PluginScan::Result> scan);
    bool HasScan();

    /** Build from the scan and the game's current state, and make it current. Main thread. */
    std::shared_ptr<const MillSet> Build(const char* reason);

    /** The current mills, or null before the first build. Any thread. */
    std::shared_ptr<const MillSet> Current();

    /** Where a person has been seen to settle, replacing the place mill the builder gave them
     *  (ADR-001, "Dismissed followers" and "Observed homes"). Keyed by their base form. Any thread. */
    void SetPlacement(RE::FormID base, const std::string& mill);
    void ClearPlacement(RE::FormID base);
    void ClearPlacements();

    /** Counts changes to the placements, so what's built from them knows when to rebuild. Any thread. */
    std::uint64_t PlacementsVersion();

    /** Name of a settlement or hold for logs and JSON: its slug, as in mill ids. */
    std::string PlaceName(RE::FormID location);

    /** Summary to the log; with `full`, every mill and its first members too. */
    void Log(const MillSet& set, bool full);

    /** The whole set as JSON, in the shape tools/mills.py exports, for comparing the two. */
    void WriteJson(const MillSet& set, const std::filesystem::path& path);
}
