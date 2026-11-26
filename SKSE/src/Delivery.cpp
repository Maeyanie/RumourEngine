#include "Delivery.h"
#include "Gossip.h"
#include "Mills.h"
#include "Runtime.h"
#include "Settings.h"
#include "SkyrimNet.h"
#include "Text.h"

namespace RumourEngine::Delivery {

    namespace {

        constexpr RE::FormID kCurrentFollowerFaction = 0x0005C84E;

        // Who is in a scene, as SkyrimNet's own speaker selectors tell it. Local FormIDs.
        constexpr std::pair<RE::FormID, std::string_view> kSceneFactions[] = {
            { 0x000D93, "OStim.esp" },    // OStimExcitementFaction
            { 0x00E50F, "SexLab.esm" },   // SexLabAnimatingFaction
        };

        /** The person record for an actor: by reference, else by base. */
        const Mills::Person* PersonOf(const Mills::MillSet& set, RE::Actor* actor) {
            if (auto it = set.persons.find(actor->GetFormID()); it != set.persons.end()) return &it->second;
            for (const RE::TESForm* base : { static_cast<const RE::TESForm*>(actor->GetBaseObject()),
                                             static_cast<const RE::TESForm*>(actor->GetActorBase()) }) {
                if (!base) continue;
                if (auto it = set.persons.find(base->GetFormID()); it != set.persons.end()) return &it->second;
            }
            return nullptr;
        }

        std::string NameOf(RE::Actor* actor) {
            return Text::FromGame(actor ? actor->GetDisplayFullName() : nullptr);
        }

        /** Cut a telling short for the GameMaster's list, at a word, not inside a UTF-8 character. */
        std::string Shorten(std::string s, std::size_t max) {
            if (s.size() <= max) return s;
            std::size_t cut = s.find_last_of(' ', max);
            if (cut == std::string::npos || cut < max / 2) {
                cut = max;
                while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
            }
            while (cut > 0 && (s[cut - 1] == ',' || s[cut - 1] == ';' || s[cut - 1] == ' ')) --cut;
            return s.substr(0, cut) + "...";
        }

        /** With debug.log_prompts, log a decorator's output when it changes for an NPC. */
        void LogIfChanged(const char* what, RE::Actor* actor, const std::string& text) {
            if (!actor || !Settings::LogPrompts()) return;
            static std::mutex                                                     lock;
            static std::map<std::pair<std::string, RE::FormID>, std::size_t> last;
            const auto                                                            h = std::hash<std::string>{}(text);
            {
                std::lock_guard lk(lock);
                auto&           slot = last[{ what, actor->GetFormID() }];
                if (slot == h) return;
                slot = h;
            }
            logger::info("{} for {} ({:08X}):{}{}", what, NameOf(actor), actor->GetFormID(), text.empty() ? " (empty)" : "\n",
                         text);
        }
    }

    bool InPlayersCompany(RE::Actor* actor) {
        if (!actor) return false;
        if (actor->IsPlayerTeammate()) return true;
        // The reference's own faction changes win: the Creation Kit puts anyone
        // recruitable in CurrentFollowerFaction at -1, and recruiting sets it to 0+.
        auto* follower = RE::TESForm::LookupByID<RE::TESFaction>(kCurrentFollowerFaction);
        if (!follower) return false;
        bool following = false;
        actor->VisitFactions([&](RE::TESFaction* f, std::int8_t rank) {
            if (f == follower) following = rank >= 0;
            return false;
        });
        return following;
    }

    bool InIntimateScene(RE::Actor* actor) {
        static const auto factions = [] {
            std::vector<RE::TESFaction*> found;
            for (const auto& [id, plugin] : kSceneFactions) {
                if (auto* f = Runtime::LookupForm<RE::TESFaction>(id, plugin)) found.push_back(f);
            }
            return found;
        }();
        if (!actor || factions.empty()) return false;
        bool in = false;
        actor->VisitFactions([&](RE::TESFaction* f, std::int8_t rank) {
            if (rank >= 0 && std::find(factions.begin(), factions.end(), f) != factions.end()) in = true;
            return in;
        });
        return in;
    }

    double GossipScore(RE::Actor* actor) {
        auto set = Mills::Current();
        if (!set || !actor) return 0.0;
        const auto* p = PersonOf(*set, actor);
        if (!p) return 0.0;
        // Asked about because they matter just now, which is when their bio is worth reading.
        Gossip::Want(actor, *p);
        return Gossip::ScoreOf(*p);
    }

    double GossipScoreOf(const Mills::Person& p) { return Gossip::ScoreOf(p); }

    std::string PlaceOf(const std::string& mill) {
        auto set = Mills::Current();
        const auto* m = set ? set->Find(mill) : nullptr;
        return m ? m->placeName : mill;
    }

    std::optional<Rumours::Heard> PickTelling(RE::Actor* actor, double now) {
        if (!actor || InPlayersCompany(actor) || InIntimateScene(actor)) return std::nullopt;
        const double gossip = GossipScore(actor);
        if (gossip < Settings::MinTellerScore()) return std::nullopt;
        auto set = Mills::Current();
        if (!set) return std::nullopt;
        const auto  mills = set->MillsFor(actor);
        const auto* base = actor->GetActorBase();
        for (auto& h : Rumours::InMills(mills, actor->GetFormID(), base ? base->GetFormID() : 0, now, 8)) {
            // Nobody gossips about themselves, and a story someone else has just told waits its turn.
            if (h.aboutThem || Rumours::AiredByOtherRecently(h.id, actor->GetFormID())) continue;
            // Nor about what they were part of: they speak of that from their own memory, not as talk.
            if (Settings::HoldbackEnabled() && Rumours::IsInvolved(h.id, actor->GetFormID())) continue;
            // A secret is only passed on by someone who gossips more readily than it is kept, and
            // nor is anything a close mill heard.
            if (const double keep = std::max(h.secrecy, set->SecrecyOf(h.mill)); keep > 0.0 && gossip <= keep) continue;
            // Told often enough in the player's hearing: still known, no longer pressed on anyone.
            if (const int limit = Settings::AiringsUntilStale(); limit > 0 && h.airings >= limit) continue;
            return h;
        }
        return std::nullopt;
    }

    std::string BuildTalk(RE::Actor* actor) {
        if (!actor || InPlayersCompany(actor)) return "";
        auto set = Mills::Current();
        if (!set) return "";
        const auto mills = set->MillsFor(actor);
        if (mills.empty()) return "";
        const std::string name = NameOf(actor);
        if (name.empty()) return "";

        const double      now = Rumours::Now();
        const auto*       base = actor->GetActorBase();
        const std::size_t cap = static_cast<std::size_t>(std::max(1, Settings::MaxRumoursPerPrompt()));
        // What a client marked as fact is what they know, not talk: its own list, with its own cap.
        std::vector<Rumours::Heard> facts, talk;
        for (auto& h : Rumours::InMills(mills, actor->GetFormID(), base ? base->GetFormID() : 0, now, cap * 2)) {
            auto& list = h.fact ? facts : talk;
            if (list.size() < cap) list.push_back(std::move(h));
        }
        if (facts.empty() && talk.empty()) return "";

        // The GameMaster can start a conversation without a topic, so the reason a
        // gossip was picked has to be in their own bio, or it never gets said. It is
        // marked where it is listed, and only spelled out on its own when it isn't.
        const auto        pick = PickTelling(actor, now);
        const std::string keen = "Most on " + name + "'s mind just now, and worth bringing up given the chance: ";
        bool              listed = false;
        auto              line = [&](const Rumours::Heard& h) {
            if (pick && h.id == pick->id) {
                listed = true;
                return "- " + keen + h.text + "\n";
            }
            // Hold-back: it has come back to them from another group's talk.
            if (!h.from.empty()) {
                return h.aboutThem ? "- People in " + PlaceOf(h.from) + " are saying this about " + name + ": " + h.text + "\n"
                                   : "- Word going round " + PlaceOf(h.from) + ": " + h.text + "\n";
            }
            return h.aboutThem ? "- People are saying this about " + name + ": " + h.text + "\n" : "- " + h.text + "\n";
        };

        std::string md;
        std::vector<std::string> factMills;  // a heading each, in the order found
        for (const auto& h : facts) {
            if (std::find(factMills.begin(), factMills.end(), h.mill) == factMills.end()) factMills.push_back(h.mill);
        }
        for (const auto& m : factMills) {
            const auto* mill = set->Find(m);
            md += mill && mill->kind == Mills::Kind::Institution ? "## What " + name + " knows as one of " + PlaceOf(m) + "\n"
                                                                 : "## What " + name + " knows for certain\n";
            md += "Their own people's account, which " + name + " takes as true.\n";
            for (const auto& h : facts) {
                if (h.mill == m) md += line(h);
            }
            md += "\n";
        }
        if (!talk.empty()) {
            md += "## Local talk\n";
            md += "What " + name + " has heard going round " + PlaceOf(mills.front()) +
                  " lately. It is talk, not certainty: any of it may be wrong, garbled or exaggerated, and " + name +
                  " knows that.\n";
            for (const auto& h : talk) md += line(h);
        }
        if (pick && !listed) md += "\n" + keen + pick->text + "\n";
        return md;
    }

    std::string BuildNews(RE::Actor* actor) {
        auto pick = PickTelling(actor, Rumours::Now());
        if (!pick) return "";
        // A pointer, not the story: the whole of it is in their bio.
        return "Also keen to pass on the talk in " + PlaceOf(pick->mill) + ": " + Shorten(pick->text, 110);
    }

    void RegisterDecorators() {
        auto wrap = [](const char* what, std::string (*build)(RE::Actor*)) {
            return [what, build](RE::Actor* actor) -> std::string {
                try {
                    auto out = build(actor);
                    LogIfChanged(what, actor, out);
                    return out;
                } catch (const std::exception& e) {
                    logger::error("{}: {}", what, e.what());
                    return "";
                }
            };
        };
        const bool talk = SkyrimNet::RegisterDecorator(
            "get_rumourengine_talk", "RumourEngine: the talk going round this NPC's mills, for their bio",
            wrap("get_rumourengine_talk", BuildTalk));
        const bool news = SkyrimNet::RegisterDecorator(
            "get_rumourengine_news", "RumourEngine: what this gossip is keen to pass on, for the GameMaster",
            wrap("get_rumourengine_news", BuildNews));
        // For world-knowledge conditions, so a promoted rumour is known to those it reached:
        // contains(get_rumourengine_heard(actorUUID), "|57|").
        const bool heard = SkyrimNet::RegisterDecorator(
            "get_rumourengine_heard", "RumourEngine: the promoted rumours this NPC has heard, as |id|id|, for world-knowledge conditions",
            [](RE::Actor* actor) -> std::string {
                try {
                    return actor ? Rumours::HeardPromoted(actor->GetFormID()) : std::string{};
                } catch (const std::exception& e) {
                    logger::error("get_rumourengine_heard: {}", e.what());
                    return "";
                }
            });
        logger::info("Decorators: get_rumourengine_talk {}, get_rumourengine_news {}, get_rumourengine_heard {}",
                     talk ? "registered" : "FAILED", news ? "registered" : "FAILED", heard ? "registered" : "FAILED");
    }
}
