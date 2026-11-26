#include "Settings.h"
#include "SkyrimNet.h"

#include <algorithm>
#include <charconv>

namespace RumourEngine::Settings {

    namespace {

        bool ReadBool(const char* path, bool fallback) {
            auto v = SkyrimNet::Setting(path, fallback ? "true" : "false");
            if (v == "true" || v == "1") return true;
            if (v == "false" || v == "0") return false;
            return fallback;
        }

        int ReadInt(const char* path, int fallback) {
            auto v = SkyrimNet::Setting(path, std::to_string(fallback).c_str());
            int  out = fallback;
            auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
            return ec == std::errc{} ? out : fallback;
        }

        double ReadDouble(const char* path, double fallback) {
            auto   v = SkyrimNet::Setting(path, std::to_string(fallback).c_str());
            double out = fallback;
            auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
            return ec == std::errc{} ? out : fallback;
        }

        TellWhere ReadTellWhere(const char* path) {
            auto v = SkyrimNet::Setting(path, "taverns");
            for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v == "never") return TellWhere::Never;
            if (v == "towns") return TellWhere::Towns;
            if (v == "anywhere") return TellWhere::Anywhere;
            return TellWhere::Taverns;
        }

        std::atomic<bool>      g_enabled = true;
        std::atomic<int>       g_millFloor = 4;
        std::atomic<bool>      g_dumpMills = true;
        std::atomic<bool>      g_logPrompts = false;
        std::atomic<int>       g_maxRumours = 3;
        std::atomic<int>       g_airingsUntilStale = 3;
        std::atomic<bool>      g_starterEnabled = true;
        std::atomic<int>       g_starterCooldown = 180;
        std::atomic<int>       g_itemCooldown = 15;
        std::atomic<double>    g_minTeller = 0.6;
        std::atomic<bool>      g_rateBios = true;
        std::atomic<double>    g_bioWeight = 0.7;
        std::atomic<bool>      g_reachEnabled = true;
        std::atomic<double>    g_reachBaseRate = 0.5;
        std::atomic<double>    g_reachGossipRate = 2.0;
        std::atomic<TellWhere> g_tellPlayer = TellWhere::Taverns;
        std::atomic<TellWhere> g_tellParty = TellWhere::Taverns;
        std::atomic<double>    g_hopTickHours = 2.0;
        std::atomic<double>    g_hopDecayLocal = 0.9;
        std::atomic<double>    g_hopDecayTravel = 0.6;
        std::atomic<double>    g_decayTravelPerN = 0.3;
        std::atomic<double>    g_travelRateScale = 8.0;
        std::atomic<double>    g_travelExponent = 4.0;
        std::atomic<double>    g_notabilityFloor = 0.15;
        std::atomic<double>    g_baseFreshDays = 2.0;
        std::atomic<double>    g_freshPerNotability = 6.0;
        std::atomic<int>       g_maxMills = 80;
        std::atomic<bool>      g_retellEager = false;
        std::atomic<bool>      g_harvestEnabled = true;
        std::atomic<double>    g_harvestInterval = 2.0;
        std::atomic<double>    g_harvestWindow = 2.0;
        std::atomic<double>    g_importanceFloor = 0.5;
        std::atomic<double>    g_gossipFloorBonus = 0.2;
        std::atomic<double>    g_screenThreshold = 0.6;
        std::atomic<double>    g_gossipThresholdBonus = 0.25;
        std::atomic<int>       g_minInterest = 3;
        std::atomic<int>       g_harvestMaxActors = 10;
        std::atomic<int>       g_harvestPerActor = 6;
        std::atomic<bool>      g_rememberTelling = true;
        std::atomic<bool>      g_skipDead = true;
        std::atomic<bool>      g_holdback = true;
        std::atomic<double>    g_holdbackAfter = 0.5;
        std::atomic<double>    g_holdbackLocal = 2.0;
        std::atomic<double>    g_holdbackThere = 0.5;
        std::atomic<int>       g_maxSeeds = 6;
        std::atomic<double>    g_settleHours = 24.0;
        std::atomic<double>    g_settleMaxDays = 3.0;
        std::atomic<int>       g_followerHarvestMax = 6;
        std::atomic<bool>      g_promoteEnabled = true;
        std::atomic<double>    g_promoteNotability = 0.8;
        std::atomic<double>    g_promoteMinReach = 0.25;
        std::atomic<double>    g_promoteLasting = 0.4;
        std::atomic<bool>      g_questsEnabled = true;
        std::atomic<double>    g_questsHeard = 0.5;
        std::atomic<double>    g_questsNotability = 0.7;
    }

    void Refresh() {
        g_enabled = ReadBool("general.enabled", true);
        g_millFloor = ReadInt("mills.floor", 4);
        g_dumpMills = ReadBool("debug.dump_mills", true);
        g_logPrompts = ReadBool("debug.log_prompts", false);
        g_maxRumours = ReadInt("delivery.max_rumours_per_prompt", 3);
        g_airingsUntilStale = ReadInt("delivery.airings_until_stale", 3);
        g_starterEnabled = ReadBool("starter.enabled", true);
        g_starterCooldown = ReadInt("starter.cooldown_seconds", 180);
        g_itemCooldown = ReadInt("starter.item_cooldown_minutes", 15);
        g_minTeller = ReadDouble("starter.min_teller_score", 0.6);
        g_rateBios = ReadBool("gossip.rate_bios", true);
        g_bioWeight = std::clamp(ReadDouble("gossip.bio_weight", 0.7), 0.0, 1.0);
        g_reachEnabled = ReadBool("reach.enabled", true);
        g_reachBaseRate = std::clamp(ReadDouble("reach.base_rate", 0.5), 0.0, 100.0);
        g_reachGossipRate = std::clamp(ReadDouble("reach.gossip_rate", 2.0), 0.0, 100.0);
        g_tellPlayer = ReadTellWhere("starter.tell_player");
        g_tellParty = ReadTellWhere("starter.tell_party");
        g_hopTickHours = std::clamp(ReadDouble("spread.tick_hours", 2.0), 0.25, 24.0);
        g_hopDecayLocal = std::clamp(ReadDouble("spread.decay_local", 0.9), 0.0, 1.0);
        g_hopDecayTravel = std::clamp(ReadDouble("spread.decay_travel", 0.6), 0.0, 1.0);
        g_decayTravelPerN = std::clamp(ReadDouble("spread.decay_travel_per_notability", 0.3), 0.0, 1.0);
        g_travelRateScale = std::clamp(ReadDouble("spread.travel_rate_scale", 8.0), 0.0, 100.0);
        g_travelExponent = std::clamp(ReadDouble("spread.travel_exponent", 4.0), 0.5, 8.0);
        g_notabilityFloor = std::clamp(ReadDouble("spread.notability_floor", 0.15), 0.0, 1.0);
        g_baseFreshDays = std::max(0.1, ReadDouble("spread.base_fresh_days", 2.0));
        g_freshPerNotability = std::max(0.0, ReadDouble("spread.fresh_days_per_notability", 6.0));
        g_maxMills = std::max(1, ReadInt("spread.max_mills_per_rumour", 80));
        auto mode = SkyrimNet::Setting("retell.mode", "lazy");
        g_retellEager = mode == "eager";
        g_harvestEnabled = ReadBool("harvest.enabled", true);
        g_harvestInterval = std::clamp(ReadDouble("harvest.interval_hours", 2.0), 0.25, 48.0);
        g_harvestWindow = std::clamp(ReadDouble("harvest.window_days", 2.0), 0.25, 30.0);
        g_importanceFloor = std::clamp(ReadDouble("harvest.importance_floor", 0.5), 0.0, 1.0);
        g_gossipFloorBonus = std::clamp(ReadDouble("harvest.gossip_floor_bonus", 0.2), 0.0, 1.0);
        g_screenThreshold = std::clamp(ReadDouble("harvest.screen_threshold", 0.6), 0.0, 1.0);
        g_gossipThresholdBonus = std::clamp(ReadDouble("harvest.gossip_threshold_bonus", 0.25), 0.0, 1.0);
        g_minInterest = std::clamp(ReadInt("harvest.min_interest", 3), 1, 5);
        g_harvestMaxActors = std::clamp(ReadInt("harvest.max_actors", 10), 1, 50);
        g_harvestPerActor = std::clamp(ReadInt("harvest.memories_per_actor", 6), 1, 10);
        g_maxSeeds = std::clamp(ReadInt("harvest.max_seeds_per_sweep", 6), 1, 20);
        g_rememberTelling = ReadBool("harvest.remember_telling", true);
        g_skipDead = ReadBool("harvest.skip_dead", true);
        g_holdback = ReadBool("holdback.enabled", true);
        g_holdbackAfter = std::clamp(ReadDouble("holdback.after_days", 0.5), 0.0, 30.0);
        g_holdbackLocal = std::clamp(ReadDouble("holdback.local_days", 2.0), 0.0, 60.0);
        g_holdbackThere = std::clamp(ReadDouble("holdback.there_hours", 0.5), 0.0, 6.0);
        g_settleHours = std::clamp(ReadDouble("settle.hours", 24.0), 1.0, 240.0);
        g_settleMaxDays = std::clamp(ReadDouble("settle.max_days", 3.0), 0.25, 30.0);
        g_followerHarvestMax = std::clamp(ReadInt("settle.follower_harvest_max", 6), 0, 20);
        g_promoteEnabled = ReadBool("promote.enabled", true);
        g_promoteNotability = std::clamp(ReadDouble("promote.notability", 0.8), 0.0, 1.0);
        g_promoteMinReach = std::clamp(ReadDouble("promote.min_reach", 0.25), 0.0, 1.0);
        g_promoteLasting = std::clamp(ReadDouble("promote.lasting", 0.4), 0.0, 1.0);
        g_questsEnabled = ReadBool("quests.enabled", true);
        g_questsHeard = std::clamp(ReadDouble("quests.heard", 0.5), 0.0, 1.0);
        g_questsNotability = std::clamp(ReadDouble("quests.notability", 0.7), 0.0, 1.0);
    }

    bool      Enabled() { return g_enabled; }
    int       MillFloor() { return g_millFloor; }
    bool      DumpMills() { return g_dumpMills; }
    bool      LogPrompts() { return g_logPrompts; }
    int       MaxRumoursPerPrompt() { return g_maxRumours; }
    int       AiringsUntilStale() { return g_airingsUntilStale; }
    bool      StarterEnabled() { return g_starterEnabled; }
    int       StarterCooldownSeconds() { return g_starterCooldown; }
    int       ItemCooldownMinutes() { return g_itemCooldown; }
    double    MinTellerScore() { return g_minTeller; }
    bool      RateBios() { return g_rateBios; }
    double    BioWeight() { return g_bioWeight; }
    bool      ReachEnabled() { return g_reachEnabled; }
    double    ReachBaseRate() { return g_reachBaseRate; }
    double    ReachGossipRate() { return g_reachGossipRate; }
    TellWhere TellPlayer() { return g_tellPlayer; }
    TellWhere TellParty() { return g_tellParty; }
    double    HopTickHours() { return g_hopTickHours; }
    double    HopDecayLocal() { return g_hopDecayLocal; }
    double    HopDecayTravel() { return g_hopDecayTravel; }
    double    DecayTravelPerNotability() { return g_decayTravelPerN; }
    double    TravelRateScale() { return g_travelRateScale; }
    double    TravelExponent() { return g_travelExponent; }
    double    NotabilityFloor() { return g_notabilityFloor; }
    double    BaseFreshDays() { return g_baseFreshDays; }
    double    FreshDaysPerNotability() { return g_freshPerNotability; }
    int       MaxMillsPerRumour() { return g_maxMills; }
    bool      RetellEager() { return g_retellEager; }
    bool      HarvestEnabled() { return g_harvestEnabled; }
    double    HarvestIntervalHours() { return g_harvestInterval; }
    double    HarvestWindowDays() { return g_harvestWindow; }
    double    ImportanceFloor() { return g_importanceFloor; }
    double    GossipFloorBonus() { return g_gossipFloorBonus; }
    double    ScreenThreshold() { return g_screenThreshold; }
    double    GossipThresholdBonus() { return g_gossipThresholdBonus; }
    int       MinInterest() { return g_minInterest; }
    int       HarvestMaxActors() { return g_harvestMaxActors; }
    int       HarvestMemoriesPerActor() { return g_harvestPerActor; }
    bool      RememberTelling() { return g_rememberTelling; }
    bool      HarvestSkipDead() { return g_skipDead; }
    bool      HoldbackEnabled() { return g_holdback; }
    double    HoldbackAfterDays() { return g_holdbackAfter; }
    double    HoldbackLocalDays() { return g_holdbackLocal; }
    double    HoldbackThereHours() { return g_holdbackThere; }
    int       MaxSeedsPerSweep() { return g_maxSeeds; }
    double    SettleHours() { return g_settleHours; }
    double    SettleMaxDays() { return g_settleMaxDays; }
    int       FollowerHarvestMax() { return g_followerHarvestMax; }
    bool      PromoteEnabled() { return g_promoteEnabled; }
    double    PromoteNotability() { return g_promoteNotability; }
    double    PromoteMinReach() { return g_promoteMinReach; }
    double    PromoteLasting() { return g_promoteLasting; }
    bool      QuestsEnabled() { return g_questsEnabled; }
    double    QuestsHeard() { return g_questsHeard; }
    double    QuestsNotability() { return g_questsNotability; }
}
