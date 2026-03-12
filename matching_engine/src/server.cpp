// server.cpp — Native HTTP server wrapping MatchingEngine
// Exposes the same JSON API as wasm_bridge.cpp over localhost:8765.
// Uses cpp-httplib (fetched via CMake FetchContent).

#include "shared_state.hpp"
#include "timer.hpp"

#include "httplib.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// ── Named simulation constants (item 33) ─────────────────────────────────────
// Price bounds for the random-walk mid price in /tick and /benchmark.
static constexpr int    PRICE_MIN        = 60;
static constexpr int    PRICE_MAX        = 160;
// Quantity ranges used in /tick workload generation.
static constexpr int    QTY_LIMIT_MAX    = 241;   // tick limit: 10 + rand % 241
static constexpr int    QTY_MARKET_MAX   = 91;    // tick market: 10 + rand % 91
// Quantity range used in /benchmark workload generation.
static constexpr int    QTY_BENCH_MAX    = 10;    // bench: 1 + rand % 10
// Probability thresholds for the three order types in /tick (cumulative).
static constexpr double PROB_LIMIT       = 0.55;
static constexpr double PROB_CANCEL      = 0.85;  // limit + cancel combined
static constexpr double PROB_MID_DOWN    = 0.30;
static constexpr double PROB_MID_UP      = 0.70;
// Batch size for /benchmark latency sampling.
// Note: benchmark_runner.cpp uses BATCH_SIZE=100 for per-op latency via
// high_resolution_clock per event; server uses 10000 for throughput measurement.
static constexpr int    BENCH_BATCH_SIZE = 10000;

// ── Server state (protected by mutex) ────────────────────────────────────────

static std::mutex  s_mu;
static SharedState s_state;

// ── JSON request parsing ──────────────────────────────────────────────────────

static std::optional<int64_t> json_int(const std::string& body, const char* key) {
    std::string search = std::string("\"") + key + "\":";
    auto pos = body.find(search);
    if (pos == std::string::npos) return {};
    pos += search.size();
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
    int64_t val = 0;
    auto [ptr, ec] = std::from_chars(body.data() + pos, body.data() + body.size(), val);
    if (ec != std::errc{}) return {};
    return val;
}

// ── CORS helper ───────────────────────────────────────────────────────────────

static void add_cors(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin",  "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
}

static void json_response(httplib::Response& res, const std::string& body) {
    add_cors(res);
    res.set_content(body, "application/json");
}

// ── Benchmark types and free function (item 32) ───────────────────────────────

struct BenchParams {
    int total_ops;
    int limit_pct;
    int cancel_pct;
    int market_pct;
};

struct BenchResult {
    uint64_t              throughput;
    uint64_t              p50_ns;
    uint64_t              p99_ns;
    uint64_t              p999_ns;
    double                elapsed_ms;
    std::vector<uint64_t> histogram;
};

static BenchResult run_benchmark(const BenchParams& p) {
    std::unique_ptr<MatchingEngine> beng = std::make_unique<MatchingEngine>();
    std::vector<Fill> bfills;
    std::vector<int>  blive;
    int bmid = 100;

    init_engine_and_book(*beng, bfills);           // item 29
    for (int i = 1; i <= 16; ++i) blive.push_back(i);

    double   lf   = p.limit_pct  / 100.0;
    double   cft  = lf + p.cancel_pct  / 100.0;
    double   mft  = cft + p.market_pct / 100.0;   // item 24: market_pct now explicit
    uint64_t brng = 0xDEADBEEFULL;
    int alt = 0;

    std::vector<uint64_t> lats;

    ScopedTimer outer_tmr;                         // item 28
    for (int op = 0; op < p.total_ops; op += BENCH_BATCH_SIZE) {
        int bn = std::min(BENCH_BATCH_SIZE, p.total_ops - op);
        ScopedTimer batch_tmr;                     // item 28
        for (int i = 0; i < bn; ++i) {
            double r = double(lcg_step(brng) >> 11) * (1.0 / double(1ULL << 53));  // item 30
            if (r < lf) {
                Side s   = (alt++ & 1) ? Side::Sell : Side::Buy;
                int  off = static_cast<int>(lcg_step(brng) % 7);
                int  px  = (s == Side::Buy) ? std::max(1, bmid - off) : bmid + off;
                int  qty = 1 + static_cast<int>(lcg_step(brng) % QTY_BENCH_MAX);
                int  nid = static_cast<int>(beng->peek_next_id());
                bfills.clear(); beng->submit_limit(s, uint64_t(px), uint64_t(qty), bfills);
                uint64_t filled = 0;
                for (auto& f : bfills) filled += f.quantity;
                if (filled < uint64_t(qty)) blive.push_back(nid);
                double r2 = double(lcg_step(brng) >> 11) * (1.0 / double(1ULL << 53));
                if      (r2 < PROB_MID_DOWN && bmid > PRICE_MIN) --bmid;
                else if (r2 > PROB_MID_UP   && bmid < PRICE_MAX) ++bmid;
            } else if (r < cft) {
                if (!blive.empty()) {
                    int idx = static_cast<int>(lcg_step(brng) % blive.size());
                    int cid = blive[idx];
                    beng->cancel(uint64_t(cid));
                    blive[idx] = blive.back(); blive.pop_back();
                }
            } else if (r < mft) {              // item 24: bounded by market_pct
                Side s   = (alt++ & 1) ? Side::Sell : Side::Buy;
                int  qty = 1 + static_cast<int>(lcg_step(brng) % QTY_BENCH_MAX);
                bfills.clear(); beng->submit_market(s, uint64_t(qty), bfills);
                if (beng->book().bids().empty()) {
                    bfills.clear();
                    int nid = static_cast<int>(beng->peek_next_id());
                    beng->submit_limit(Side::Buy, bmid - 1, 100, bfills);
                    blive.push_back(nid);
                }
                if (beng->book().asks().empty()) {
                    bfills.clear();
                    int nid = static_cast<int>(beng->peek_next_id());
                    beng->submit_limit(Side::Sell, bmid + 1, 100, bfills);
                    blive.push_back(nid);
                }
            }
        }
        lats.push_back(batch_tmr.elapsed_ns() / uint64_t(bn));  // item 28
    }

    BenchResult res;
    res.elapsed_ms = double(outer_tmr.elapsed_ns()) / 1'000'000.0;  // item 28
    res.throughput = res.elapsed_ms > 0.0
        ? static_cast<uint64_t>(double(p.total_ops) / (res.elapsed_ms / 1000.0)) : 0;

    std::vector<uint64_t> sorted = lats;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double q) -> uint64_t {
        if (sorted.empty()) return 0;
        size_t idx = std::min(
            static_cast<size_t>(std::lround(q * double(sorted.size() - 1))),
            sorted.size() - 1);
        return sorted[idx];
    };
    res.p50_ns  = pct(0.50);
    res.p99_ns  = pct(0.99);
    res.p999_ns = pct(0.999);
    res.histogram = std::move(lats);
    return res;
}

// ── Tick helpers (item 31) ────────────────────────────────────────────────────

static void maintain_liquidity(SharedState& st) {
    if (st.eng->book().bids().empty()) {
        st.fills.clear();
        int nid = static_cast<int>(st.eng->peek_next_id());
        st.eng->submit_limit(Side::Buy, st.tick_mid - 1, 200, st.fills);
        st.live_ids.insert(nid);
    }
    if (st.eng->book().asks().empty()) {
        st.fills.clear();
        int nid = static_cast<int>(st.eng->peek_next_id());
        st.eng->submit_limit(Side::Sell, st.tick_mid + 1, 200, st.fills);
        st.live_ids.insert(nid);
    }
}

static void dispatch_workload_tick(SharedState& st, std::vector<Fill>& tick_fills, int n_batches) {
    for (int i = 0; i < n_batches; ++i) {
        double r = double(next_rand(st) >> 11) * (1.0 / double(1ULL << 53));
        std::vector<Fill> step;

        if (r < PROB_LIMIT) {
            Side side   = (next_rand(st) & 1) ? Side::Sell : Side::Buy;
            int  offset = static_cast<int>(next_rand(st) % 7);
            int  px     = (side == Side::Buy)
                          ? std::max(1, st.tick_mid - offset)
                          : st.tick_mid + offset;
            int  qty    = 10 + static_cast<int>(next_rand(st) % QTY_LIMIT_MAX);
            int  nid    = static_cast<int>(st.eng->peek_next_id());
            st.eng->submit_limit(side, uint64_t(px), uint64_t(qty), step);
            ++st.total_orders;
            uint64_t filled = 0;
            for (auto& f : step) {
                filled += f.quantity;
                add_trade(st, f.price, f.quantity, side == Side::Buy);
                remove_live(st, static_cast<int>(f.maker_id));
                tick_fills.push_back(f);
            }
            if (filled < uint64_t(qty)) st.live_ids.insert(nid);
            double r2 = double(next_rand(st) >> 11) * (1.0 / double(1ULL << 53));
            if      (r2 < PROB_MID_DOWN && st.tick_mid > PRICE_MIN) --st.tick_mid;
            else if (r2 > PROB_MID_UP   && st.tick_mid < PRICE_MAX) ++st.tick_mid;
        } else if (r < PROB_CANCEL) {
            if (!st.live_ids.empty()) {
                auto it = std::next(st.live_ids.begin(),
                                    static_cast<ptrdiff_t>(next_rand(st) % st.live_ids.size()));
                int cid = *it;
                if (st.eng->cancel(uint64_t(cid))) {
                    st.live_ids.erase(it);
                }
            }
        } else {
            Side side = (next_rand(st) & 1) ? Side::Sell : Side::Buy;
            int  qty  = 10 + static_cast<int>(next_rand(st) % QTY_MARKET_MAX);
            st.eng->submit_market(side, uint64_t(qty), step);
            ++st.total_orders;
            for (auto& f : step) {
                add_trade(st, f.price, f.quantity, side == Side::Buy);
                remove_live(st, static_cast<int>(f.maker_id));
                tick_fills.push_back(f);
            }
        }
    }
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    init_engine(s_state);

    httplib::Server svr;

    // Handle CORS preflight
    svr.Options(".*", [](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        res.status = 204;
    });

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        res.set_content("{\"ok\":true,\"mode\":\"native\"}", "application/json");
    });

    svr.Post("/submit_limit", [](const httplib::Request& req, httplib::Response& res) {
        auto sv = json_int(req.body, "side");
        auto pv = json_int(req.body, "price");
        auto qv = json_int(req.body, "qty");
        if (!sv || !pv || !qv) { add_cors(res); res.status = 400; return; }

        int64_t side_raw  = *sv;
        int64_t price_raw = *pv;
        int64_t qty_raw   = *qv;
        if (side_raw != 0 && side_raw != 1) {
            add_cors(res); res.status = 400;
            res.set_content(R"({"error":"invalid side"})", "application/json");
            return;
        }
        if (price_raw <= 0 || qty_raw <= 0) {
            add_cors(res); res.status = 400;
            res.set_content(R"({"error":"price and qty must be > 0"})", "application/json");
            return;
        }

        std::lock_guard<std::mutex> lock(s_mu);
        Side     side  = side_raw == 0 ? Side::Buy : Side::Sell;
        uint64_t price = static_cast<uint64_t>(price_raw);
        uint64_t qty   = static_cast<uint64_t>(qty_raw);

        int assigned_id = static_cast<int>(s_state.eng->peek_next_id());
        ScopedTimer tmr;                           // item 28
        s_state.fills.clear();
        s_state.eng->submit_limit(side, price, qty, s_state.fills);
        uint64_t elapsed_ns = tmr.elapsed_ns();   // item 28

        ++s_state.total_orders;
        uint64_t filled = 0;
        for (auto& f : s_state.fills) {
            filled += f.quantity;
            add_trade(s_state, f.price, f.quantity, side == Side::Buy);
            remove_live(s_state, static_cast<int>(f.maker_id));
        }
        if (filled < qty) s_state.live_ids.insert(assigned_id);

        json_response(res, build_response(s_state, assigned_id, elapsed_ns, s_state.fills));
    });

    svr.Post("/submit_market", [](const httplib::Request& req, httplib::Response& res) {
        auto sv = json_int(req.body, "side");
        auto qv = json_int(req.body, "qty");
        if (!sv || !qv) { add_cors(res); res.status = 400; return; }

        int64_t side_raw = *sv;
        int64_t qty_raw  = *qv;
        if (side_raw != 0 && side_raw != 1) {
            add_cors(res); res.status = 400;
            res.set_content(R"({"error":"invalid side"})", "application/json");
            return;
        }
        if (qty_raw <= 0) {
            add_cors(res); res.status = 400;
            res.set_content(R"({"error":"price and qty must be > 0"})", "application/json");
            return;
        }

        std::lock_guard<std::mutex> lock(s_mu);
        Side     side = side_raw == 0 ? Side::Buy : Side::Sell;
        uint64_t qty  = static_cast<uint64_t>(qty_raw);

        ScopedTimer tmr;                           // item 28
        s_state.fills.clear();
        s_state.eng->submit_market(side, qty, s_state.fills);
        uint64_t elapsed_ns = tmr.elapsed_ns();   // item 28

        ++s_state.total_orders;
        for (auto& f : s_state.fills) {
            add_trade(s_state, f.price, f.quantity, side == Side::Buy);
            remove_live(s_state, static_cast<int>(f.maker_id));
        }
        json_response(res, build_response(s_state, 0, elapsed_ns, s_state.fills));
    });

    svr.Post("/cancel", [](const httplib::Request& req, httplib::Response& res) {
        auto iv = json_int(req.body, "id");
        if (!iv) { add_cors(res); res.status = 400; return; }

        std::lock_guard<std::mutex> lock(s_mu);
        int id = static_cast<int>(*iv);
        ScopedTimer tmr;                           // item 28
        bool ok = s_state.eng->cancel(static_cast<uint64_t>(id));
        uint64_t elapsed_ns = tmr.elapsed_ns();   // item 28

        if (ok) remove_live(s_state, id);
        std::vector<Fill> no_fills;
        json_response(res, build_response(s_state, ok ? id : 0, elapsed_ns, no_fills));
    });

    // item 31: /tick lambda reduced to ~10 lines; logic in maintain_liquidity +
    // dispatch_workload_tick.
    svr.Post("/tick", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(s_mu);
        maintain_liquidity(s_state);
        ScopedTimer tmr;                           // item 28
        std::vector<Fill> tick_fills;
        int batch = 1 + static_cast<int>(next_rand(s_state) % 5);
        dispatch_workload_tick(s_state, tick_fills, batch);
        uint64_t elapsed_ns = tmr.elapsed_ns();   // item 28
        json_response(res, build_response(s_state, 0, elapsed_ns, tick_fills));
    });

    // item 32: /benchmark lambda reduced to parse → call → serialize.
    svr.Post("/benchmark", [](const httplib::Request& req, httplib::Response& res) {
        auto ops_v = json_int(req.body, "ops");
        auto lp_v  = json_int(req.body, "lp");
        auto cp_v  = json_int(req.body, "cp");
        auto mp_v  = json_int(req.body, "mp");
        if (!ops_v || !lp_v || !cp_v || !mp_v) { add_cors(res); res.status = 400; return; }

        BenchParams bp;
        bp.total_ops  = static_cast<int>(*ops_v);
        bp.limit_pct  = static_cast<int>(*lp_v);
        bp.cancel_pct = static_cast<int>(*cp_v);
        bp.market_pct = static_cast<int>(*mp_v);

        BenchResult r = run_benchmark(bp);

        std::string hist = "[";
        for (size_t i = 0; i < r.histogram.size(); ++i) {
            if (i) hist += ',';
            hist += std::to_string(r.histogram[i]);
        }
        hist += ']';

        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "{\"throughput\":%llu,\"p50_ns\":%llu,\"p99_ns\":%llu,\"p999_ns\":%llu,"
            "\"elapsed_ms\":%.1f,\"mode\":\"native\"",
            (unsigned long long)r.throughput,
            (unsigned long long)r.p50_ns,
            (unsigned long long)r.p99_ns,
            (unsigned long long)r.p999_ns,
            r.elapsed_ms);
        json_response(res, std::string(buf) + ",\"histogram\":" + hist + '}');
    });

    svr.Post("/reset", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(s_mu);
        init_engine(s_state);
        add_cors(res);
        res.set_content("{\"ok\":true}", "application/json");
    });

    // item 35: read port and bind address from environment.
    // Safer default is loopback (127.0.0.1), not 0.0.0.0.
    const char* port_env = std::getenv("LOBE_PORT");
    const char* bind_env = std::getenv("LOBE_BIND");
    int         port     = port_env ? std::atoi(port_env) : 8765;
    std::string bind_addr = bind_env ? bind_env : "127.0.0.1";

    std::printf("engine_server listening on http://%s:%d\n", bind_addr.c_str(), port);
    std::printf("Press Ctrl+C to stop.\n");
    svr.listen(bind_addr.c_str(), port);
    return 0;
}
