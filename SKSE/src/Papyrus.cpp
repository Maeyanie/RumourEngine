#include "Papyrus.h"
#include "Delivery.h"
#include "Mills.h"
#include "Rumours.h"
#include "Session.h"

#include <nlohmann/json.hpp>

// RumourEngine.psc: global natives only, so nothing is attached to anything and
// nothing is stored in saves.

namespace RumourEngine::Papyrus {

    namespace {

        constexpr auto kScript = "RumourEngine"sv;

        std::int32_t AddFromJson(std::string_view fn, const std::string& json) {
            std::string error;
            const auto  id = Rumours::Add(json, error);
            if (!id) logger::warn("RumourEngine.{}: {}", fn, error);
            return id;
        }

        std::int32_t GetApiVersion(RE::StaticFunctionTag*) { return kApiVersion; }

        bool IsReady(RE::StaticFunctionTag*) { return Session::IsReady(); }

        std::int32_t AddRumour(RE::StaticFunctionTag*, RE::BSFixedString source, RE::BSFixedString text,
                               RE::TESObjectREFR* origin, float notability, std::vector<RE::Actor*> subjects,
                               RE::BSFixedString key) {
            if (!origin) {
                logger::warn("RumourEngine.AddRumour: no origin");
                return 0;
            }
            nlohmann::json j = { { "source", source.c_str() },
                                 { "text", text.c_str() },
                                 { "notability", notability },
                                 { "origin", { { "actor", origin->GetFormID() } } } };
            if (key.c_str() && *key.c_str()) j["key"] = key.c_str();
            auto subs = nlohmann::json::array();
            for (auto* a : subjects) {
                if (a) subs.push_back(a->GetFormID());
            }
            if (!subs.empty()) j["subjects"] = subs;
            return AddFromJson("AddRumour", j.dump());
        }

        std::int32_t AddRumourInMill(RE::StaticFunctionTag*, RE::BSFixedString source, RE::BSFixedString text,
                                     RE::BSFixedString mill, float notability, RE::BSFixedString key) {
            nlohmann::json j = { { "source", source.c_str() },
                                 { "text", text.c_str() },
                                 { "notability", notability },
                                 { "origin", { { "mills", { mill.c_str() } } } } };
            if (key.c_str() && *key.c_str()) j["key"] = key.c_str();
            return AddFromJson("AddRumourInMill", j.dump());
        }

        std::int32_t AddRumourJson(RE::StaticFunctionTag*, RE::BSFixedString json) {
            return AddFromJson("AddRumourJson", json.c_str() ? json.c_str() : "");
        }

        bool RetireRumour(RE::StaticFunctionTag*, std::int32_t id) {
            std::string error;
            if (Rumours::Retire(id, error)) return true;
            logger::warn("RumourEngine.RetireRumour: {}", error);
            return false;
        }

        bool RetireRumourByKey(RE::StaticFunctionTag*, RE::BSFixedString source, RE::BSFixedString key) {
            std::string error;
            if (Rumours::RetireByKey(source.c_str(), key.c_str(), error)) return true;
            logger::warn("RumourEngine.RetireRumourByKey: {}", error);
            return false;
        }

        std::vector<RE::BSFixedString> GetRumoursFor(RE::StaticFunctionTag*, RE::Actor* actor, std::int32_t max) {
            std::vector<RE::BSFixedString> out;
            auto                           set = Mills::Current();
            if (!set || !actor || Delivery::InPlayersCompany(actor)) return out;
            const auto* base = actor->GetActorBase();
            for (const auto& h : Rumours::InMills(set->MillsFor(actor), actor->GetFormID(), base ? base->GetFormID() : 0,
                                                  Rumours::Now(), static_cast<std::size_t>(std::max(1, max)))) {
                out.emplace_back(h.text);
            }
            return out;
        }

        std::vector<RE::BSFixedString> GetMillsFor(RE::StaticFunctionTag*, RE::Actor* actor) {
            std::vector<RE::BSFixedString> out;
            auto                           set = Mills::Current();
            if (!set || !actor) return out;
            for (const auto& id : set->MillsFor(actor)) out.emplace_back(id);
            return out;
        }

        bool HasHeard(RE::StaticFunctionTag*, std::int32_t id, RE::Actor* actor) {
            return actor && Rumours::HasHeard(id, actor->GetFormID());
        }
    }

    bool Register(RE::BSScript::IVirtualMachine* vm) {
        vm->RegisterFunction("GetApiVersion"sv, kScript, GetApiVersion);
        vm->RegisterFunction("IsReady"sv, kScript, IsReady);
        vm->RegisterFunction("AddRumour"sv, kScript, AddRumour);
        vm->RegisterFunction("AddRumourInMill"sv, kScript, AddRumourInMill);
        vm->RegisterFunction("AddRumourJson"sv, kScript, AddRumourJson);
        vm->RegisterFunction("RetireRumour"sv, kScript, RetireRumour);
        vm->RegisterFunction("RetireRumourByKey"sv, kScript, RetireRumourByKey);
        vm->RegisterFunction("GetRumoursFor"sv, kScript, GetRumoursFor);
        vm->RegisterFunction("GetMillsFor"sv, kScript, GetMillsFor);
        vm->RegisterFunction("HasHeard"sv, kScript, HasHeard);
        logger::info("Papyrus: registered RumourEngine natives");
        return true;
    }
}
