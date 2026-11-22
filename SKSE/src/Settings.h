#pragma once

// Settings live on SkyrimNet's settings page (the layer's settings/RumourEngine.yaml)
// and are read through SkyrimNet's plugin config API. They are cached, and the
// cache is refreshed at data load, at every game load, and every few seconds by
// the GossipStarter's worker. Prompt decorators run inside SkyrimNet's own
// rendering, where calling back into SkyrimNet is best avoided, so the
// accessors only ever read the cache.

namespace RumourEngine::Settings {

    /** Where the player, or a member of their party, may be told something. */
    enum class TellWhere { Never, Taverns, Towns, Anywhere };

    /** Read every setting from SkyrimNet into the cache. Not from inside a decorator. */
    void Refresh();

    /** The whole mod. Off means nothing is built and no save database is opened. */
    bool Enabled();

    /** Mills smaller than this fold into a neighbour (ADR-001, "Merging"). */
    int MillFloor();

    /** Write every mill and its members to the log, as well as the summary. */
    bool DumpMills();

    /** Log what the prompt decorators render, whenever it changes for an NPC. */
    bool LogPrompts();

    /** How many rumours an NPC's bio shows. */
    int MaxRumoursPerPrompt();

    /** Tellings in the player's hearing before a rumour is stale in that mill: everyone there has heard it. */
    int AiringsUntilStale();

    bool StarterEnabled();
    int  StarterCooldownSeconds();
    int  ItemCooldownMinutes();

    /** Gossip score from which an NPC starts tellings; below it they only join in. */
    double MinTellerScore();

    TellWhere TellPlayer();
    TellWhere TellParty();

    // Gossip scores and who has heard it (ADR-001, "Gossip score" and "Who has heard it").

    /** Read each named person's bio once with the decision model, and again when it changes. */
    bool RateBios();

    /** How far a bio reading moves someone's score from their job's: 0 not at all, 1 all the way. */
    double BioWeight();

    /** Off, everyone in a mill has heard everything there. */
    bool ReachEnabled();

    /** The share of a mill a rumour reaches grows at base + gossip x the person's score, per day. */
    double ReachBaseRate();
    double ReachGossipRate();

    // Spreading (ADR-001, "The mill graph and hops").

    /** Game hours between hop simulation ticks. */
    double HopTickHours();

    /** Notability kept per hop within a place, and per hop to another place. */
    double HopDecayLocal();
    double HopDecayTravel();

    /** Added to the travel decay per unit of notability, so big news keeps its size on the road. */
    double DecayTravelPerNotability();

    /** Hops to another place (road, carriage, ship, hold): the base rates' multiplier, and the
     *  power of notability they scale with. Within a place it stays notability squared. */
    double TravelRateScale();
    double TravelExponent();

    /** A rumour weaker than this stops spreading. */
    double NotabilityFloor();

    /** Days a mill keeps a rumour fresh: base + per_notability x notability. */
    double BaseFreshDays();
    double FreshDaysPerNotability();

    /** The most mills one rumour reaches. */
    int MaxMillsPerRumour();

    /** Retell every arrival at once, instead of when someone first needs it. For testing. */
    bool RetellEager();

    // Harvest (ADR-001, "Harvest and screening").

    bool HarvestEnabled();

    /** Game hours between sweeps of who has new memories. */
    double HarvestIntervalHours();

    /** How far back, in game days, a sweep looks for memories nobody has screened. */
    double HarvestWindowDays();

    /** Importance a memory needs to be looked at; a gossip's floor is lower by bonus x score. */
    double ImportanceFloor();
    double GossipFloorBonus();

    /** Probability of "would pass it on" a memory needs; a gossip's is lower by bonus x score. */
    double ScreenThreshold();
    double GossipThresholdBonus();

    /** "Would a room go quiet for it", 1 to 5, a memory needs to become a rumour. */
    int MinInterest();

    /** Most people screened per sweep, and memories per person. */
    int HarvestMaxActors();
    int HarvestMemoriesPerActor();

    /** Most new rumours one sweep seeds. */
    int MaxSeedsPerSweep();

    /** Whoever a harvested rumour starts with remembers telling it. */
    bool RememberTelling();

    // Settling (ADR-001, "Dismissed followers" and "Observed homes").

    /** Game hours someone must stay in one place before they count as living there. */
    double SettleHours();

    /** Game days after a dismissal when a follower counts as settled wherever they are. */
    double SettleMaxDays();

    /** Most of a dismissed follower's memories from their time away that are screened. */
    int FollowerHarvestMax();

    // Promotion to world knowledge (ADR-001, "Lifecycle").

    /** Keep the most notable rumours as SkyrimNet world knowledge once they go stale. Off, the
     *  entries already made are taken out again, and put back when it is turned on. */
    bool PromoteEnabled();

    /** The share of the mills' people a rumour must have reached to be considered, 0 to 1. */
    double PromoteMinReach();

    /** How sure the decision model must be (0 to 1) that people will still talk of it in a year. */
    double PromoteLasting();

    /** Without a decision model: how notable a rumour must be to be kept instead. */
    double PromoteNotability();

    // Quest endings (ADR-001, "Quest endings").

    /** Make a rumour of a quest's ending when word of it would get out. */
    bool QuestsEnabled();

    /** How sure the decision model must be (0 to 1) that word of an ending gets beyond those who took part. */
    double QuestsHeard();

    /** Without a decision model: the notability of the endings quests.ini says always. */
    double QuestsNotability();
}
