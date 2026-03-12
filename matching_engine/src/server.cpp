// server.cpp — Native HTTP server wrapping MatchingEngine
// Exposes the same JSON API as wasm_bridge.cpp over localhost:8765.
// Uses cpp-httplib (fetched via CMake FetchContent).

#include "shared_state.hpp"

#include "httplib.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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

        int64_t side_raw = *sv;
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
        auto t0 = std::chrono::high_resolution_clock::now();
        s_state.fills.clear();
        s_state.eng->submit_limit(side, price, qty, s_state.fills);
        auto t1 = std::chrono::high_resolution_clock::now();
        uint64_t elapsed_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1-t0).count());

        ++s_state.total_orders;
        uint64_t filled = 0;
        for (auto& f : s_state.fills) {
            filled += f.quantity;
            add_trade(s_state, f.price, f.quantity, side == Side::Buy);
            remove_live(s_state, static_cast<int>(f.maker_id));
        }
        if (filled < qty) s_state.live_ids.push_back(assigned_id);

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

        auto t0 = std::chrono::high_resolution_clock::now();
        s_state.fills.clear();
        s_state.eng->submit_market(side, qty, s_state.fills);
        auto t1 = std::chrono::high_resolution_clock::now();
        uint64_t elapsed_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1-t0).count());

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
        auto t0 = std::chrono::high_resolution_clock::now();
        bool ok = s_state.eng->cancel(static_cast<uint64_t>(id));
        auto t1 = std::chrono::high_resolution_clock::now();
        uint64_t elapsed_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1-t0).count());

        if (ok) remove_live(s_state, id);
        std::vector<Fill> no_fills;
        json_response(res, build_response(s_state, ok ? id : 0, elapsed_ns, no_fills));
    });

    svr.Post("/tick", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(s_mu);

        if (s_state.eng->book().bids().empty()) {
            s_state.fills.clear();
            int nid = static_cast<int>(s_state.eng->peek_next_id());
            s_state.eng->submit_limit(Side::Buy, s_state.tick_mid-1, 200, s_state.fills);
            s_state.live_ids.push_back(nid);
        }
        if (s_state.eng->book().asks().empty()) {
            s_state.fills.clear();
            int nid = static_cast<int>(s_state.eng->peek_next_id());
            s_state.eng->submit_limit(Side::Sell, s_state.tick_mid+1, 200, s_state.fills);
            s_state.live_ids.push_back(nid);
        }

        auto t0 = std::chrono::high_resolution_clock::now();
        int batch = 1 + static_cast<int>(next_rand(s_state) % 5);
        std::vector<Fill> tick_fills;

        for (int i = 0; i < batch; ++i) {
            double r = double(next_rand(s_state) >> 11) * (1.0 / double(1ULL << 53));
            std::vector<Fill> step;

            if (r < 0.55) {
                Side side   = (next_rand(s_state) & 1) ? Side::Sell : Side::Buy;
                int  offset = static_cast<int>(next_rand(s_state) % 7);
                int  px     = (side == Side::Buy)
                              ? std::max(1, s_state.tick_mid - offset)
                              : s_state.tick_mid + offset;
                int  qty    = 10 + static_cast<int>(next_rand(s_state) % 241);
                int  nid    = static_cast<int>(s_state.eng->peek_next_id());
                s_state.eng->submit_limit(side, uint64_t(px), uint64_t(qty), step);
                ++s_state.total_orders;
                uint64_t filled = 0;
                for (auto& f : step) {
                    filled += f.quantity;
                    add_trade(s_state, f.price, f.quantity, side == Side::Buy);
                    remove_live(s_state, static_cast<int>(f.maker_id));
                    tick_fills.push_back(f);
                }
                if (filled < uint64_t(qty)) s_state.live_ids.push_back(nid);
                double r2 = double(next_rand(s_state) >> 11) * (1.0 / double(1ULL << 53));
                if      (r2 < 0.30 && s_state.tick_mid > 60)  --s_state.tick_mid;
                else if (r2 > 0.70 && s_state.tick_mid < 160) ++s_state.tick_mid;
            } else if (r < 0.85) {
                if (!s_state.live_ids.empty()) {
                    int idx = static_cast<int>(next_rand(s_state) % s_state.live_ids.size());
                    int cid = s_state.live_ids[idx];
                    if (s_state.eng->cancel(uint64_t(cid))) {
                        s_state.live_ids[idx] = s_state.live_ids.back();
                        s_state.live_ids.pop_back();
                    }
                }
            } else {
                Side side = (next_rand(s_state) & 1) ? Side::Sell : Side::Buy;
                int  qty  = 10 + static_cast<int>(next_rand(s_state) % 91);
                s_state.eng->submit_market(side, uint64_t(qty), step);
                ++s_state.total_orders;
                for (auto& f : step) {
                    add_trade(s_state, f.price, f.quantity, side == Side::Buy);
                    remove_live(s_state, static_cast<int>(f.maker_id));
                    tick_fills.push_back(f);
                }
            }
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        uint64_t elapsed_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1-t0).count());

        json_response(res, build_response(s_state, 0, elapsed_ns, tick_fills));
    });

    svr.Post("/benchmark", [](const httplib::Request& req, httplib::Response& res) {
        auto ops_v = json_int(req.body, "ops");
        auto lp_v  = json_int(req.body, "lp");
        auto cp_v  = json_int(req.body, "cp");
        auto mp_v  = json_int(req.body, "mp");
        if (!ops_v || !lp_v || !cp_v || !mp_v) { add_cors(res); res.status = 400; return; }

        int total_ops  = static_cast<int>(*ops_v);
        int limit_pct  = static_cast<int>(*lp_v);
        int cancel_pct = static_cast<int>(*cp_v);

        std::unique_ptr<MatchingEngine> beng = std::make_unique<MatchingEngine>();
        std::vector<Fill>  bfills;
        std::vector<int>   blive;
        int bmid = 100;

        for (int d = 1; d <= 8; ++d) {
            bfills.clear(); beng->submit_limit(Side::Buy,  100-d, 200, bfills);
            bfills.clear(); beng->submit_limit(Side::Sell, 100+d, 200, bfills);
        }
        for (int i = 1; i <= 16; ++i) blive.push_back(i);

        double lf  = limit_pct  / 100.0;
        double cft = lf + cancel_pct / 100.0;
        uint64_t brng = 0xDEADBEEFULL;
        auto brand = [&]() -> uint64_t {
            brng = brng * 6364136223846793005ULL + 1442695040888963407ULL;
            return brng;
        };

        const int BATCH = 10000;
        std::vector<uint64_t> lats;
        int alt = 0;

        auto tstart = std::chrono::high_resolution_clock::now();
        for (int op = 0; op < total_ops; op += BATCH) {
            int bn = std::min(BATCH, total_ops - op);
            auto t0 = std::chrono::high_resolution_clock::now();
            for (int i = 0; i < bn; ++i) {
                double r = double(brand() >> 11) * (1.0 / double(1ULL << 53));
                if (r < lf) {
                    Side s   = (alt++ & 1) ? Side::Sell : Side::Buy;
                    int  off = static_cast<int>(brand() % 7);
                    int  px  = (s == Side::Buy) ? std::max(1, bmid-off) : bmid+off;
                    int  qty = 1 + static_cast<int>(brand() % 10);
                    int  nid = static_cast<int>(beng->peek_next_id());
                    bfills.clear(); beng->submit_limit(s, uint64_t(px), uint64_t(qty), bfills);
                    uint64_t filled = 0;
                    for (auto& f : bfills) filled += f.quantity;
                    if (filled < uint64_t(qty)) blive.push_back(nid);
                    double r2 = double(brand() >> 11) * (1.0 / double(1ULL << 53));
                    if      (r2 < 0.3 && bmid > 60)  --bmid;
                    else if (r2 > 0.7 && bmid < 160) ++bmid;
                } else if (r < cft) {
                    if (!blive.empty()) {
                        int idx = static_cast<int>(brand() % blive.size());
                        int cid = blive[idx];
                        beng->cancel(uint64_t(cid));
                        blive[idx] = blive.back(); blive.pop_back();
                    }
                } else {
                    Side s   = (alt++ & 1) ? Side::Sell : Side::Buy;
                    int  qty = 1 + static_cast<int>(brand() % 10);
                    bfills.clear(); beng->submit_market(s, uint64_t(qty), bfills);
                    if (beng->book().bids().empty()) {
                        bfills.clear();
                        int nid = static_cast<int>(beng->peek_next_id());
                        beng->submit_limit(Side::Buy, bmid-1, 100, bfills);
                        blive.push_back(nid);
                    }
                    if (beng->book().asks().empty()) {
                        bfills.clear();
                        int nid = static_cast<int>(beng->peek_next_id());
                        beng->submit_limit(Side::Sell, bmid+1, 100, bfills);
                        blive.push_back(nid);
                    }
                }
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            uint64_t ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1-t0).count());
            lats.push_back(ns / uint64_t(bn));
        }

        auto tend = std::chrono::high_resolution_clock::now();
        double elapsed_ms = double(
            std::chrono::duration_cast<std::chrono::microseconds>(tend-tstart).count()) / 1000.0;

        std::vector<uint64_t> sorted = lats;
        std::sort(sorted.begin(), sorted.end());
        auto pct = [&](double p) -> uint64_t {
            if (sorted.empty()) return 0;
            size_t idx = std::min(
                static_cast<size_t>(std::lround(p * double(sorted.size()-1))),
                sorted.size()-1);
            return sorted[idx];
        };
        uint64_t throughput = elapsed_ms > 0.0
            ? static_cast<uint64_t>(double(total_ops) / (elapsed_ms / 1000.0)) : 0;

        std::string hist = "[";
        for (size_t i = 0; i < lats.size(); ++i) {
            if (i) hist += ',';
            hist += std::to_string(lats[i]);
        }
        hist += ']';

        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "{\"throughput\":%llu,\"p50_ns\":%llu,\"p99_ns\":%llu,\"p999_ns\":%llu,"
            "\"elapsed_ms\":%.1f,\"mode\":\"native\"",
            (unsigned long long)throughput,
            (unsigned long long)pct(0.50),
            (unsigned long long)pct(0.99),
            (unsigned long long)pct(0.999),
            elapsed_ms);
        std::string result = std::string(buf) + ",\"histogram\":" + hist + '}';
        json_response(res, result);
    });

    svr.Post("/reset", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(s_mu);
        init_engine(s_state);
        add_cors(res);
        res.set_content("{\"ok\":true}", "application/json");
    });

    std::printf("engine_server listening on http://localhost:8765\n");
    std::printf("Press Ctrl+C to stop.\n");
    svr.listen("0.0.0.0", 8765);
    return 0;
}
