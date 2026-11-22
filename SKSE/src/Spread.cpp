#include "Spread.h"
#include "Api.h"
#include "Delivery.h"
#include "Gossip.h"
#include "Retell.h"
#include "Rumours.h"
#include "Session.h"
#include "Settings.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <deque>

namespace RumourEngine::Spread {

    namespace {

        // Hop rates per day, per unit of edge weight and notability squared. Stage 0's
        // values (tools/hopsim.py): squaring n keeps minor talk local, and these gave
        // 76% of minor talk staying in its hold while 9 in 10 major rumours left it.
        // Hops to another place are scaled by spread.travel_rate_scale and go with a
        // steeper power of n (spread.travel_exponent), so big news crosses Skyrim in days
        // while minor talk still stays home (tuned with tools/spreadsim.py).
        constexpr double kRateShared = 1.0;  // x the gossip scores of the shared members
        constexpr double kRateStrata = 3.0;
        constexpr double kRateRoad = 0.6;
        constexpr double kRateCarriage = 0.1;
        constexpr double kRateShip = 0.3;
        constexpr double kRateHold = 0.3;

        constexpr double kTravelCellsPerDay = 40.0;
        constexpr int    kMaxTicksPerCall = 720;  // a long sleep or wait catches up at most this many

        /** A fixed roll in [0, 1), as Delivery's per-person one. */
        double Mix(std::uint64_t a, std::uint64_t b) {
            std::uint64_t x = a * 0x9E3779B97F4A7C15ull + b * 0xBF58476D1CE4E5B9ull;
            x ^= x >> 31;
            x *= 0x94D049BB133111EBull;
            x ^= x >> 29;
            return double(x & 0xFFFFFFFFull) / 4294967296.0;
        }

        /** FNV-1a, stable across builds, unlike std::hash. */
        std::uint64_t Fnv(std::string_view s, std::uint64_t h = 0xCBF29CE484222325ull) {
            for (unsigned char c : s) {
                h ^= c;
                h *= 0x100000001B3ull;
            }
            return h;
        }

        double Distance(const Mills::Point& a, const Mills::Point& b) { return std::hypot(a.x - b.x, a.y - b.y); }

        /** The mill graph, ready to simulate on. Rebuilt whenever the mills or the placements are. */
        struct Graph {
            struct Out {
                std::string        to;
                const Mills::Edge* edge;
                double             gossip;  // shared edges: the shared members' summed gossip scores
                // Shared edges: each shared member's roll key and score, to count only those a rumour has reached.
                const std::vector<std::pair<std::uint64_t, double>>* shared = nullptr;
            };
            std::shared_ptr<const Mills::MillSet>                set;
            std::uint64_t                                         placements = 0;  // the PlacementsVersion it was built at
            std::uint64_t                                         gossip = 0;      // and the Gossip::Version
            std::deque<std::vector<std::pair<std::uint64_t, double>>> sharedLists;
            std::vector<Mills::Edge>                              edges;
            std::unordered_map<std::string, std::vector<Out>>   adj;
            std::unordered_map<std::string, double>             maxGossip;  // the readiest gossip in each mill
            std::unordered_map<RE::FormID, Mills::Point>         placePos;

            explicit Graph(std::shared_ptr<const Mills::MillSet> s)
                : set(std::move(s)), placements(Mills::PlacementsVersion()), gossip(Gossip::Version()) {
                // Who is in each mill now. Someone who has settled elsewhere since the build belongs to
                // the mill they live in, so their guild or faction is linked to that town instead.
                std::unordered_map<std::string, std::vector<RE::FormID>> members;
                for (const auto& m : set->mills) {
                    members[m.id] = m.members;
                    if (m.settlement && m.pos && !placePos.contains(m.settlement)) placePos[m.settlement] = *m.pos;
                }
                for (const auto& [key, built] : set->millsOf) {
                    const auto now = set->MillsFor(key);
                    if (now == built) continue;
                    for (const auto& m : built) {
                        if (std::find(now.begin(), now.end(), m) == now.end()) std::erase(members[m], key);
                    }
                    for (const auto& m : now) {
                        if (std::find(built.begin(), built.end(), m) == built.end()) members[m].push_back(key);
                    }
                }
                auto gossipOf = [&](RE::FormID k) {
                    auto p = set->persons.find(k);
                    return p != set->persons.end() ? Delivery::GossipScoreOf(p->second) : 0.0;
                };
                std::unordered_map<RE::FormID, std::vector<std::string>> memberOf;
                for (auto& [id, ks] : members) {
                    std::sort(ks.begin(), ks.end());
                    double best = 0.0;
                    for (auto k : ks) {
                        best = std::max(best, gossipOf(k));
                        memberOf[k].push_back(id);
                    }
                    maxGossip[id] = best;
                }

                // Shared members link mills, as the builder links them; then the builder's other edges,
                // the stronger kept where two would join the same pair.
                std::map<std::pair<std::string, std::string>, Mills::Edge> byPair;
                for (auto& [k, ms] : memberOf) {
                    std::sort(ms.begin(), ms.end());
                    for (std::size_t i = 0; i < ms.size(); ++i) {
                        for (std::size_t j = i + 1; j < ms.size(); ++j) {
                            auto [it, fresh] = byPair.try_emplace({ ms[i], ms[j] });
                            if (fresh) it->second = Mills::Edge{ ms[i], ms[j], Mills::EdgeKind::Shared, 0.0, 0.0 };
                            it->second.weight += 1.0;
                        }
                    }
                }
                for (const auto& e : set->edges) {
                    if (e.kind == Mills::EdgeKind::Shared) continue;
                    const auto key = e.a < e.b ? std::make_pair(e.a, e.b) : std::make_pair(e.b, e.a);
                    auto       it = byPair.find(key);
                    if (it == byPair.end() || it->second.weight < e.weight) byPair[key] = e;
                }
                edges.reserve(byPair.size());
                for (auto& [key, e] : byPair) edges.push_back(std::move(e));
                for (const auto& e : edges) {
                    double gossip = 0.0;
                    const std::vector<std::pair<std::uint64_t, double>>* shared = nullptr;
                    if (e.kind == Mills::EdgeKind::Shared) {
                        const auto& a = members[e.a];
                        const auto& b = members[e.b];
                        std::vector<RE::FormID> both;
                        std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(both));
                        auto& list = sharedLists.emplace_back();
                        for (auto k : both) {
                            const double g = gossipOf(k);
                            gossip += g;
                            auto p = set->persons.find(k);
                            list.emplace_back(p != set->persons.end() ? Gossip::RollKey(p->second) : std::uint64_t(k), g);
                        }
                        shared = &list;
                    }
                    adj[e.a].push_back({ e.b, &e, gossip, shared });
                    if (e.kind != Mills::EdgeKind::Ears) adj[e.b].push_back({ e.a, &e, gossip, shared });  // ears hear, never tell
                }
            }

            const Mills::Mill* Find(const std::string& id) const { return set->Find(id); }

            /** Days for news to cross an edge. */
            static double EdgeDelay(const Mills::Edge& e) {
                switch (e.kind) {
                case Mills::EdgeKind::Shared: return 0.1;
                case Mills::EdgeKind::Strata: return 0.2;
                case Mills::EdgeKind::Ship: return 2.0;
                default: return 0.25 + e.dist / kTravelCellsPerDay;
                }
            }

            /** Hops a day along an edge, from a mill where the rumour has notability `n`. */
            static double Rate(const Out& o, double n, double gossip) {
                const double w = o.edge->weight;
                const double local = n * n;
                const double travel = Settings::TravelRateScale() * std::pow(n, Settings::TravelExponent());
                switch (o.edge->kind) {
                case Mills::EdgeKind::Shared: return kRateShared * gossip * local;
                case Mills::EdgeKind::Strata: return kRateStrata * w * local;
                case Mills::EdgeKind::Road: return kRateRoad * w * travel;
                case Mills::EdgeKind::Carriage: return kRateCarriage * w * travel;
                case Mills::EdgeKind::Ship: return kRateShip * w * travel;
                case Mills::EdgeKind::Hold: return kRateHold * w * travel;
                case Mills::EdgeKind::Ears: return kRateShared * w * local;  // one listener, keen as a shared member
                }
                return 0.0;
            }

            /** Days between two places, by where they sit on the map. */
            double PlaceDelay(RE::FormID a, RE::FormID b) const {
                auto pa = placePos.find(a), pb = placePos.find(b);
                if (pa == placePos.end() || pb == placePos.end() || pa->second.root != pb->second.root) return 2.0;
                return 0.25 + Distance(pa->second, pb->second) / kTravelCellsPerDay;
            }

            /** The place a mill's telling is in. An institution carries the place it heard it in. */
            RE::FormID PlaceOf(const Rumours::Rumour& r, std::string mill) const {
                for (int i = 0; i < 64; ++i) {
                    const auto* m = Find(mill);
                    if (!m) return 0;
                    if (m->kind != Mills::Kind::Institution) return m->settlement;
                    auto p = r.mills.find(mill);
                    if (p == r.mills.end() || p->second.parent.empty()) return 0;
                    mill = p->second.parent;
                }
                return 0;
            }
        };

        std::mutex             g_lock;
        std::int64_t           g_lastTick = -1;
        double                 g_lastNow = -1.0;
        double                 g_dt = 0.0;
        std::unique_ptr<Graph> g_graph;

        double FreshDays(const Rumours::Rumour& r, double n) {
            return r.freshDays ? *r.freshDays : Settings::BaseFreshDays() + Settings::FreshDaysPerNotability() * n;
        }

        /** Simulate ticks (from, to]: the hops decided, in order. */
        std::vector<Rumours::Arrival> Simulate(const Graph& G, std::vector<Rumours::Rumour>& rumours, std::int64_t from,
                                               std::int64_t to, double dt) {
            const double decayLocal = Settings::HopDecayLocal();
            const double decayTravel = Settings::HopDecayTravel();
            const double decayTravelPerN = Settings::DecayTravelPerNotability();
            const double floor = Settings::NotabilityFloor();
            const auto   maxMills = static_cast<std::size_t>(Settings::MaxMillsPerRumour());
            const bool   reach = Settings::ReachEnabled();

            std::vector<Rumours::Arrival> out;
            for (std::int64_t k = from + 1; k <= to; ++k) {
                const double t = double(k) * dt;
                for (auto& r : rumours) {
                    if (r.scope != Rumours::Scope::Spread) continue;
                    std::vector<std::string> spreading;
                    for (const auto& [g, p] : r.mills) {
                        if (p.arrived <= t && t <= p.freshUntil && p.notability >= floor) spreading.push_back(g);
                    }
                    for (const auto& g : spreading) {
                        // A secret leaves a mill only if someone there gossips more readily than it is kept,
                        // and a close mill keeps everything it hears so.
                        if (const double keep = std::max(r.secrecy, G.set->SecrecyOf(g)); keep > 0.0) {
                            auto mg = G.maxGossip.find(g);
                            if (mg == G.maxGossip.end() || mg->second <= keep) continue;
                        }
                        auto adj = G.adj.find(g);
                        if (adj == G.adj.end()) continue;
                        const Rumours::Presence from = r.mills.at(g);
                        // A shared member carries it on only once it has reached them here. Where it is
                        // their own people's account, or the mill is read out by another mod, it has.
                        const auto   fv = r.versions.find(g);
                        const bool   allHeard = !reach || G.set->IsEager(g) || (fv != r.versions.end() && fv->second.fact);
                        const double days = std::min(t, from.freshUntil) - from.arrived;
                        for (const auto& o : adj->second) {
                            if (r.mills.size() >= maxMills) break;
                            if (r.mills.contains(o.to)) continue;
                            double gossip = o.gossip;
                            if (o.shared && !allHeard) {
                                gossip = 0.0;
                                for (const auto& [roll, score] : *o.shared) {
                                    if (Rumours::HeardRoll(roll, r.id) < Rumours::Reach(from.notability, score, days)) gossip += score;
                                }
                            }
                            const double rate = Graph::Rate(o, from.notability, gossip);
                            if (rate <= 0.0) continue;
                            const double u = Mix(Fnv(o.to, Fnv(g, std::uint64_t(r.id) * 0x9E3779B97F4A7C15ull)), std::uint64_t(k));
                            if (u >= 1.0 - std::exp(-rate * dt)) continue;

                            // Within a place, or carried to another one, perhaps by a shared member.
                            const auto* h = G.Find(o.to);
                            if (!h) continue;
                            RE::FormID here = G.PlaceOf(r, g);
                            const bool walk = o.edge->kind == Mills::EdgeKind::Shared || o.edge->kind == Mills::EdgeKind::Strata;
                            // A guild or faction told directly has no place of its own: every member knows
                            // it wherever they live, so a shared member passes it on where they are.
                            const bool everywhere = !here && walk;
                            RE::FormID       there;
                            double           delay;
                            if (h->kind == Mills::Kind::Institution) {
                                there = here;
                                delay = Graph::EdgeDelay(*o.edge);
                            } else {
                                there = h->settlement;
                                if (everywhere) here = there;
                                delay = there == here || !walk ? Graph::EdgeDelay(*o.edge) : G.PlaceDelay(here, there);
                            }
                            // Big news keeps more of its size on the road: everyone carries it.
                            const double travel = std::min(1.0, decayTravel + decayTravelPerN * from.notability);
                            const double n = from.notability * ((there && there == here) || everywhere ? decayLocal : travel);
                            if (n < floor) continue;

                            Rumours::Arrival a;
                            a.id = r.id;
                            a.mill = o.to;
                            a.parent = g;
                            a.decided = t;
                            a.arrives = t + delay;
                            a.notability = n;
                            a.hops = from.hops + 1;
                            out.push_back(a);

                            Rumours::Presence p;
                            p.decided = a.decided;
                            p.arrived = a.arrives;
                            p.notability = n;
                            p.hops = a.hops;
                            p.parent = g;
                            p.freshUntil = a.arrives + FreshDays(r, n);
                            r.mills[o.to] = p;
                        }
                    }
                }
            }
            return out;
        }
    }

    void Reset() {
        std::lock_guard lk(g_lock);
        g_lastTick = -1;
        g_lastNow = -1.0;
    }

    void CatchUp(std::int32_t id, double from) {
        auto set = Mills::Current();
        if (!set) return;
        const double now = Rumours::Now();
        const double dt = Settings::HopTickHours() / 24.0;
        std::vector<Rumours::Arrival> hops;
        double                        announced;
        {
            std::lock_guard lk(g_lock);
            // Up to the last tick run; before the first tick of a session, up to now, which is where
            // that first tick starts counting.
            const auto to = g_lastTick >= 0 && g_dt == dt ? g_lastTick : static_cast<std::int64_t>(std::floor(now / dt));
            const auto start = static_cast<std::int64_t>(std::floor(from / dt));
            if (start >= to) return;
            std::vector<Rumours::Rumour> one;
            for (auto& r : Rumours::Snapshot()) {
                if (r.id == id) one.push_back(std::move(r));
            }
            if (one.empty()) return;
            if (!g_graph || g_graph->set != set || g_graph->placements != Mills::PlacementsVersion() ||
                g_graph->gossip != Gossip::Version()) {
                g_graph = std::make_unique<Graph>(set);
            }
            hops = Simulate(*g_graph, one, start, to, dt);
            announced = g_lastNow >= 0.0 ? g_lastNow : now;
        }
        Rumours::AddArrivals(hops);
        logger::info("Rumour #{}: {} hop(s) since day {:.2f}, caught up", id, hops.size(), from);
        // Later arrivals are announced by the regular tick.
        for (const auto& a : hops) {
            if (a.arrives <= announced) {
                Api::DispatchEvent("arrived", nlohmann::json{ { "type", "arrived" }, { "id", a.id }, { "mill", a.mill } }.dump());
            }
        }
    }

    namespace {
        double PlaceTravelDays(const Mills::Mill& a, const Mills::Mill& b) {
            if (a.settlement && a.settlement == b.settlement) return 0.1;
            if (!a.pos || !b.pos || a.pos->root != b.pos->root) return 2.0;
            return 0.25 + Distance(*a.pos, *b.pos) / kTravelCellsPerDay;
        }

        /** Where news reaches a mill: the place itself, or for a guild or faction, the towns its members
         *  live in, where they have settled since the build included. */
        std::vector<const Mills::Mill*> PlacesOf(const Mills::MillSet& set, const Mills::Mill& m) {
            if (m.kind != Mills::Kind::Institution) return { &m };
            std::vector<const Mills::Mill*> out;
            for (auto k : m.members) {
                const auto mills = set.MillsFor(k);
                if (mills.empty()) continue;
                const auto* home = set.Find(mills.front());
                if (home && home->kind != Mills::Kind::Institution && std::find(out.begin(), out.end(), home) == out.end()) {
                    out.push_back(home);
                }
            }
            return out;
        }
    }

    double TravelDays(const Mills::MillSet& set, const std::string& from, const std::string& to) {
        const auto* a = set.Find(from);
        const auto* b = set.Find(to);
        if (!a || !b) return 2.0;
        // A guild or faction has no place of its own: news gets there through its nearest member's town.
        double best = -1.0;
        for (const auto* x : PlacesOf(set, *a)) {
            for (const auto* y : PlacesOf(set, *b)) {
                const double d = PlaceTravelDays(*x, *y);
                if (best < 0.0 || d < best) best = d;
            }
        }
        return best < 0.0 ? 2.0 : best;
    }

    void Tick() {
        if (!Session::IsReady()) return;
        auto set = Mills::Current();
        if (!set) return;
        const double now = Rumours::Now();
        const double dt = Settings::HopTickHours() / 24.0;
        const auto   k = static_cast<std::int64_t>(std::floor(now / dt));

        double after;
        {
            std::lock_guard lk(g_lock);
            if (g_lastTick < 0 || dt != g_dt || now < g_lastNow) {
                // A fresh start: a load, or a changed tick length.
                g_lastTick = k;
                g_lastNow = now;
                g_dt = dt;
                return;
            }
            if (k > g_lastTick) {
                if (!g_graph || g_graph->set != set || g_graph->placements != Mills::PlacementsVersion() ||
                    g_graph->gossip != Gossip::Version()) {
                    g_graph = std::make_unique<Graph>(set);
                }
                const std::int64_t from = std::max(g_lastTick, k - kMaxTicksPerCall);
                auto               rumours = Rumours::Snapshot();
                const auto         hops = Simulate(*g_graph, rumours, from, k, dt);
                Rumours::AddArrivals(hops);
                g_lastTick = k;
            }
            after = g_lastNow;
            g_lastNow = now;
        }

        // What has arrived since the last call: an event for listeners, and a retelling
        // started at once if the player is there (or retell.mode is eager).
        const bool eager = Settings::RetellEager();
        for (const auto& [id, mill] : Rumours::ArrivalsBetween(after, now)) {
            Api::DispatchEvent("arrived", nlohmann::json{ { "type", "arrived" }, { "id", id }, { "mill", mill } }.dump());
            if (eager || Retell::InPlayersPlace(mill)) Retell::Request(id, mill);
        }
    }
}
