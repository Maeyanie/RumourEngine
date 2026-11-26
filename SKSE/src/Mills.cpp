#include "Mills.h"
#include "Ini.h"
#include "Settings.h"
#include "Text.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <regex>
#include <set>
#include <shared_mutex>
#include <tuple>

namespace RumourEngine::Mills {

    namespace {

        // ---- Location keywords, as bits (tools/mills.py has the same sets) ----

        enum : std::uint32_t {
            kwHold = 1 << 0,
            kwSettlement = 1 << 1,
            kwCivil = 1 << 2,
            kwHostile = 1 << 3,
            kwCity = 1 << 5,
            kwBarracks = 1 << 6,
            kwCastle = 1 << 7,
            kwHasInn = 1 << 8,
            kwInn = 1 << 9,
        };

        const std::unordered_map<std::string_view, std::uint32_t>& KeywordBits() {
            static const std::unordered_map<std::string_view, std::uint32_t> bits = [] {
                std::unordered_map<std::string_view, std::uint32_t> m;
                for (auto k : { "LocTypeHold"sv, "LocTypeHoldMajor"sv, "LocTypeHoldMinor"sv }) m[k] |= kwHold;
                for (auto k : { "LocTypeHabitation"sv, "LocTypeHabitationHasInn"sv, "LocTypeCity"sv, "LocTypeTown"sv,
                                "LocTypeSettlement"sv, "LocTypeHoldCapital"sv, "LocTypeOrcStronghold"sv })
                    m[k] |= kwSettlement;
                for (auto k : { "LocTypeDwelling"sv, "LocTypeHouse"sv, "LocTypeInn"sv, "LocTypeStore"sv,
                                "LocTypeGuild"sv, "LocTypeTemple"sv, "LocTypeCastle"sv, "LocTypeBarracks"sv,
                                "LocTypeStewardsDwelling"sv, "LocTypePlayerHouse"sv, "LocTypeFarm"sv,
                                "LocTypeLumberMill"sv, "LocTypeMine"sv, "LocTypeJail"sv, "LocTypeShip"sv,
                                "BYOH_LocTypeHomestead"sv })
                    m[k] |= kwCivil;
                for (auto k : { "LocTypeBanditCamp"sv, "LocTypeForswornCamp"sv, "LocTypeVampireLair"sv,
                                "LocTypeWarlockLair"sv, "LocTypeDungeon"sv, "LocTypeDraugrCrypt"sv,
                                "LocTypeDragonLair"sv, "LocTypeGiantCamp"sv, "LocTypeHagravenNest"sv,
                                "LocTypeAnimalDen"sv, "LocTypeFalmerHive"sv, "LocTypeDwarvenAutomatons"sv,
                                "LocTypeSprigganGrove"sv, "LocTypeMilitaryCamp"sv, "LocTypeMilitaryFort"sv })
                    m[k] |= kwHostile;
                m["LocTypeCity"sv] |= kwCity;
                m["LocTypeBarracks"sv] |= kwBarracks;
                m["LocTypeCastle"sv] |= kwCastle;
                m["LocTypeHabitationHasInn"sv] |= kwHasInn;
                m["LocTypeInn"sv] |= kwInn;
                return m;
            }();
            return bits;
        }

        // ---- Factions, as bits ----

        enum : std::uint32_t {
            fHostile = 1 << 0,
            fCourt = 1 << 1,
            fGuard = 1 << 2,
            fUnderworld = 1 << 3,
            fGuild = 1 << 4,
            fCarriage = 1 << 5,
            fTraveller = 1 << 6,     // a caravan: a travel link, never a resident
            fInstitution0 = 1 << 8,  // one bit per entry of kInstitutions
        };

        struct Institution {
            const char*                   id;
            const char*                   name;
            std::vector<std::string_view> factions;
        };

        const std::vector<Institution> kInstitutions = {
            { "companions", "the Companions", { "CompanionsFaction" } },
            { "college", "the College of Winterhold", { "CollegeofWinterholdFaction" } },
            { "thievesguild", "the Thieves Guild", { "ThievesGuildFaction" } },
            { "darkbrotherhood", "the Dark Brotherhood", { "DarkBrotherhoodFaction" } },
            { "bardscollege", "the Bards College", { "SolitudeBardsCollegeFaction" } },
            { "eastempire", "the East Empire Company",
              { "TG04EastEmpireFaction", "ServicesSolitudeEastEmpireCompany", "WindhelmEastEmpireFaction" } },
            { "dawnguard", "the Dawnguard", { "DLC1DawnguardFaction" } },
        };

        std::uint32_t FactionBits(std::string_view edid) {
            static const std::unordered_map<std::string_view, std::uint32_t> bits = [] {
                std::unordered_map<std::string_view, std::uint32_t> m;
                for (auto k : { "BanditFaction"sv, "ForswornFaction"sv, "VampireFaction"sv, "NecromancerFaction"sv,
                                "WarlockFaction"sv, "HagravenFaction"sv, "DragonPriestFaction"sv, "DLC1VampireFaction"sv,
                                "DLC2BanditDialogueFaction"sv, "SilverHandFaction"sv, "DLC1UndeadGuardianFaction"sv })
                    m[k] |= fHostile;
                for (auto k : { "JobJarlFaction"sv, "JobStewardFaction"sv, "JobHousecarlFaction"sv, "JobCourtWizardFaction"sv })
                    m[k] |= fCourt;
                for (auto k : { "IsGuardFaction"sv, "JobGuardCaptainFaction"sv, "DragonsreachBasementGuards"sv })
                    m[k] |= fGuard;
                for (auto k : { "ThievesGuildFaction"sv, "RiftenRaggedFlagonFaction"sv, "JobFenceFaction"sv,
                                "TownCidhnaMinePrisonerFaction"sv })
                    m[k] |= fUnderworld;
                // Guild members sometimes carry guard factions; they belong with the townsfolk.
                for (auto k : { "CompanionsFaction"sv, "CollegeofWinterholdFaction"sv, "SolitudeBardsCollegeFaction"sv })
                    m[k] |= fGuild;
                m["CarriageSystemFaction"sv] |= fCarriage;
                for (std::size_t i = 0; i < kInstitutions.size(); ++i) {
                    for (auto f : kInstitutions[i].factions) m[f] |= fInstitution0 << i;
                }
                return m;
            }();
            std::uint32_t out = 0;
            if (auto it = bits.find(edid); it != bits.end()) out = it->second;
            if (edid.starts_with("GuardFaction")) out |= fGuard;
            // Vanilla's Khajiit caravans, and mods' (Trade Routes: CaravanALeader, ...).
            if (edid.find("Caravan") != std::string_view::npos) out |= fTraveller;
            return out;
        }

        // Quest copies, corpses, props and battle extras rather than people who live there,
        // and the crowd for Windhelm's fighting pit, which was cut from the game.
        const std::regex kNotAResident(
            R"(Dead|Corpse|Captive|Mannequin|Decoy|TortureVictim|Ghost|^CW|^MQ\d|^DA\d\d|Sentry|^dun|^BYOHHouse\dBard|^BYOHCarriageDriver\d|^WindhelmPitFan)");

        // Non-hostile places outside any settlement that hold a community of their own.
        constexpr int kEnclaveMin = 4;
        const std::unordered_set<std::string_view> kNotEnclaves = {
            "PersistAll", "HelgenLocation", "SovngardeHallofHeroesLocation", "SovngardeLocation", "HoldingCell",
            "DLC1SoulCairnLocation", "ThroatoftheWorldLocation", "DLC1FalmerValleyLocation", "DLC2DremoraShopLocation"
        };

        // The ship to Solstheim, which geography alone wouldn't find. Others go in mills.ini [Links].
        const std::vector<std::pair<std::string_view, std::string_view>> kShipLinks = { { "windhelm", "ravenrock" } };

        // mills.ini [Factions]: "id = Name: FactionEditorID, ...". A mill of the named members of
        // those factions, wherever they live, for groups whose talk has its own slant but who don't
        // live together (the Legion's officers, the Black-Briars). Never made automatically.
        struct FactionMill {
            std::string              id, name;
            std::vector<std::string> factions;
        };

        using Ini::Lower;
        using Ini::Trim;

        std::vector<FactionMill> LoadFactionMills(const Ini::Files& ini) {
            std::vector<FactionMill> out;
            for (const auto& e : Ini::Keyed(ini, "factions")) {
                FactionMill      fm;
                fm.id = e.key;
                std::string_view rest = e.value;
                if (const auto colon = rest.find(':'); colon != std::string_view::npos) {
                    fm.name = Trim(rest.substr(0, colon));
                    rest = rest.substr(colon + 1);
                }
                while (!rest.empty()) {
                    const auto comma = rest.find(',');
                    if (auto e = Trim(rest.substr(0, comma)); !e.empty()) fm.factions.push_back(e);
                    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
                }
                if (fm.name.empty()) fm.name = fm.id;
                if (!fm.id.empty() && !fm.factions.empty()) out.push_back(std::move(fm));
            }
            return out;
        }

        // mills.ini [Links]: "a = b: kind, weight". A link between two mills that the map doesn't
        // give, or one it gives made different: road, carriage, ship or hold, the weight optional.
        // "ears" is one-way: a hears what is said in b. "none" takes the builder's link away.
        // Mills linked by shared members can't be linked here.
        // In a patch, a later line for the same two mills replaces an earlier one.
        struct Link {
            std::string           a, b, kind, file;
            std::optional<double> weight;
        };

        std::vector<Link> LoadLinks(const Ini::Files& ini) {
            std::vector<Link> out;
            for (const auto& [a, value, file] : Ini::Lines(ini, "links")) {
                Link             l;
                l.a = a;
                l.file = file;
                std::string_view rest = value;
                const auto       colon = rest.find(':');
                if (colon == std::string_view::npos) {
                    logger::warn("Mills: {}: link {} = {} needs a kind after a colon", file, a, value);
                    continue;
                }
                l.b = Lower(Trim(rest.substr(0, colon)));
                rest = rest.substr(colon + 1);
                const auto comma = rest.find(',');
                l.kind = Lower(Trim(rest.substr(0, comma)));
                if (comma != std::string_view::npos) {
                    const auto w = Trim(rest.substr(comma + 1));
                    double     v = 0.0;
                    auto [p, ec] = std::from_chars(w.data(), w.data() + w.size(), v);
                    if (ec == std::errc{} && v >= 0.0) l.weight = v;
                    else logger::warn("Mills: {}: link {} = {}: weight {} isn't a number", file, a, value, w);
                }
                out.push_back(std::move(l));
            }
            return out;
        }

        // mills.ini [Secrecy]: "id = 0.8". A mill passes nothing on unless someone in it gossips
        // more readily than this, so what it hears stays with it.
        std::vector<std::pair<std::string, double>> LoadMillSecrecy(const Ini::Files& ini) {
            std::vector<std::pair<std::string, double>> out;
            for (const auto& [id, value, file] : Ini::Keyed(ini, "secrecy")) {
                double v = 0.0;
                auto [p, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
                if (ec == std::errc{} && v >= 0.0 && v <= 1.0) out.emplace_back(id, v);
                else logger::warn("Mills: {}: secrecy {} = {} isn't a number from 0 to 1", file, id, value);
            }
            return out;
        }

        // mills.ini [Retell]: "id = eager". A mill whose versions are written as soon as a rumour
        // arrives, for a mod that reads them out in prompts of its own (FactionEngine's leaders),
        // rather than when someone there first talks. "lazy" undoes an earlier file's line.
        std::vector<std::pair<std::string, std::string>> LoadEager(const Ini::Files& ini) {
            std::vector<std::pair<std::string, std::string>> out;  // id, file
            for (const auto& [id, value, file] : Ini::Keyed(ini, "retell")) {
                const auto v = Lower(value);
                if (v == "eager") out.emplace_back(id, file);
                else if (v != "lazy") logger::warn("Mills: {}: retell {} = {} should be eager or lazy", file, id, value);
            }
            return out;
        }

        // mills.ini [Party]: "mod = FactionEditorID, ...". Followers in one of these factions at
        // rank 0 or more are with the player, for follower mods that keep a faction of their own
        // instead of CurrentFollowerFaction and don't keep the teammate flag set throughout (Sofia
        // clears it while she idles near a player who has stopped). A faction whose plugin isn't
        // loaded is passed over quietly, so the shipped lines cost nothing without their mods.
        void LoadPartyFactions(const Ini::Files& ini, const PluginScan::Result& scan, MillSet& out) {
            std::unordered_map<std::string, RE::FormID> byEditorID;
            for (const auto& [id, edid] : scan.factionEditorIDs) byEditorID.emplace(Lower(edid), id);
            std::string found, absent;
            for (const auto& [key, value, file] : Ini::Keyed(ini, "party")) {
                std::string_view rest = value;
                while (!rest.empty()) {
                    const auto comma = rest.find(',');
                    if (const auto name = Trim(rest.substr(0, comma)); !name.empty()) {
                        if (auto it = byEditorID.find(Lower(name)); it != byEditorID.end()) {
                            if (std::find(out.partyFactions.begin(), out.partyFactions.end(), it->second) == out.partyFactions.end()) {
                                out.partyFactions.push_back(it->second);
                            }
                            found += fmt::format("{}{} ({}, {:08X})", found.empty() ? "" : ", ", name, key, it->second);
                        } else {
                            absent += fmt::format("{}{} ({})", absent.empty() ? "" : ", ", name, key);
                        }
                    }
                    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
                }
            }
            if (!found.empty()) logger::info("Mills: following the player, as well as by the game's own signs, while in: {}", found);
            if (!absent.empty()) logger::info("Mills: party factions not in the load order: {}", absent);
        }

        constexpr double kCellUnits = 4096.0;
        constexpr RE::FormID kPlayerBase = 0x7;

        std::string Slug(std::string_view edid) {
            std::string s(edid);
            for (auto suffix : { "Location"sv, "Loc"sv }) {
                if (s.ends_with(suffix)) {
                    s.resize(s.size() - suffix.size());
                    break;
                }
            }
            for (auto prefix : { "DLC2"sv, "DLC1"sv, "BYOH"sv }) {
                if (s.starts_with(prefix)) {
                    s.erase(0, prefix.size());
                    break;
                }
            }
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        double Dist(const Point& a, const Point& b) {
            if (a.root != b.root) return 1e9;
            return std::hypot(a.x - b.x, a.y - b.y);
        }

        std::mutex                                g_lock;
        std::shared_ptr<const PluginScan::Result> g_scan;
        std::shared_ptr<const MillSet>            g_current;
        std::unordered_map<RE::FormID, std::string> g_placeNames;

        std::shared_mutex                           g_placementLock;
        std::unordered_map<RE::FormID, std::string> g_placements;  // base -> the mill they settled in
        std::atomic<std::uint64_t>                  g_placementsVersion = 0;

        /** The game data a build reads, gathered once per build. */
        class World {
        public:
            explicit World(const PluginScan::Result& scan) : scan_(scan) {
                LoadLocations();
                LoadCellGrid();
            }

            struct Loc {
                const RE::BGSLocation* form = nullptr;
                RE::FormID             parent = 0;
                std::uint32_t          kw = 0;
                std::string            edid;
            };

            const PluginScan::Result&                scan_;
            std::unordered_map<RE::FormID, Loc>      locs;
            std::unordered_map<std::uint64_t, RE::FormID> cellByGrid;
            std::unordered_map<RE::FormID, RE::FormID>    worldRoot;
            std::unordered_map<RE::FormID, std::uint32_t> factionBits;

            std::uint32_t Kw(RE::FormID loc) const {
                auto it = locs.find(loc);
                return it != locs.end() ? it->second.kw : 0;
            }

            const std::string& Edid(RE::FormID loc) const {
                static const std::string none = "?";
                auto it = locs.find(loc);
                return it != locs.end() ? it->second.edid : none;
            }

            /** loc and its parents, nearest first. */
            std::vector<RE::FormID> Ancestors(RE::FormID loc) const {
                std::vector<RE::FormID> out;
                while (loc && locs.contains(loc) && std::find(out.begin(), out.end(), loc) == out.end()) {
                    out.push_back(loc);
                    loc = locs.at(loc).parent;
                }
                return out;
            }

            RE::FormID TierOf(RE::FormID loc, std::uint32_t kws) const {
                for (auto a : Ancestors(loc)) {
                    if (Kw(a) & kws) return a;
                }
                return 0;
            }

            bool Hostile(RE::FormID loc) const { return TierOf(loc, kwHostile) != 0; }

            /** The hold a location is in: its outermost ancestor with a hold keyword.
             *  Not the nearest: keyword distributors can copy a hold's keyword down to
             *  everything inside it (OCF gives LocTypeHoldMajor to all of Eastmarch and
             *  Haafingar this way), and holds don't nest. */
            RE::FormID HoldOf(RE::FormID loc) const {
                RE::FormID out = 0;
                for (auto a : Ancestors(loc)) {
                    if (Kw(a) & kwHold) out = a;
                }
                return out;
            }

            RE::FormID WorldRoot(RE::FormID world) {
                if (auto it = worldRoot.find(world); it != worldRoot.end()) return it->second;
                auto* ws = RE::TESForm::LookupByID<RE::TESWorldSpace>(world);
                RE::FormID root = world;
                for (int i = 0; ws && ws->parentWorld && i < 16; ++i) {
                    ws = ws->parentWorld;
                    root = ws->GetFormID();
                }
                worldRoot[world] = root;
                return root;
            }

            /** The ancestor directly below the hold (or below the root): where someone lives, outside settlements. */
            RE::FormID PlaceUnderHold(RE::FormID loc) const {
                auto chain = Ancestors(loc);
                for (std::size_t i = chain.size(); i-- > 0;) {
                    if (Kw(chain[i]) & kwHold) return chain[i > 0 ? i - 1 : 0];
                }
                return chain.size() > 1 ? chain[chain.size() - 2] : chain.empty() ? loc : chain[0];
            }

            std::uint32_t Factions(RE::FormID faction) {
                if (auto it = factionBits.find(faction); it != factionBits.end()) return it->second;
                auto e = scan_.factionEditorIDs.find(faction);
                const auto bits = e != scan_.factionEditorIDs.end() ? FactionBits(e->second) : 0;
                factionBits[faction] = bits;
                return bits;
            }

            /** Follow the template chain while the NPC takes `flag` data from its template. */
            static const RE::TESNPC* Resolve(const RE::TESNPC* npc, RE::ACTOR_BASE_DATA::TEMPLATE_USE_FLAG flag) {
                for (int depth = 0; npc && depth < 8; ++depth) {
                    if (!npc->actorData.templateUseFlags.any(flag) || !npc->baseTemplateForm) break;
                    const RE::TESForm* tpl = npc->baseTemplateForm;
                    // Leveled lists can nest (Requiem's do): take the first entry down to an NPC.
                    for (int i = 0; tpl && i < 8; ++i) {
                        auto* lvl = tpl->As<RE::TESLevCharacter>();
                        if (!lvl) break;
                        tpl = lvl->numEntries > 0 ? lvl->entries[0].form : nullptr;
                    }
                    npc = tpl ? tpl->As<RE::TESNPC>() : nullptr;
                }
                return npc;
            }

            static bool IsPerson(const RE::TESNPC* npc) {
                if (!npc || npc->IsPreset()) return false;
                auto* t = Resolve(npc, RE::ACTOR_BASE_DATA::TEMPLATE_USE_FLAG::kTraits);
                auto* race = t ? t->race : nullptr;
                return race && race->HasKeywordString("ActorTypeNPC");
            }

            std::uint32_t FactionMask(const RE::TESNPC* npc, std::vector<std::string>* names) {
                auto*         n = Resolve(npc, RE::ACTOR_BASE_DATA::TEMPLATE_USE_FLAG::kFactions);
                std::uint32_t out = 0;
                if (!n) return out;
                for (const auto& fr : n->factions) {
                    if (!fr.faction || fr.rank < 0) continue;
                    out |= Factions(fr.faction->GetFormID());
                    if (names) {
                        auto e = scan_.factionEditorIDs.find(fr.faction->GetFormID());
                        if (e != scan_.factionEditorIDs.end()) names->push_back(e->second);
                    }
                }
                if (names) {
                    std::sort(names->begin(), names->end());
                    names->erase(std::unique(names->begin(), names->end()), names->end());
                }
                return out;
            }

            /** Disabled now: by its own state or through its enable parents. */
            bool Disabled(RE::FormID ref, const PluginScan::Placement& a) const {
                auto* live = RE::TESForm::LookupByID<RE::TESObjectREFR>(ref);
                if (live ? live->IsDisabled() : a.disabled) return true;
                return a.enableParent && ParentDisabled(a.enableParent, 0) != a.opposite;
            }

            bool ParentDisabled(RE::FormID id, int depth) const {
                if (depth > 6) return false;
                auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
                if (!ref) {
                    auto a = scan_.actors.find(id);
                    return a != scan_.actors.end() && Disabled(id, a->second);
                }
                if (ref->IsDisabled()) return true;
                auto* esp = ref->extraList.GetByType<RE::ExtraEnableStateParent>();
                if (!esp) return false;
                auto parent = esp->parent.get();
                if (!parent) return false;
                return ParentDisabled(parent->GetFormID(), depth + 1) != ((esp->flags & 0x01) != 0);
            }

            RE::FormID RefLocation(const PluginScan::Placement& a) const {
                if (a.location) return a.location;
                auto c = scan_.cells.find(a.cell);
                if (c == scan_.cells.end()) return 0;
                if (c->second.location) return c->second.location;
                if (c->second.world) {
                    const auto gx = static_cast<std::int32_t>(std::floor(a.pos[0] / kCellUnits));
                    const auto gy = static_cast<std::int32_t>(std::floor(a.pos[1] / kCellUnits));
                    if (auto e = cellByGrid.find(GridKey(c->second.world, gx, gy)); e != cellByGrid.end()) {
                        if (auto ext = scan_.cells.find(e->second); ext != scan_.cells.end() && ext->second.location) {
                            return ext->second.location;
                        }
                    }
                    // A city worldspace names its own location (Riften's is Riften).
                    if (auto* ws = RE::TESForm::LookupByID<RE::TESWorldSpace>(c->second.world); ws && ws->location) {
                        return ws->location->GetFormID();
                    }
                }
                return 0;
            }

            static std::uint64_t GridKey(RE::FormID world, std::int32_t x, std::int32_t y) {
                return (std::uint64_t(world) << 32) | (std::uint64_t(std::uint16_t(x)) << 16) | std::uint16_t(y);
            }

        private:
            void LoadLocations() {
                const auto& bits = KeywordBits();
                for (auto* loc : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::BGSLocation>()) {
                    if (!loc) continue;
                    Loc l;
                    l.form = loc;
                    l.parent = loc->parentLoc ? loc->parentLoc->GetFormID() : 0;
                    for (auto* kw : loc->GetKeywords()) {
                        if (!kw) continue;
                        if (auto it = bits.find(kw->GetFormEditorID()); it != bits.end()) l.kw |= it->second;
                    }
                    auto e = scan_.locationEditorIDs.find(loc->GetFormID());
                    l.edid = e != scan_.locationEditorIDs.end() ? e->second : fmt::format("{:08X}", loc->GetFormID());
                    locs.emplace(loc->GetFormID(), std::move(l));
                }
            }

            void LoadCellGrid() {
                for (const auto& [id, c] : scan_.cells) {
                    if (c.world && c.hasGrid) cellByGrid[GridKey(c.world, c.x, c.y)] = id;
                }
            }
        };

        struct Resident {
            RE::FormID key, npc, loc;
        };

        /** Mean exterior cell centre per location, counting its descendants' cells; else its map marker. */
        std::unordered_map<RE::FormID, Point> LocationPositions(World& W) {
            struct Acc {
                std::unordered_map<RE::FormID, int> roots;
                std::vector<Point>                  pts;
            };
            std::unordered_map<RE::FormID, Acc> acc;
            for (const auto& [id, c] : W.scan_.cells) {
                if (c.interior || !c.hasGrid || !c.location || !c.world) continue;
                Point      pt{ W.WorldRoot(c.world), c.x + 0.5, c.y + 0.5 };
                const auto hold = W.HoldOf(c.location);
                for (auto a : W.Ancestors(c.location)) {
                    if (a == hold) break;
                    acc[a].roots[pt.root]++;
                    acc[a].pts.push_back(pt);
                }
            }
            std::unordered_map<RE::FormID, Point> pos;
            for (auto& [loc, a] : acc) {
                auto best = std::max_element(a.roots.begin(), a.roots.end(),
                                             [](auto& l, auto& r) { return l.second < r.second; });
                Point  p{ best->first, 0.0, 0.0 };
                double n = 0;
                for (auto& q : a.pts) {
                    if (q.root != p.root) continue;
                    p.x += q.x;
                    p.y += q.y;
                    ++n;
                }
                p.x /= n;
                p.y /= n;
                pos[loc] = p;
            }
            for (const auto& [loc, marker] : W.scan_.locationMarkers) {
                if (pos.contains(loc) || !W.locs.contains(loc)) continue;
                auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(marker);
                auto* ws = ref ? ref->GetWorldspace() : nullptr;
                if (!ws) continue;
                const auto at = ref->GetPosition();
                pos[loc] = Point{ W.WorldRoot(ws->GetFormID()), at.x / kCellUnits, at.y / kCellUnits };
            }
            return pos;
        }

        /** Mill ids in the order they were first made, as the stage 0 builder kept them. */
        class MillTable {
        public:
            struct Info {
                Kind       kind = Kind::Commons;
                RE::FormID settlement = 0;
                RE::FormID hold = 0;
            };

            bool Has(const std::string& id) const { return index_.contains(id); }
            std::set<RE::FormID>& Members(const std::string& id) { return members_.at(index_.at(id)); }
            Info& InfoOf(const std::string& id) { return info_.at(index_.at(id)); }

            std::set<RE::FormID>& Ensure(const std::string& id, const Info& info) {
                if (auto it = index_.find(id); it != index_.end()) return members_[it->second];
                index_[id] = ids_.size();
                ids_.push_back(id);
                members_.emplace_back();
                info_.push_back(info);
                alive_.push_back(true);
                return members_.back();
            }

            void Remove(const std::string& id) {
                auto it = index_.find(id);
                if (it == index_.end()) return;
                alive_[it->second] = false;
                index_.erase(it);
            }

            /** Live ids, in creation order. */
            std::vector<std::string> Ids() const {
                std::vector<std::string> out;
                for (std::size_t i = 0; i < ids_.size(); ++i) {
                    if (alive_[i]) out.push_back(ids_[i]);
                }
                return out;
            }

        private:
            std::vector<std::string>                     ids_;
            std::vector<std::set<RE::FormID>>            members_;
            std::vector<Info>                            info_;
            std::vector<bool>                            alive_;
            std::unordered_map<std::string, std::size_t> index_;
        };

        std::shared_ptr<MillSet> BuildFrom(const PluginScan::Result& scan) {
            World W(scan);
            auto  out = std::make_shared<MillSet>();

            const auto ini = Ini::ReadMills();
            {
                std::string names;
                for (const auto& n : ini.names) names += (names.empty() ? "" : ", ") + n;
                logger::info("Mills: read {}", names.empty() ? "no mills.ini" : names);
            }

            // ---- Who lives where ----

            // Location rows naming a unique NPC (LCUN/ACUN), from the scan. The engine
            // merges them across files; the set keeps each NPC's rows in (ref, location) order.
            struct Row {
                RE::FormID ref, loc;
            };
            std::unordered_map<RE::FormID, std::vector<Row>> uniqueRows;
            for (const auto& r : scan.uniqueRows) {
                if (r.npc) uniqueRows[r.npc].push_back({ r.ref, r.location });
            }
            int copiedHold = 0;
            for (const auto& [id, l] : W.locs) {
                if ((l.kw & kwHold) && W.HoldOf(id) != id) ++copiedHold;
            }
            if (copiedHold) out->sources["locations with a hold keyword inside a hold"] = copiedHold;

            std::unordered_map<RE::FormID, std::vector<RE::FormID>> refsByBase;
            for (const auto& [ref, a] : scan.actors) {
                refsByBase[a.base].push_back(ref);
                out->baseOfRef[ref] = a.base;
            }
            for (auto& [base, refs] : refsByBase) std::sort(refs.begin(), refs.end());

            std::vector<const RE::TESNPC*> npcs;
            for (auto* npc : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESNPC>()) {
                if (npc && npc->GetFormID() != kPlayerBase) npcs.push_back(npc);
            }
            std::sort(npcs.begin(), npcs.end(), [](auto* a, auto* b) { return a->GetFormID() < b->GetFormID(); });

            std::vector<Resident> people;
            for (const auto* npc : npcs) {
                if (!World::IsPerson(npc)) continue;
                const RE::FormID id = npc->GetFormID();
                auto             e = scan.npcEditorIDs.find(id);
                if (e != scan.npcEditorIDs.end() && std::regex_search(e->second, kNotAResident)) {
                    out->sources["skipped by name"]++;
                    continue;
                }
                const auto refsIt = refsByBase.find(id);
                if (npc->IsUnique()) {
                    if (auto rows = uniqueRows.find(id); rows != uniqueRows.end()) {
                        const Row* pick = &rows->second.front();
                        for (const auto& r : rows->second) {
                            auto a = scan.actors.find(r.ref);
                            if (a == scan.actors.end() || !W.Disabled(r.ref, a->second)) {
                                pick = &r;
                                break;
                            }
                        }
                        people.push_back({ id, id, pick->loc });
                        out->sources["unique via location row"]++;
                        continue;
                    }
                    if (refsIt != refsByBase.end()) {
                        RE::FormID pick = refsIt->second.front();
                        for (auto r : refsIt->second) {
                            if (!W.Disabled(r, scan.actors.at(r))) {
                                pick = r;
                                break;
                            }
                        }
                        people.push_back({ id, id, W.RefLocation(scan.actors.at(pick)) });
                        out->sources["unique via placement"]++;
                    }
                    continue;
                }
                if (refsIt == refsByBase.end()) continue;
                for (auto r : refsIt->second) {
                    if (W.Disabled(r, scan.actors.at(r))) {
                        out->sources["generic, disabled"]++;
                        continue;
                    }
                    people.push_back({ r, id, W.RefLocation(scan.actors.at(r)) });
                    out->sources["generic placements"]++;
                }
            }

            const auto lpos = LocationPositions(W);

            // ---- Where each person belongs ----

            struct Pending {
                Person        p;
                std::uint32_t fac = 0;
                bool          enclave = false;
            };
            std::vector<Pending>                pending;
            std::unordered_map<RE::FormID, int> enclaveCount;

            const auto                      factionMills = LoadFactionMills(ini);
            std::unordered_set<std::string> listedFactions;
            for (const auto& fm : factionMills) listedFactions.insert(fm.factions.begin(), fm.factions.end());
            auto inAny = [](const std::vector<std::string>& have, const std::vector<std::string>& want) {
                return std::any_of(want.begin(), want.end(), [&](auto& w) { return std::find(have.begin(), have.end(), w) != have.end(); });
            };
            auto listedIn = [&](const std::vector<std::string>& have) {
                return std::any_of(have.begin(), have.end(), [&](auto& f) { return listedFactions.contains(f); });
            };
            // Named members of a faction mill with no place of their own: in that mill only.
            std::vector<Pending> unplacedListed;

            // Named NPCs with a persistent reference are watched for moves, placed or not: a quest
            // or follower mod moves exactly those, and filling an alias makes a reference persistent.
            auto persistentRef = [&](RE::FormID base) -> RE::FormID {
                auto refs = refsByBase.find(base);
                if (refs == refsByBase.end()) return 0;
                RE::FormID any = 0;
                for (auto r : refs->second) {
                    const auto& a = scan.actors.at(r);
                    if (!a.persistent) continue;
                    if (!W.Disabled(r, a)) return r;
                    if (!any) any = r;
                }
                return any;
            };
            // The same order as placeMill below: factions first, then the place.
            auto stratumOf = [](std::uint32_t f) -> std::pair<Kind, bool> {
                if (f & fUnderworld) return { Kind::Underworld, false };
                if (f & fCourt) return { Kind::Court, false };
                if (f & fGuild) return { Kind::Commons, false };
                if (f & fGuard) return { Kind::Garrison, false };
                return { Kind::Commons, true };
            };

            for (const auto& r : people) {
                const auto* npc = RE::TESForm::LookupByID<RE::TESNPC>(r.npc);
                Pending     pd;
                pd.fac = W.FactionMask(npc, &pd.p.factions);
                auto e = scan.npcEditorIDs.find(r.npc);
                pd.p.key = r.key;
                pd.p.npc = r.npc;
                pd.p.editorID = e != scan.npcEditorIDs.end() ? e->second : fmt::format("{:08X}", r.npc);
                pd.p.name = npc ? Text::FromGame(npc->GetName()) : "";
                pd.p.location = r.loc;
                pd.p.unique = r.key == r.npc;
                // A faction mills.ini makes a mill of is people, even one hostile to strangers (Volkihar).
                const bool listed = pd.p.unique && listedIn(pd.p.factions);
                if (listed) pd.fac &= ~fHostile;
                auto drop = [&](const char* why) {
                    out->dropped[why]++;
                    if (listed) unplacedListed.push_back(pd);
                };
                // Travelling trades (caravans, carriage drivers, the courier) are links between
                // mills and never settle anywhere: a caravan's three days outside a town aren't a move.
                const auto edid = scan.npcEditorIDs.find(r.npc);
                const bool travels = (pd.fac & (fTraveller | fCarriage)) ||
                                     (edid != scan.npcEditorIDs.end() && edid->second.find("Courier") != std::string::npos);
                if (r.key == r.npc && !(pd.fac & fHostile) && !travels) {
                    if (const auto ref = persistentRef(r.npc)) {
                        const auto [kind, byPlace] = stratumOf(pd.fac);
                        out->watch.push_back({ ref, r.npc, kind, byPlace, false });
                    }
                }
                if (!r.loc) {
                    drop("no location");
                    continue;
                }
                if (pd.fac & fHostile) {
                    drop("hostile faction");
                    continue;
                }
                if (W.Hostile(r.loc)) {
                    drop("hostile location");
                    continue;
                }
                RE::FormID settlement = W.TierOf(r.loc, kwSettlement);
                const RE::FormID hold = W.HoldOf(r.loc);
                if (!settlement && (W.Kw(r.loc) & kwCivil) && hold && lpos.contains(r.loc)) {
                    // A farm, mill or mine outside town: the nearest settlement in the same hold.
                    double best = 0;
                    for (const auto& [s, sp] : lpos) {
                        if (!(W.Kw(s) & kwSettlement) || W.HoldOf(s) != hold) continue;
                        const double d = Dist(sp, lpos.at(r.loc));
                        if (!settlement || d < best || (d == best && s < settlement)) {
                            settlement = s;
                            best = d;
                        }
                    }
                }
                if (!settlement) {
                    const RE::FormID place = W.PlaceUnderHold(r.loc);
                    if (kNotEnclaves.contains(W.Edid(place))) {
                        drop("no settlement");
                        continue;
                    }
                    settlement = place;
                    pd.enclave = true;
                    if (r.key == r.npc) enclaveCount[place]++;
                }
                pd.p.settlement = settlement;
                pd.p.hold = hold ? hold : W.HoldOf(settlement);
                pending.push_back(std::move(pd));
            }

            std::vector<Pending*> persons;  // in placement order
            std::vector<Pending*> factionOnly;
            for (auto& pd : pending) {
                if (pd.enclave && enclaveCount[pd.p.settlement] < kEnclaveMin) {
                    out->dropped["no settlement"]++;
                    if (pd.p.unique && listedIn(pd.p.factions)) {
                        pd.p.settlement = pd.p.hold = 0;
                        factionOnly.push_back(&pd);
                    }
                    continue;
                }
                persons.push_back(&pd);
            }
            for (auto& pd : unplacedListed) factionOnly.push_back(&pd);

            MillTable mills;
            auto      placeMill = [&](const Pending& pd) -> std::pair<std::string, Kind> {
                const RE::FormID s = pd.p.settlement;
                const std::string base = Slug(W.Edid(s));
                if (!(W.Kw(s) & kwCity)) return { base, pd.enclave ? Kind::Enclave : Kind::Settlement };
                const auto f = pd.fac;
                if (f & fUnderworld) return { base + ".underworld", Kind::Underworld };
                if (f & fCourt) return { base + ".court", Kind::Court };
                if (f & fGuild) return { base + ".commons", Kind::Commons };
                if ((f & fGuard) || W.TierOf(pd.p.location, kwBarracks)) return { base + ".garrison", Kind::Garrison };
                if (W.TierOf(pd.p.location, kwCastle)) return { base + ".court", Kind::Court };
                return { base + ".commons", Kind::Commons };
            };

            std::map<std::pair<RE::FormID, Kind>, std::string> originalMill;  // before merging
            std::unordered_map<std::string, std::string>       foldedInto;
            for (auto* pd : persons) {
                auto [id, kind] = placeMill(*pd);
                originalMill[{ pd->p.settlement, kind }] = id;
                mills.Ensure(id, { kind, pd->p.settlement, pd->p.hold }).insert(pd->p.key);
                mills.InfoOf(id) = { kind, pd->p.settlement, pd->p.hold };
                pd->p.home = id;
            }

            // Faction mills from mills.ini, whatever their size or overlap: someone asked for exactly these.
            std::unordered_set<std::string> forced;
            for (const auto& fm : factionMills) {
                std::set<RE::FormID> members;
                int                  unplaced = 0;
                for (auto* pd : persons) {
                    if (pd->p.unique && inAny(pd->p.factions, fm.factions)) members.insert(pd->p.key);
                }
                for (auto* pd : factionOnly) {
                    if (inAny(pd->p.factions, fm.factions) && members.insert(pd->p.key).second) ++unplaced;
                }
                if (members.empty()) {
                    logger::info("Mills: faction mill {} has no named members", fm.id);
                    continue;
                }
                mills.Ensure(fm.id, { Kind::Institution, 0, 0 }) = members;
                forced.insert(fm.id);
                logger::info("Mills: faction mill {} ({}): {} named members, {} with no other mill", fm.id, fm.name, members.size(), unplaced);
            }

            // Institutions, unless one is really a single place's mill.
            for (std::size_t i = 0; i < kInstitutions.size(); ++i) {
                if (forced.contains(kInstitutions[i].id)) continue;
                std::set<RE::FormID> members;
                for (auto* pd : persons) {
                    if (pd->fac & (fInstitution0 << i)) members.insert(pd->p.key);
                }
                if (members.size() < 2) continue;
                std::size_t biggest = 0;
                for (const auto& id : mills.Ids()) {
                    const auto& ms = mills.Members(id);
                    std::size_t n = 0;
                    for (auto k : members) n += ms.count(k);
                    biggest = std::max(biggest, n);
                }
                if (biggest >= 0.8 * members.size()) continue;
                mills.Ensure(kInstitutions[i].id, { Kind::Institution, 0, 0 }) = members;
            }

            // ---- Merging below the floor ----

            const std::size_t floor = static_cast<std::size_t>(std::max(1, Settings::MillFloor()));
            std::unordered_map<RE::FormID, Pending*> byKey;
            for (auto* pd : persons) byKey[pd->p.key] = pd;

            auto fold = [&](const std::string& from, const std::string& to) {
                auto members = mills.Members(from);
                auto info = mills.InfoOf(from);
                mills.Remove(from);
                MillTable::Info target = mills.Has(to) ? mills.InfoOf(to) : MillTable::Info{ Kind::Commons, info.settlement, info.hold };
                mills.Ensure(to, target).insert(members.begin(), members.end());
                foldedInto[from] = to;
                for (auto k : members) {
                    if (byKey[k]->p.home == from) byKey[k]->p.home = to;
                }
            };

            // Small strata fold into their town's commons; small institutions go.
            for (const auto& id : mills.Ids()) {
                if (mills.Members(id).size() >= floor) continue;
                const auto info = mills.InfoOf(id);
                if (info.kind == Kind::Institution) {
                    if (!forced.contains(id)) mills.Remove(id);
                } else if (info.kind == Kind::Court || info.kind == Kind::Garrison || info.kind == Kind::Underworld) {
                    fold(id, Slug(W.Edid(info.settlement)) + ".commons");
                }
            }

            // Small settlements and enclaves fold into the nearest bigger one in their
            // hold; a place with no position on the map, into the hold's biggest.
            auto isPlace = [](Kind k) { return k == Kind::Settlement || k == Kind::Commons || k == Kind::Enclave; };
            std::vector<std::string> places;
            for (const auto& id : mills.Ids()) {
                if (isPlace(mills.InfoOf(id).kind)) places.push_back(id);
            }
            std::stable_sort(places.begin(), places.end(),
                             [&](auto& a, auto& b) { return mills.Members(a).size() < mills.Members(b).size(); });
            for (const auto& id : places) {
                if (!mills.Has(id) || mills.Members(id).size() >= floor) continue;
                const auto  info = mills.InfoOf(id);
                const auto  here = lpos.find(info.settlement);
                const bool  placed = here != lpos.end();
                std::string best;
                double      bestScore = 0;  // distance, or minus the size when unplaced
                for (const auto& other : mills.Ids()) {
                    if (other == id) continue;
                    const auto& oi = mills.InfoOf(other);
                    if (!isPlace(oi.kind) || oi.hold != info.hold || mills.Members(other).size() < floor) continue;
                    double score;
                    if (placed) {
                        auto op = lpos.find(oi.settlement);
                        if (op == lpos.end()) continue;
                        score = Dist(op->second, here->second);
                    } else {
                        score = -double(mills.Members(other).size());
                    }
                    if (best.empty() || score < bestScore) {
                        best = other;
                        bestScore = score;
                    }
                }
                if (!best.empty()) fold(id, best);
            }

            // ---- Edges ----

            std::map<std::pair<std::string, std::string>, Edge> edges;
            auto edge = [&](const std::string& a, const std::string& b, EdgeKind kind, double d, double w) {
                if (a == b || !mills.Has(a) || !mills.Has(b)) return;
                auto key = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
                auto it = edges.find(key);
                if (it == edges.end() || it->second.weight < w) edges[key] = Edge{ key.first, key.second, kind, d, w };
            };

            const auto liveIds = mills.Ids();
            std::unordered_map<RE::FormID, std::vector<std::string>> memberOf;
            for (const auto& id : liveIds) {
                for (auto k : mills.Members(id)) memberOf[k].push_back(id);
            }
            std::map<std::pair<std::string, std::string>, int> shared;
            for (auto& [k, ms] : memberOf) {
                std::sort(ms.begin(), ms.end());
                for (std::size_t i = 0; i < ms.size(); ++i) {
                    for (std::size_t j = i + 1; j < ms.size(); ++j) shared[{ ms[i], ms[j] }]++;
                }
            }
            for (auto& [pair, n] : shared) edge(pair.first, pair.second, EdgeKind::Shared, 0.0, double(n));

            std::vector<RE::FormID>                                      settlementOrder;
            std::unordered_map<RE::FormID, std::vector<std::string>>     bySettlement;
            for (const auto& id : liveIds) {
                const auto& info = mills.InfoOf(id);
                if (!info.settlement) continue;
                if (!bySettlement.contains(info.settlement)) settlementOrder.push_back(info.settlement);
                bySettlement[info.settlement].push_back(id);
            }
            auto strataWeight = [](Kind a, Kind b) {
                if (b < a) std::swap(a, b);
                if (a == Kind::Commons && b == Kind::Garrison) return 1.0;
                if (a == Kind::Commons && b == Kind::Court) return 0.4;
                if (a == Kind::Court && b == Kind::Garrison) return 0.7;
                if (a == Kind::Commons && b == Kind::Underworld) return 0.7;
                if (a == Kind::Garrison && b == Kind::Underworld) return 0.2;
                if (a == Kind::Court && b == Kind::Underworld) return 0.1;
                return 0.3;
            };
            for (auto s : settlementOrder) {
                const auto& ms = bySettlement[s];
                for (std::size_t i = 0; i < ms.size(); ++i) {
                    for (std::size_t j = i + 1; j < ms.size(); ++j) {
                        edge(ms[i], ms[j], EdgeKind::Strata, 0.0, strataWeight(mills.InfoOf(ms[i]).kind, mills.InfoOf(ms[j]).kind));
                    }
                }
            }

            // Roads: each settlement to its 3 nearest in the same world.
            std::unordered_map<RE::FormID, std::string> commonsOf;
            for (auto s : settlementOrder) {
                const auto& ms = bySettlement[s];
                commonsOf[s] = ms.front();
                for (const auto& m : ms) {
                    const auto k = mills.InfoOf(m).kind;
                    if (k == Kind::Commons || k == Kind::Settlement) {
                        commonsOf[s] = m;
                        break;
                    }
                }
            }
            std::vector<RE::FormID> settled;
            for (auto s : settlementOrder) {
                if (lpos.contains(s)) settled.push_back(s);
            }
            for (auto s : settled) {
                std::vector<std::pair<double, RE::FormID>> near;
                for (auto t : settled) {
                    if (t != s && lpos.at(t).root == lpos.at(s).root) near.push_back({ Dist(lpos.at(s), lpos.at(t)), t });
                }
                std::stable_sort(near.begin(), near.end(), [](auto& a, auto& b) { return a.first < b.first; });
                if (near.size() > 3) near.resize(3);
                for (auto& [d, t] : near) {
                    const bool inn = W.TierOf(s, kwHasInn) || W.TierOf(t, kwHasInn) || (W.Kw(s) & kwCity) || (W.Kw(t) & kwCity);
                    edge(commonsOf[s], commonsOf[t], EdgeKind::Road, d, inn ? 1.0 : 0.5);
                }
            }

            // Carriages between the settlements where drivers live; the ship to Solstheim.
            std::set<RE::FormID> carriage;
            for (auto* pd : persons) {
                if (pd->fac & fCarriage) carriage.insert(pd->p.settlement);
            }
            std::vector<RE::FormID> stops(carriage.begin(), carriage.end());
            for (std::size_t i = 0; i < stops.size(); ++i) {
                for (std::size_t j = i + 1; j < stops.size(); ++j) {
                    auto a = stops[i], b = stops[j];
                    if (!lpos.contains(a) || !lpos.contains(b) || lpos.at(a).root != lpos.at(b).root) continue;
                    if (!commonsOf.contains(a) || !commonsOf.contains(b)) continue;
                    edge(commonsOf[a], commonsOf[b], EdgeKind::Carriage, Dist(lpos.at(a), lpos.at(b)), 0.6);
                }
            }
            std::unordered_map<std::string, RE::FormID> bySlug;
            for (auto& [s, m] : commonsOf) bySlug[Slug(W.Edid(s))] = s;
            for (auto& [a, b] : kShipLinks) {
                auto ia = bySlug.find(std::string(a)), ib = bySlug.find(std::string(b));
                if (ia != bySlug.end() && ib != bySlug.end()) {
                    edge(commonsOf[ia->second], commonsOf[ib->second], EdgeKind::Ship, 60.0, 0.5);
                }
            }

            // Hold fallback: every settlement to its hold's largest.
            std::unordered_map<RE::FormID, RE::FormID> capital;
            for (auto s : settlementOrder) {
                const auto h = W.HoldOf(s);
                if (!h) continue;
                auto it = capital.find(h);
                if (it == capital.end() || mills.Members(commonsOf[s]).size() > mills.Members(commonsOf[it->second]).size()) {
                    capital[h] = s;
                }
            }
            for (auto s : settlementOrder) {
                const auto h = W.HoldOf(s);
                if (!h || capital[h] == s || !lpos.contains(s) || !lpos.contains(capital[h])) continue;
                edge(commonsOf[s], commonsOf[capital[h]], EdgeKind::Hold, Dist(lpos.at(s), lpos.at(capital[h])), 0.15);
            }

            // Links from mills.ini, in place of whatever the builder made between the two.
            auto resolve = [&](std::string id) {
                for (int i = 0; i < 16 && !mills.Has(id); ++i) {
                    auto f = foldedInto.find(id);
                    if (f == foldedInto.end()) break;
                    id = f->second;
                }
                return id;
            };
            // Where a mill is on the map: its place, or for a guild or faction, its members' towns.
            auto pointsOf = [&](const std::string& id) {
                std::vector<Point> out;
                if (auto p = lpos.find(mills.InfoOf(id).settlement); p != lpos.end()) out.push_back(p->second);
                if (mills.InfoOf(id).kind == Kind::Institution) {
                    for (auto k : mills.Members(id)) {
                        auto pd = byKey.find(k);
                        if (pd == byKey.end() || pd->second->p.home.empty() || !mills.Has(pd->second->p.home)) continue;
                        if (auto p = lpos.find(mills.InfoOf(pd->second->p.home).settlement); p != lpos.end()) out.push_back(p->second);
                    }
                }
                return out;
            };
            for (const auto& l : LoadLinks(ini)) {
                const auto a = resolve(l.a), b = resolve(l.b);
                if (!mills.Has(a) || !mills.Has(b) || a == b) {
                    logger::warn("Mills: {}: link {} = {}: {}", l.file, l.a, l.b, a == b && mills.Has(a) ? "both are " + a : "no such mill");
                    continue;
                }
                const auto key = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
                auto       it = edges.find(key);
                if (it != edges.end() && it->second.kind == EdgeKind::Shared) {
                    logger::warn("Mills: link {} = {}: they're linked by their shared members already", a, b);
                    continue;
                }
                if (l.kind == "none") {
                    logger::info("Mills: link {} - {} {}", a, b, it != edges.end() ? "removed" : "to remove isn't there");
                    if (it != edges.end()) edges.erase(it);
                    continue;
                }
                EdgeKind kind;
                double   w;
                if (l.kind == "road") kind = EdgeKind::Road, w = 1.0;
                else if (l.kind == "carriage") kind = EdgeKind::Carriage, w = 0.6;
                else if (l.kind == "ship") kind = EdgeKind::Ship, w = 0.5;
                else if (l.kind == "hold") kind = EdgeKind::Hold, w = 0.15;
                else if (l.kind == "ears") kind = EdgeKind::Ears, w = 1.0;
                else {
                    logger::warn("Mills: {}: link {} = {}: unknown kind {} (road, carriage, ship, hold, ears or none)", l.file, a, b,
                                 l.kind);
                    continue;
                }
                if (l.weight) w = *l.weight;
                // The nearest two of their places; 0 where either has none.
                double d = -1.0;
                for (const auto& pa : pointsOf(a)) {
                    for (const auto& pb : pointsOf(b)) {
                        if (pa.root == pb.root && (d < 0.0 || Dist(pa, pb) < d)) d = Dist(pa, pb);
                    }
                }
                d = kind == EdgeKind::Ship ? 60.0 : std::max(0.0, d);
                const bool fresh = it == edges.end();
                // Ears carry news from b, where it is said, to a, who listens.
                edges[key] = kind == EdgeKind::Ears ? Edge{ b, a, kind, d, w } : Edge{ key.first, key.second, kind, d, w };
                logger::info("Mills: link {} {} {}: {} (weight {:.2f}, {:.1f} cells){}", kind == EdgeKind::Ears ? a : key.first,
                             kind == EdgeKind::Ears ? "hears" : "-", kind == EdgeKind::Ears ? b : key.second, l.kind, w, d,
                             fresh ? ", new" : "");
            }
            for (const auto& [id, v] : LoadMillSecrecy(ini)) {
                const auto m = resolve(id);
                if (!mills.Has(m)) {
                    logger::warn("Mills: secrecy {}: no such mill", id);
                    continue;
                }
                out->secrecy[m] = v;
                logger::info("Mills: {} keeps what it hears at secrecy {:.2f}", m, v);
            }
            for (const auto& [id, file] : LoadEager(ini)) {
                const auto m = resolve(id);
                if (!mills.Has(m)) {
                    logger::warn("Mills: {}: retell {}: no such mill", file, id);
                    continue;
                }
                if (std::find(out->eager.begin(), out->eager.end(), m) == out->eager.end()) out->eager.push_back(m);
            }
            if (!out->eager.empty()) {
                std::string list;
                for (const auto& m : out->eager) list += (list.empty() ? "" : ", ") + m;
                logger::info("Mills: written as soon as a rumour arrives: {}", list);
            }
            for (const auto& e : Ini::Keyed(ini, "lines")) out->lines[e.key] = e.value;
            logger::info("Mills: {} mill lines for the retell prompt", out->lines.size());
            LoadPartyFactions(ini, W.scan_, *out);

            // ---- Output ----

            auto placeNameOf = [&](RE::FormID loc) -> std::string {
                auto        it = W.locs.find(loc);
                const char* n = it != W.locs.end() && it->second.form ? it->second.form->GetName() : nullptr;
                return n && *n ? Text::FromGame(n) : Slug(W.Edid(loc));
            };
            for (const auto& id : liveIds) {
                const auto& info = mills.InfoOf(id);
                Mill        m;
                m.id = id;
                m.placeName = info.settlement ? placeNameOf(info.settlement) : id;
                for (const auto& inst : kInstitutions) {
                    if (id == inst.id) m.placeName = inst.name;
                }
                for (const auto& fm : factionMills) {
                    if (id == fm.id) m.placeName = fm.name;
                }
                m.kind = info.kind;
                m.settlement = info.settlement;
                m.hold = info.hold;
                const auto& ms = mills.Members(id);
                m.members.assign(ms.begin(), ms.end());
                if (auto p = lpos.find(info.settlement); info.settlement && p != lpos.end()) m.pos = p->second;
                out->mills.push_back(std::move(m));
            }
            std::sort(out->mills.begin(), out->mills.end(), [](auto& a, auto& b) { return a.id < b.id; });
            for (std::size_t i = 0; i < out->mills.size(); ++i) out->byId[out->mills[i].id] = i;

            for (auto* pd : persons) {
                if (!memberOf.contains(pd->p.key)) continue;  // left with a dropped institution only
                auto& list = out->millsOf[pd->p.key];
                list.push_back(pd->p.home);
                for (const auto& m : memberOf[pd->p.key]) {
                    if (m != pd->p.home) list.push_back(m);
                }
                out->persons[pd->p.key] = pd->p;
            }
            for (auto* pd : factionOnly) {
                if (!memberOf.contains(pd->p.key) || out->millsOf.contains(pd->p.key)) continue;
                out->millsOf[pd->p.key] = memberOf[pd->p.key];
                out->persons[pd->p.key] = pd->p;
            }
            for (auto& w : out->watch) w.placed = out->millsOf.contains(w.base);
            for (auto& [k, e] : edges) out->edges.push_back(e);

            // ---- Where talk belongs, for rumours placed by location ----

            for (const auto& [id, l] : W.locs) {
                std::uint32_t f = 0;
                if (l.kw & kwSettlement) f |= kSettlement;
                if (l.kw & kwInn) f |= kInn;
                if (l.kw & kwCastle) f |= kCastle;
                if (l.kw & kwBarracks) f |= kBarracks;
                if (W.HoldOf(id) == id) {
                    f |= kHold;
                    out->holdNames[id] = placeNameOf(id);
                }
                out->locations[id] = { l.parent, f };
            }
            for (const auto& [id, c] : W.scan_.cells) {
                if (!c.interior && c.hasGrid && c.world && c.location) out->exteriorLocations[World::GridKey(c.world, c.x, c.y)] = c.location;
            }
            for (const auto& [key, id] : originalMill) {
                std::string at = id;
                for (int i = 0; i < 16 && !mills.Has(at); ++i) {
                    auto f = foldedInto.find(at);
                    if (f == foldedInto.end()) break;
                    at = f->second;
                }
                if (mills.Has(at)) {
                    out->placeMills[key.first][key.second] = at;
                    out->settlementNames[key.first] = placeNameOf(key.first);
                }
            }
            std::unordered_map<RE::FormID, std::map<std::string, int>> homes;
            for (auto* pd : persons) homes[pd->p.location][pd->p.home]++;
            for (const auto& [loc, counts] : homes) {
                auto best = std::max_element(counts.begin(), counts.end(), [](auto& a, auto& b) { return a.second < b.second; });
                out->locationHome[loc] = best->first;
            }
            for (const auto& id : liveIds) {
                const auto& info = mills.InfoOf(id);
                if (!info.hold || !isPlace(info.kind)) continue;
                auto& cap = out->holdCapital[info.hold];
                if (cap.empty() || mills.Members(id).size() > mills.Members(cap).size()) cap = id;
            }

            {
                std::lock_guard lk(g_lock);
                for (auto& [id, l] : W.locs) {
                    if ((l.kw & kwSettlement) || W.HoldOf(id) == id) g_placeNames[id] = Slug(l.edid);
                }
                for (const auto& m : out->mills) {
                    if (m.settlement) g_placeNames[m.settlement] = Slug(W.Edid(m.settlement));
                }
            }
            return out;
        }
    }

    const char* KindName(Kind k) {
        switch (k) {
        case Kind::Settlement: return "settlement";
        case Kind::Commons: return "commons";
        case Kind::Court: return "court";
        case Kind::Garrison: return "garrison";
        case Kind::Underworld: return "underworld";
        case Kind::Enclave: return "enclave";
        case Kind::Institution: return "institution";
        }
        return "?";
    }

    const char* EdgeKindName(EdgeKind k) {
        switch (k) {
        case EdgeKind::Shared: return "shared";
        case EdgeKind::Strata: return "strata";
        case EdgeKind::Road: return "road";
        case EdgeKind::Carriage: return "carriage";
        case EdgeKind::Ship: return "ship";
        case EdgeKind::Hold: return "hold";
        case EdgeKind::Ears: return "ears";
        }
        return "?";
    }

    const Mill* MillSet::Find(std::string_view id) const {
        auto it = byId.find(std::string(id));
        return it != byId.end() ? &mills[it->second] : nullptr;
    }

    double MillSet::SecrecyOf(std::string_view id) const {
        auto it = secrecy.find(std::string(id));
        return it != secrecy.end() ? it->second : 0.0;
    }

    bool MillSet::IsEager(std::string_view id) const { return std::find(eager.begin(), eager.end(), id) != eager.end(); }

    namespace {
        /** A person's mills with any stored placement applied: where they settled replaces their
         *  place mill, and their institutions stay. `built` is null for someone the builder never placed. */
        std::vector<std::string> WithPlacement(const MillSet& set, RE::FormID key, const std::vector<std::string>* built) {
            std::string placed;
            {
                std::shared_lock lk(g_placementLock);
                if (auto it = g_placements.find(key); it != g_placements.end()) placed = it->second;
            }
            if (placed.empty() || !set.Find(placed)) return built ? *built : std::vector<std::string>{};
            std::vector<std::string> out{ placed };
            if (built) {
                for (const auto& m : *built) {
                    const auto* mill = set.Find(m);
                    if (m != placed && mill && mill->kind == Kind::Institution) out.push_back(m);
                }
            }
            return out;
        }
    }

    std::vector<std::string> MillSet::MillsFor(RE::FormID formId) const {
        if (auto it = millsOf.find(formId); it != millsOf.end()) return WithPlacement(*this, formId, &it->second);
        // A named NPC's reference, loaded or not: their base is the key.
        if (auto b = baseOfRef.find(formId); b != baseOfRef.end()) {
            auto it = millsOf.find(b->second);
            return WithPlacement(*this, b->second, it != millsOf.end() ? &it->second : nullptr);
        }
        if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(formId)) {
            if (auto* actor = ref->As<RE::Actor>()) return MillsFor(actor);
        }
        return WithPlacement(*this, formId, nullptr);
    }

    RE::FormID MillSet::LocationAt(RE::FormID world, const RE::NiPoint3& pos) const {
        const auto gx = static_cast<std::int32_t>(std::floor(pos.x / kCellUnits));
        const auto gy = static_cast<std::int32_t>(std::floor(pos.y / kCellUnits));
        if (auto e = exteriorLocations.find(World::GridKey(world, gx, gy)); e != exteriorLocations.end()) return e->second;
        auto* ws = RE::TESForm::LookupByID<RE::TESWorldSpace>(world);
        return ws && ws->location ? ws->location->GetFormID() : 0;
    }

    RE::FormID MillSet::LocationNear(RE::FormID world, const RE::NiPoint3& pos, int cells) const {
        const double cx = pos.x / kCellUnits, cy = pos.y / kCellUnits;
        const auto   gx = static_cast<std::int32_t>(std::floor(cx));
        const auto   gy = static_cast<std::int32_t>(std::floor(cy));
        // Ring by ring, the nearest cell centre in the first ring that has any.
        for (int r = 0; r <= cells; ++r) {
            RE::FormID best = 0;
            double     bestDist = 0.0;
            for (int dx = -r; dx <= r; ++dx) {
                for (int dy = -r; dy <= r; ++dy) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                    auto e = exteriorLocations.find(World::GridKey(world, gx + dx, gy + dy));
                    if (e == exteriorLocations.end()) continue;
                    const double ex = gx + dx + 0.5 - cx, ey = gy + dy + 0.5 - cy, d = ex * ex + ey * ey;
                    if (!best || d < bestDist) {
                        best = e->second;
                        bestDist = d;
                    }
                }
            }
            if (best) return best;
        }
        return 0;
    }

    std::string MillSet::BuiltHome(RE::FormID key) const {
        auto it = millsOf.find(key);
        if (it == millsOf.end() || it->second.empty()) return "";
        const auto* m = Find(it->second.front());
        return m && m->kind != Kind::Institution ? it->second.front() : "";
    }

    std::string MillSet::SettleAt(RE::FormID location, Kind kind, bool byPlace) const {
        // In a settlement with mills, the one for their stratum, as placeMill decides it.
        auto choose = [&](RE::FormID settlement, std::uint32_t seen) -> std::string {
            auto p = placeMills.find(settlement);
            if (p == placeMills.end()) return "";
            const auto& kinds = p->second;
            auto pick = [&](Kind k) -> const std::string* {
                auto it = kinds.find(k);
                return it != kinds.end() ? &it->second : nullptr;
            };
            const std::string* m = nullptr;
            if (!byPlace) m = pick(kind);
            if (!m && byPlace && (seen & kBarracks)) m = pick(Kind::Garrison);
            if (!m && byPlace && (seen & kCastle)) m = pick(Kind::Court);
            for (Kind k : { Kind::Commons, Kind::Settlement, Kind::Enclave }) {
                if (!m) m = pick(k);
            }
            return m ? *m : "";
        };
        std::uint32_t seen = 0;
        RE::FormID    at = location;
        for (int i = 0; at && i < 32; ++i) {
            auto loc = locations.find(at);
            if (loc == locations.end()) break;
            seen |= loc->second.second;
            if (placeMills.contains(at)) return choose(at, seen);
            // A farm or mine outside town belongs to a settlement's mills: a guard posted
            // there is still that town's garrison, as the builder has it.
            if (auto h = locationHome.find(at); h != locationHome.end()) {
                const auto* m = Find(h->second);
                if (m && m->settlement && placeMills.contains(m->settlement)) {
                    if (auto c = choose(m->settlement, seen); !c.empty()) return c;
                }
                return h->second;
            }
            at = loc->second.first;
        }
        return "";
    }

    void SetPlacement(RE::FormID base, const std::string& mill) {
        std::unique_lock lk(g_placementLock);
        if (auto& p = g_placements[base]; p != mill) {
            p = mill;
            ++g_placementsVersion;
        }
    }

    void ClearPlacement(RE::FormID base) {
        std::unique_lock lk(g_placementLock);
        if (g_placements.erase(base)) ++g_placementsVersion;
    }

    std::uint64_t PlacementsVersion() { return g_placementsVersion; }

    void ClearPlacements() {
        std::unique_lock lk(g_placementLock);
        if (!g_placements.empty()) ++g_placementsVersion;
        g_placements.clear();
    }

    RE::FormID MillSet::HoldOf(RE::FormID location) const {
        RE::FormID out = 0;
        for (int i = 0; location && i < 32; ++i) {
            auto it = locations.find(location);
            if (it == locations.end()) break;
            if (it->second.second & kHold) out = location;
            location = it->second.first;
        }
        return out;
    }

    bool MillSet::LocationIs(RE::FormID location, std::uint32_t flags) const {
        for (int i = 0; location && i < 32; ++i) {
            auto it = locations.find(location);
            if (it == locations.end()) return false;
            if (it->second.second & flags) return true;
            location = it->second.first;
        }
        return false;
    }

    std::vector<std::string> MillSet::MillsForLocation(RE::FormID location) const {
        // Up to the nearest settlement that has mills, noting a castle or barracks on the way.
        std::uint32_t seen = 0;
        RE::FormID    at = location;
        for (int i = 0; at && i < 32; ++i) {
            auto loc = locations.find(at);
            if (loc == locations.end()) break;
            seen |= loc->second.second;
            if (auto p = placeMills.find(at); p != placeMills.end()) {
                const auto& kinds = p->second;
                auto pick = [&](Kind k) -> const std::string* {
                    auto it = kinds.find(k);
                    return it != kinds.end() ? &it->second : nullptr;
                };
                const std::string* m = nullptr;
                if (seen & kCastle) m = pick(Kind::Court);
                if (!m && (seen & kBarracks)) m = pick(Kind::Garrison);
                for (Kind k : { Kind::Commons, Kind::Settlement, Kind::Enclave, Kind::Court, Kind::Garrison, Kind::Underworld }) {
                    if (!m) m = pick(k);
                }
                if (m) return { *m };
            }
            if (auto h = locationHome.find(at); h != locationHome.end()) return { h->second };
            at = loc->second.first;
        }
        if (auto c = holdCapital.find(HoldOf(location)); c != holdCapital.end()) return { c->second };
        return {};
    }

    std::vector<std::string> MillSet::PlaceMillsAt(RE::FormID location) const {
        RE::FormID at = location;
        for (int i = 0; at && i < 32; ++i) {
            auto loc = locations.find(at);
            if (loc == locations.end()) break;
            RE::FormID place = placeMills.contains(at) ? at : 0;
            if (!place) {
                if (auto h = locationHome.find(at); h != locationHome.end()) {
                    if (const auto* m = Find(h->second)) place = m->settlement;
                }
            }
            if (auto p = placeMills.find(place); place && p != placeMills.end()) {
                std::vector<std::string> out;
                for (const auto& [kind, id] : p->second) out.push_back(id);
                return out;
            }
            at = loc->second.first;
        }
        return {};
    }

    std::vector<std::string> MillSet::MillsInHold(std::string_view hold) const {
        auto lower = [](std::string_view s) {
            std::string out(s);
            for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        };
        // "Whiterun", "Whiterun Hold" and "whiterunhold" all name it, whichever of the first two
        // the game calls it.
        auto bare = [&](std::string_view s) {
            auto out = lower(s);
            if (out.ends_with(" hold")) out.resize(out.size() - 5);
            return out;
        };
        const std::string want = bare(hold);
        std::string       squashed = lower(hold);
        std::erase(squashed, ' ');
        RE::FormID id = 0;
        for (const auto& [h, name] : holdNames) {
            if (bare(name) == want || PlaceName(h) == squashed) id = h;
        }
        std::vector<std::string> out;
        if (!id) return out;
        for (const auto& m : mills) {
            if (m.hold == id) out.push_back(m.id);
        }
        return out;
    }

    std::vector<std::string> MillSet::MillsInSettlement(std::string_view name, int* matches) const {
        auto lower = [](std::string_view s) {
            std::string out(s);
            for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        };
        const std::string       want = lower(name);
        std::vector<RE::FormID> found;
        for (const auto& [s, n] : settlementNames) {
            if (lower(n) == want || PlaceName(s) == want) found.push_back(s);
        }
        if (matches) *matches = static_cast<int>(found.size());
        return found.size() == 1 ? PlaceMillsAt(found.front()) : std::vector<std::string>{};
    }

    std::vector<std::string> MillSet::MillsFor(const RE::Actor* actor) const {
        if (!actor) return {};
        if (auto it = millsOf.find(actor->GetFormID()); it != millsOf.end()) return WithPlacement(*this, actor->GetFormID(), &it->second);
        RE::FormID key = 0;
        for (const RE::TESForm* base : { static_cast<const RE::TESForm*>(actor->GetBaseObject()),
                                         static_cast<const RE::TESForm*>(actor->GetActorBase()) }) {
            if (!base) continue;
            if (auto it = millsOf.find(base->GetFormID()); it != millsOf.end()) return WithPlacement(*this, base->GetFormID(), &it->second);
            if (!key) key = base->GetFormID();
        }
        return key ? WithPlacement(*this, key, nullptr) : std::vector<std::string>{};
    }

    void SetScan(std::shared_ptr<const PluginScan::Result> scan) {
        std::lock_guard lk(g_lock);
        g_scan = std::move(scan);
    }

    bool HasScan() {
        std::lock_guard lk(g_lock);
        return g_scan != nullptr;
    }

    std::shared_ptr<const MillSet> Build(const char* reason) {
        std::shared_ptr<const PluginScan::Result> scan;
        {
            std::lock_guard lk(g_lock);
            scan = g_scan;
        }
        if (!scan) return Current();

        const auto start = std::chrono::steady_clock::now();
        auto       set = BuildFrom(*scan);
        set->reason = reason;
        set->milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

        std::lock_guard lk(g_lock);
        g_current = set;
        return set;
    }

    std::shared_ptr<const MillSet> Current() {
        std::lock_guard lk(g_lock);
        return g_current;
    }

    std::string PlaceName(RE::FormID location) {
        if (!location) return {};
        std::lock_guard lk(g_lock);
        auto it = g_placeNames.find(location);
        return it != g_placeNames.end() ? it->second : fmt::format("{:08X}", location);
    }

    void Log(const MillSet& set, bool full) {
        std::map<std::string, int> kinds, edgeKinds;
        std::vector<std::size_t>   sizes;
        for (const auto& m : set.mills) {
            kinds[KindName(m.kind)]++;
            sizes.push_back(m.members.size());
        }
        for (const auto& e : set.edges) edgeKinds[EdgeKindName(e.kind)]++;
        std::sort(sizes.begin(), sizes.end());
        int unique = 0, multi = 0;
        for (const auto& [k, p] : set.persons) unique += p.unique;
        for (const auto& [k, ms] : set.millsOf) multi += ms.size() > 1;
        auto join = [](const std::map<std::string, int>& m) {
            std::string s;
            for (const auto& [k, v] : m) s += fmt::format("{}{} {}", s.empty() ? "" : ", ", k, v);
            return s;
        };

        logger::info("Mills ({}): {} mills built in {:.0f} ms: {}", set.reason, set.mills.size(), set.milliseconds,
                     join(kinds));
        logger::info("  people placed {} (named {}, generic {}), in 2+ mills {}", set.persons.size(), unique,
                     set.persons.size() - unique, multi);
        if (!sizes.empty()) {
            logger::info("  mill size: min {}, median {}, p90 {}, max {}", sizes.front(), sizes[sizes.size() / 2],
                         sizes[(sizes.size() - 1) * 9 / 10], sizes.back());
        }
        logger::info("  sources: {}", join(set.sources));
        logger::info("  dropped: {}", join(set.dropped));
        logger::info("  edges {}: {}", set.edges.size(), join(edgeKinds));
        if (!full) return;
        for (const auto& m : set.mills) {
            std::vector<std::string> names;
            for (auto k : m.members) {
                auto p = set.persons.find(k);
                if (p != set.persons.end()) names.push_back(p->second.name.empty() ? p->second.editorID : p->second.name);
            }
            std::sort(names.begin(), names.end());
            std::string list;
            for (std::size_t i = 0; i < names.size() && i < 14; ++i) list += (i ? ", " : "") + names[i];
            if (names.size() > 14) list += ", ...";
            logger::info("  {:<28} {:<11} {:>3}  {}", m.id, KindName(m.kind), m.members.size(), list);
        }
    }

    void WriteJson(const MillSet& set, const std::filesystem::path& path) {
        using nlohmann::json;
        json mills = json::object();
        for (const auto& m : set.mills) {
            json members = json::array();
            for (auto k : m.members) members.push_back(std::to_string(k));
            mills[m.id] = { { "kind", KindName(m.kind) },
                            { "settlement", m.settlement ? json(PlaceName(m.settlement)) : json(nullptr) },
                            { "hold", m.hold ? json(PlaceName(m.hold)) : json(nullptr) },
                            { "pos", m.pos ? json::array({ m.pos->root, m.pos->x, m.pos->y }) : json(nullptr) },
                            { "secrecy", set.SecrecyOf(m.id) },
                            { "eager", set.IsEager(m.id) },
                            { "members", members } };
        }
        json persons = json::object();
        for (const auto& [k, p] : set.persons) {
            persons[std::to_string(k)] = { { "name", p.editorID },
                                           { "display", p.name },
                                           { "home", p.home },
                                           { "settlement", PlaceName(p.settlement) },
                                           { "hold", p.hold ? json(PlaceName(p.hold)) : json(nullptr) },
                                           { "unique", p.unique },
                                           { "fac", p.factions } };
        }
        json edges = json::array();
        for (const auto& e : set.edges) {
            edges.push_back({ { "a", e.a }, { "b", e.b }, { "kind", EdgeKindName(e.kind) }, { "dist", e.dist }, { "w", e.weight } });
        }
        json out = { { "strategy", "merged" }, { "reason", set.reason }, { "mills", mills }, { "persons", persons }, { "edges", edges } };
        try {
            std::ofstream f(path);
            f << Text::Dump(out, 1);
        } catch (const std::exception& e) {
            logger::warn("Could not write {}: {}", path.string(), e.what());
        }
    }
}
