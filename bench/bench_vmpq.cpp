// VMP-07：VMPQ 基准测试（**参数化 + 机器可读输出**版）。
//
// 与旧版的差别（旧版只有 `--quick`，l / λ / N / seed / 谓词 / 重复次数全部硬编码，无 JSON）：
//   * 全部轴可配：`--rows` / `--attr-bits` / `--lambda` / `--seed` / `--pred-value` /
//     `--repeat` / `--warmup` / `--no-refresh`
//   * **区间查询**：`--range-width K`（+ `--range-lo`）—— ⚠️ 口径是
//     "**VMPQ 论文只原生支持点查询，区间用多点查询拼**"：对区间内每个取值各取一次
//     one-hot 列，客户端本地按位或。代价 = K 个查询集，与 MPRAQ 的原生 LCTE
//     （区间恒占 2 列）正是参数表里要对照的那一条。区间结果与**明文基准逐值断言**。
//   * `--mode local|grpc`：`grpc` = **真实两进程**（回环 socket，需先起两台 `vmpq_server`，
//     用 `--server0/--server1` 给端点）⇒ 这条路径才有**上行/下行字节**（实测）
//   * `--json`（JSONL → stdout）/ `--json-out FILE`（完整文档）
//     **`points[]` 里每条 = 一次运行**（含 `is_warmup`），统计交给外部分析脚本
//   * 一个人读表（主表）+ 一张 SUM 次数对照表；`--refresh-cost` 额外跑 D17 的开关对照
//
// 口径（决策 D38，2026-09-11）：**一个 DB 条目 = 一整列 = N 个 128 位 cell**
//   ⇒ 一次列查询 = **1 次 PIR**；服务器应答 N 个元素、存储 padded_columns·N·16 B/台；
//   hint parity 是 N 个分量（每条 hint N·16 B + 21 B 元数据）。
//
// 运行：./build/bin/bench_vmpq [--quick] [--rows 512,1024] [--attr-bits 6] [--lambda 24]
//        [--repeat 5] [--warmup 1] [--json-out /tmp/vmpq.json]
//
// ⚠️ `--mode grpc` 下**不能**用 `client.node(i)`（远程模式会抛异常）⇒ 服务器侧计数器
//    （`queries_served`）只有 local 口径有；grpc 口径用**通道实测**的 `rpc_count` 与
//    `sent_bytes`/`recv_bytes` 代替，并在 JSON 里把两者分开命名，绝不混报。

#include "vmpq/vmpq.hpp"

#include "core/random.hpp"
#include "net/grpc_vmpq.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace tsb;
using Clock = std::chrono::steady_clock;

namespace {

double MsSince(const Clock::time_point& t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ---------------------------------------------------------------------------
// 最小 JSON 发射器（与 `bench_mpraq` 的用法保持一致，但各自独立、互不依赖）
// ---------------------------------------------------------------------------
std::string JsonEsc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default: o += c;
        }
    }
    return o;
}

class Json {
public:
    // ⚠️ 必须在构造函数体里写 `{`：`std::ostringstream os_{"{"}` 那种成员初始化
    //    在本环境**不会**把内容置成 "{"（实测输出的对象少了左花括号 ⇒ JSON 非法）。
    Json() { os_ << "{"; }
    Json& Sep() {
        if (!first_) os_ << ",";
        first_ = false;
        return *this;
    }
    Json& Str(const char* k, const std::string& v) {
        Sep();
        os_ << "\"" << k << "\":\"" << JsonEsc(v) << "\"";
        return *this;
    }
    Json& U64(const char* k, uint64_t v) {
        Sep();
        os_ << "\"" << k << "\":" << v;
        return *this;
    }
    Json& I64(const char* k, int64_t v) {
        Sep();
        os_ << "\"" << k << "\":" << v;
        return *this;
    }
    Json& Dbl(const char* k, double v, int prec) {
        Sep();
        std::ostringstream t;
        t.precision(prec);
        t << std::fixed << v;
        os_ << "\"" << k << "\":" << t.str();
        return *this;
    }
    Json& Bool(const char* k, bool v) {
        Sep();
        os_ << "\"" << k << "\":" << (v ? "true" : "false");
        return *this;
    }
    Json& Raw(const char* k, const std::string& raw) {
        Sep();
        os_ << "\"" << k << "\":" << raw;
        return *this;
    }
    std::string Done() {
        os_ << "}";
        return os_.str();
    }

private:
    std::ostringstream os_;
    bool first_ = true;
};

// ---------------------------------------------------------------------------
// 选项
// ---------------------------------------------------------------------------
struct Options {
    bool quick = false;
    bool has_rows = false;
    std::vector<uint32_t> rows{512, 1024, 2048, 4096};
    uint32_t attr_bits = 6;        // l（每个属性的值域 = 2^l）
    uint32_t lambda = 24;          // 主表旧口径 24；论文取值 80
    uint64_t seed = 7;
    uint64_t pred_value = 3;       // 谓词阈值（会 mod 值域）
    // 区间查询（**非原生**：VMPQ 论文只原生支持点查询，区间靠多点查询拼）
    uint32_t range_width = 0;      // 0 = 不跑区间；>0 ⇒ 区间 [lo, lo+width-1]
    uint64_t range_lo = 1;         // 区间下界
    // R3：**一次 Init 内连跑 R 个连续区间**（每区间宽 k），用于与 MPRAQ/Waldo 的
    // "8 个谓词"对齐。**不重做 Init**（9 GiB / 400 s 重做 8 次不可行）。
    uint32_t range_count = 1;
    uint64_t range_budget_mb = 256;  // 分块 OR 的内存预算（峰值 ≈ 预算 + N 字节）
    // Init 上传量预检（grpc 口径）：`2 × padded_columns × N × 16 B`。
    // ⚠️ 这条是**真会咬人**的：l=6（padded 256）、N=2^18 时是 2 GiB；
    //    l=8（padded 1024）、N=2^18 时是 **8 GiB**（实测 N=4096/l=8 已 144 MiB / 7–10 s）。
    //    VMPQ 的 Init 是 O(2^l · N)，与 MPRAQ 的 O(m·entry_words) 完全不是一个量级
    //    ⇒ 大 N 必须配小 l，否则会在"什么都没测到"之前先花几十分钟上传。
    uint64_t init_budget_mb = 4096;  // 默认 4 GiB
    bool allow_huge_init = false;    // 显式放行超预算的 Init
    uint32_t repeat = 5;           // 测量运行次数（每次运行一条独立记录）
    uint32_t warmup = 1;           // 预热次数（同样产出记录，标记 is_warmup）
    bool no_refresh = false;       // D17 的开关：关闭 hint 刷新（**不保证隐私**，仅基准）
    bool refresh_cost = false;     // 额外跑一遍开关对照表
    std::string mode = "local";    // local | grpc
    std::string server0, server1;  // grpc 模式必填
    bool json = false;
    std::string json_out;
    bool verbose = false;
};

std::vector<uint32_t> ParseU32List(const std::string& s) {
    std::vector<uint32_t> out;
    std::string cur;
    std::istringstream is(s);
    while (std::getline(is, cur, ',')) {
        if (cur.empty()) continue;
        out.push_back(static_cast<uint32_t>(std::stoul(cur)));
    }
    if (out.empty()) throw std::invalid_argument("列表不能为空");
    return out;
}

Options ParseArgs(int argc, char** argv) {
    Options o;
    auto next = [&](int& i) -> std::string {
        if (i + 1 >= argc) throw std::invalid_argument("选项缺少参数");
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--quick") {
            o.quick = true;
        } else if (a == "--rows") {
            o.rows = ParseU32List(next(i));
            o.has_rows = true;
        } else if (a == "--attr-bits") {
            o.attr_bits = static_cast<uint32_t>(std::stoul(next(i)));
        } else if (a == "--lambda") {
            o.lambda = static_cast<uint32_t>(std::stoul(next(i)));
        } else if (a == "--seed") {
            o.seed = std::stoull(next(i));
        } else if (a == "--pred-value") {
            o.pred_value = std::stoull(next(i));
        } else if (a == "--range-width") {
            o.range_width = static_cast<uint32_t>(std::stoul(next(i)));
        } else if (a == "--range-count") {
            // R3：一次 Init 内连跑的区间个数（默认 1 = 旧行为）
            o.range_count = static_cast<uint32_t>(std::stoul(next(i)));
            if (o.range_count == 0) throw std::invalid_argument("--range-count 必须 >= 1");
        } else if (a == "--range-lo") {
            o.range_lo = std::stoull(next(i));
        } else if (a == "--range-budget-mb") {
            o.range_budget_mb = std::stoull(next(i));
        } else if (a == "--init-budget-mb") {
            o.init_budget_mb = std::stoull(next(i));
        } else if (a == "--allow-huge-init") {
            o.allow_huge_init = true;
        } else if (a == "--repeat") {
            o.repeat = static_cast<uint32_t>(std::stoul(next(i)));
        } else if (a == "--warmup") {
            o.warmup = static_cast<uint32_t>(std::stoul(next(i)));
        } else if (a == "--no-refresh") {
            o.no_refresh = true;
        } else if (a == "--refresh-cost") {
            o.refresh_cost = true;
        } else if (a == "--mode") {
            o.mode = next(i);
            if (o.mode != "local" && o.mode != "grpc") {
                throw std::invalid_argument("--mode 只认 local|grpc");
            }
        } else if (a == "--server0") {
            o.server0 = next(i);
        } else if (a == "--server1") {
            o.server1 = next(i);
        } else if (a == "--json") {
            o.json = true;
        } else if (a == "--json-out") {
            o.json_out = next(i);
        } else if (a == "--verbose" || a == "-v") {
            o.verbose = true;
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "用法: bench_vmpq [--quick] [--rows 512,1024,...]\n"
                "       [--attr-bits 6] [--lambda 24] [--seed 7] [--pred-value 3]\n"
                "       [--repeat 5] [--warmup 1] [--no-refresh] [--refresh-cost]\n"
                "       [--mode local|grpc] [--server0 host:port --server1 host:port]\n"
                "       [--json] [--json-out FILE] [--verbose]\n"
                "\n"
                "  --repeat R    **测量**运行次数（默认 5）：每次运行产出**一条独立记录**，\n"
                "                统计（均值/标准差/p50/p95）交给外部分析脚本\n"
                "  --warmup W    预热运行次数（默认 1）：产出记录但标记 is_warmup，由脚本排除\n"
                "  --range-width K  区间查询宽度（默认 0 = 不跑）：区间 = [lo, lo+K-1]。\n"
                "                ⚠️ **非原生**：VMPQ 论文只原生支持点查询，区间靠"
                "「区间内每个取值各做一次点查询 + 客户端本地按位或」拼出来\n"
                "                ⇒ 代价 = K 个查询集（论文口径的 |filter|），下行 ≈ K·N·16 B/台。\n"
                "                内部按 --range-budget-mb 分块 OR 限制峰值内存\n"
                "  --range-lo L  区间下界（默认 1；需 1 <= L 且 L+K-1 <= 2^l - 1）\n"
                "  --range-budget-mb MB  分块 OR 的内存预算（默认 256）\n"
                "  --init-budget-mb MB   Init 上传量上限（默认 4096 = 4 GiB；grpc 口径）\n"
                "                ⚠️ Init 上传 = 2·padded_columns·N·16 B，而 padded_columns ∝ 2^l。\n"
                "                ⚠️ 但 l 也不能太小：`DerivePirParams` 要求分区数 P >= 16\n"
                "                ⇒ 双属性下 **l >= 5**（l=4 时 padded=64、P=8，直接拒绝）。\n"
                "                参考（N=2^18）：l=5 → 1 GiB；l=6 → 2 GiB；l=8 → 8 GiB。\n"
                "                超上限**拒绝运行**（退出码 3），除非给 --allow-huge-init\n"
                "  --allow-huge-init     放行超预算的 Init（会跑很久，慎用）\n"
                "  --mode grpc   真实两进程（回环）；**必须先起两台 vmpq_server** 并给出端点。\n"
                "                这条路径才有实测的上/下行字节（本地模式恒 0，语义如此）\n"
                "  --rows N,...  记录数（= window_size = 每列的 cell 数）。⚠️ `--quick` 只覆盖\n"
                "                **未显式指定**的轴\n");
            std::exit(EXIT_SUCCESS);
        } else {
            throw std::invalid_argument("未知参数: " + a);
        }
    }
    if (o.quick && !o.has_rows) o.rows = {512, 1024};
    if (o.repeat == 0) throw std::invalid_argument("--repeat 必须 >= 1");
    if (o.attr_bits == 0 || o.attr_bits > 20) {
        throw std::invalid_argument("--attr-bits 必须在 1..20");
    }
    if (o.mode == "grpc" && (o.server0.empty() || o.server1.empty())) {
        throw std::invalid_argument("--mode grpc 必须同时给 --server0 与 --server1");
    }
    if (o.range_width > 0) {
        const uint64_t domain = 1ull << o.attr_bits;
        // ⚠️ `lo == 0` 是**合法**的（取值 0 在域内）——库侧的 `RetrieveRangeFilter`
        //    只要求 `lo <= hi` 且 `hi < domain`。这里曾多写一条 `lo >= 1`，
        //    直接把 `l=5（域 32）+ k=32` 这种合法组合误判成非法（实测被它挡掉过一次）。
        // R3：`--range-count R` 是**同一区间重复 R 次**（对齐 Waldo bench 的约定：它的
        // 8 个谓词共用同一个 [left,right]）⇒ 只需校验**这一个**区间合法。
        if (o.range_lo + o.range_width - 1 >= domain) {
            throw std::invalid_argument(
                "区间 [lo, lo+k-1] 超出取值域：需要 0 <= lo 且 lo+k-1 <= 2^l - 1");
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
// 确定性记录生成（固定密钥 ⇒ 每次运行的数据集完全一致，数字可复现）
// ---------------------------------------------------------------------------
std::vector<std::vector<uint64_t>> MakeRecords(uint32_t n, uint32_t domain,
                                               uint64_t seed) {
    std::array<uint8_t, 16> key{};
    key.fill(0x2B);
    random::DeterministicPrng prng(key, seed);
    std::vector<std::vector<uint64_t>> recs(n, std::vector<uint64_t>(2));
    for (auto& r : recs) {
        r[0] = prng.Below(domain);
        r[1] = prng.Below(domain);
    }
    return recs;
}

// ---------------------------------------------------------------------------
// 记录结构
// ---------------------------------------------------------------------------
struct RunRec {
    uint32_t run_index = 0;
    bool is_warmup = false;
    double wall_ms = 0;          // 本次运行的三条查询合计墙钟
    double count1_ms = 0;        // 单谓词 Count
    double count2_ms = 0;        // 双谓词 Count（AND）
    double sum_ms = 0;           // SumWithFilter
    uint64_t answer1 = 0, answer2 = 0, answer_sum = 0;
    uint64_t pir_count1_measured = 0;  // **仅 local**（服务器侧计数器增量）
    uint64_t pir_sum_measured = 0;     // **仅 local**
    uint64_t rpc_count0 = 0;           // **仅 grpc**（通道实测）
    uint64_t rpc_count1 = 0;
    uint64_t sent_bytes = 0;           // **仅 grpc**（实测）
    uint64_t recv_bytes = 0;
    // 区间查询（仅 `--range-width k > 0` 时有值）
    double range_count_ms = 0;         // 区间 Count（多点查询 OR）
    double range_sum_ms = 0;           // 区间 SUM（区间列 + l_a 个取值位面）
    uint64_t range_count_answer = 0, range_sum_answer = 0;
    uint64_t range_pir_sets = 0;       // **仅 local**（服务器侧计数器增量）
    uint64_t range_rpc0 = 0, range_rpc1 = 0;   // **仅 grpc**
    uint64_t range_sent_bytes = 0, range_recv_bytes = 0;  // **仅 grpc**
    // R3：`--range-count R` 时 R 个区间的**总量**（上面 `range_count_ms`/`range_sum_ms`
    // 仍是**每区间均值**；R=1 时总量 == 单区间值 ⇒ 旧行为逐位不变）
    uint32_t range_count_reps = 1;
    double range_total_count_ms = 0;
    double range_total_sum_ms = 0;
    uint64_t range_total_recv_bytes = 0;
    uint64_t range_total_sent_bytes = 0;
    uint64_t range_total_rpc0 = 0;
    uint64_t range_total_rpc1 = 0;
    uint64_t range_total_pir_sets = 0;
    uint64_t range_total_baseline_count = 0;   // R 个区间的明文基准之和
};

struct PointRec {
    uint32_t window = 0;
    uint32_t attr_bits = 0;
    uint32_t lambda = 0;
    bool no_refresh = false;
    // 几何
    uint32_t part_num = 0;
    uint32_t part_size = 0;
    uint64_t hints = 0;
    uint64_t real_columns = 0;    // 真实列数（one-hot + bit 面）
    uint64_t padded_columns = 0;  // 补齐到 2 的幂后的条目数
    uint64_t pir_n = 0;           // = padded_columns（V-OO-PIR 要求 2 的幂）
    // 离线 / 存储 / 通信
    double init_ms = 0;
    // Init 阶段的**线上字节**（仅 grpc；含 InitTable + 逐 chunk 的 UploadEntries。
    // 这是上行大头 —— 共享份额上传 = 2 台 × padded_columns × N × 16 B 量级）
    uint64_t init_sent_bytes = 0;
    uint64_t init_recv_bytes = 0;
    uint64_t server_kb_formula = 0;    // 两台合计（公式 2·padded·N·16）
    uint64_t server_kb_measured = 0;   // **仅 local**（节点实测）
    uint64_t hint_kb = 0;
    // 区间口径（仅 `--range-width k > 0`）
    uint64_t range_lo = 0;
    uint64_t range_width = 0;
    uint64_t range_baseline_count = 0;  // 明文基准（确定性数据集上算出来的期望值）
    uint64_t range_baseline_sum = 0;
    uint64_t range_total_pir_sets = 0;
    uint64_t range_total_baseline_count = 0;   // 8 个区间的明文基准之和
    std::vector<RunRec> runs;
};

// 客户端构造：local = 进程内；grpc = 真实两进程（回调里才能看到通道计数器）
template <typename F>
void WithClient(const Options& o, const VmpqParams& p, const AesPrf& prf, F&& f) {
    if (o.mode == "local") {
        VmpqClient c(p, prf);
        f(c, nullptr, nullptr);
        return;
    }
    GrpcChannel ch0(o.server0), ch1(o.server1);
    if (!ch0.WaitForConnection(5000) || !ch1.WaitForConnection(5000)) {
        throw std::runtime_error("无法连接到 " + o.server0 + " / " + o.server1 +
                                 "（先起两台 vmpq_server）");
    }
    // 决策 D38：服务器要先知道 N（= 条目宽度）才能解码 PirQuery
    ch0.InitTable(p.window_size, p.attr_sizes);
    ch1.InitTable(p.window_size, p.attr_sizes);
    VmpqClient c(p, prf, ch0, ch1);
    f(c, &ch0, &ch1);
}

PointRec RunPoint(const Options& o, uint32_t N, bool no_refresh) {
    PointRec rec;
    rec.window = N;
    rec.attr_bits = o.attr_bits;
    rec.lambda = o.lambda;
    rec.no_refresh = no_refresh;

    VmpqParams p;
    p.window_size = N;
    const uint32_t domain = 1u << o.attr_bits;
    p.attr_sizes = {domain, domain};
    p.lambda = o.lambda;
    p.unsafe_disable_hint_refresh = no_refresh;

    const auto recs = MakeRecords(N, domain, o.seed);
    // 区间查询的**明文基准**：确定性数据集上算出的期望值 ⇒ 任何一次实测都必须逐值相等
    // （仓库纪律：确定性随机源下的期望值断言，只验"跑通"不算）。
    if (o.range_width > 0) {
        rec.range_lo = o.range_lo;
        rec.range_width = o.range_width;
        const uint64_t lo = o.range_lo;
        const uint64_t hi = o.range_lo + o.range_width - 1;
        for (const auto& r0 : recs) {
            if (r0[0] >= lo && r0[0] <= hi) {
                rec.range_baseline_count++;
                rec.range_baseline_sum += r0[1];
            }
        }
    }
    std::vector<uint8_t> key(16);
    for (int i = 0; i < 16; ++i) key[i] = static_cast<uint8_t>(i + 1);
    AesPrf prf(key);

    const uint64_t pv = o.pred_value % domain;
    const uint64_t pv2 = (o.pred_value + 2) % domain;

    WithClient(o, p, prf, [&](VmpqClient& client, GrpcChannel* ch0, GrpcChannel* ch1) {
        // ---- 离线：Init（每点**只做一次**；`repeat` 只重复在线查询）----
        const auto t_init = Clock::now();
        const uint64_t init_s0 = ch0 ? ch0->sent_bytes() : 0;
        const uint64_t init_s1 = ch1 ? ch1->sent_bytes() : 0;
        const uint64_t init_v0 = ch0 ? ch0->recv_bytes() : 0;
        const uint64_t init_v1 = ch1 ? ch1->recv_bytes() : 0;
        client.Init(recs);
        rec.init_ms = MsSince(t_init);
        if (ch0 != nullptr) {
            rec.init_sent_bytes = (ch0->sent_bytes() - init_s0) + (ch1->sent_bytes() - init_s1);
            rec.init_recv_bytes = (ch0->recv_bytes() - init_v0) + (ch1->recv_bytes() - init_v1);
        }

        const VooPirParams pp = p.DerivePirParams();
        rec.part_num = pp.part_num;
        rec.part_size = pp.part_size;
        rec.hints = client.pir().ValidHintCount();
        rec.real_columns = p.total_columns();
        rec.padded_columns = p.padded_columns();
        rec.pir_n = pp.n;
        rec.server_kb_formula = 2ull * rec.padded_columns * N * 16ull / 1024ull;
        rec.hint_kb = rec.hints * (static_cast<uint64_t>(N) * 16ull + 21ull) / 1024ull;
        if (ch0 == nullptr) {
            // local：节点实测（两台合计）。grpc 下节点在别的进程里，拿不到 ⇒ 留 0。
            rec.server_kb_measured = client.node(0).storage_bytes() * 2ull / 1024ull;
        }

        // ---- 在线：`warmup` + `repeat` 次运行，**每次一条独立记录** ----
        const uint32_t total = o.repeat + o.warmup;
        for (uint32_t run = 0; run < total; ++run) {
            RunRec rr;
            rr.run_index = run;
            rr.is_warmup = (run < o.warmup);

            const uint64_t served_before =
                (ch0 == nullptr) ? client.node(0).queries_served() : 0;
            const uint64_t r0b = ch0 ? ch0->rpc_count() : 0;
            const uint64_t r1b = ch1 ? ch1->rpc_count() : 0;
            const uint64_t s0b = ch0 ? ch0->sent_bytes() : 0;
            const uint64_t s1b = ch1 ? ch1->sent_bytes() : 0;
            const uint64_t v0b = ch0 ? ch0->recv_bytes() : 0;
            const uint64_t v1b = ch1 ? ch1->recv_bytes() : 0;

            const auto t_run = Clock::now();

            // 单谓词 Count（1 列 ⇒ 1 个查询集）
            auto t = Clock::now();
            rr.answer1 = client.CountSinglePredicate(0, pv);
            rr.count1_ms = MsSince(t);
            if (ch0 == nullptr) {
                rr.pir_count1_measured = client.node(0).queries_served() - served_before;
            }

            // 双谓词 Count（2 列 ⇒ 2 个查询集，同一次 RPC 发出）
            t = Clock::now();
            rr.answer2 = client.CountMultiPredicate({{0, pv}, {1, pv2}});
            rr.count2_ms = MsSince(t);

            // Sum：filter 一个谓词 ⇒ 取回 1 个 one-hot 列 + l 个 bit 面列
            const uint64_t served_sum_before =
                (ch0 == nullptr) ? client.node(0).queries_served() : 0;
            t = Clock::now();
            rr.answer_sum = client.SumWithFilter({{0, pv}}, 1);
            rr.sum_ms = MsSince(t);
            if (ch0 == nullptr) {
                rr.pir_sum_measured = client.node(0).queries_served() - served_sum_before;
            }

            // ---- 区间（**非原生**：k 个点查询 + 客户端本地 OR）----
            // R3：`--range-count R` ⇒ **一次 Init 内**连跑 R 个**连续**区间
            //   （[lo,lo+k-1]、[lo+k,lo+2k-1]、…），逐区间验明文基准，并累计总量。
            //   R=1 时与旧行为逐位相同（总量 == 单区间值）。
            if (o.range_width > 0) {
                const uint64_t budget = o.range_budget_mb << 20;
                const uint64_t rc0b = ch0 ? ch0->rpc_count() : 0;
                const uint64_t rc1b = ch1 ? ch1->rpc_count() : 0;
                const uint64_t rs0b = ch0 ? ch0->sent_bytes() : 0;
                const uint64_t rs1b = ch1 ? ch1->sent_bytes() : 0;
                const uint64_t rv0b = ch0 ? ch0->recv_bytes() : 0;
                const uint64_t rv1b = ch1 ? ch1->recv_bytes() : 0;
                const uint64_t served_range_before =
                    (ch0 == nullptr) ? client.node(0).queries_served() : 0;

                rr.range_count_reps = o.range_count;
                double c_ms = 0.0, s_ms = 0.0;
                uint64_t base_c = 0, base_s = 0;
                for (uint32_t ri = 0; ri < o.range_count; ++ri) {
                    // 同一区间重复 R 次（R3：对齐 Waldo 的 8 个谓词共用同一区间）
                    const uint64_t rlo = o.range_lo;
                    const uint64_t rhi = o.range_lo + o.range_width - 1;
                    // 明文基准：同一区间，只算一次（逐次断言仍逐次做）
                    const uint64_t bc = rec.range_baseline_count;
                    const uint64_t bs = rec.range_baseline_sum;
                        // (recs 已在上方算过同一区间的基准)
                    
                    auto tr = Clock::now();
                    const uint64_t ca = client.CountRange(0, rlo, rhi, budget);
                    c_ms += MsSince(tr);
                    tr = Clock::now();
                    const uint64_t sa = client.SumRange(0, rlo, rhi, 1, budget);
                    s_ms += MsSince(tr);
                    if (ri == 0) { rr.range_count_answer = ca; rr.range_sum_answer = sa; }
                    // 逐值断言（只在**测量**运行上炸，避免预热把失败放大成噪声）：
                    // 区间走的是"多点查询 + 本地 OR"，必须与明文基准逐值相等。
                    if (!rr.is_warmup && (ca != bc || sa != bs)) {
                        throw std::runtime_error(
                            "区间多点查询与明文基准不一致：第 " + std::to_string(ri) +
                            " 个区间 count " + std::to_string(ca) + " vs 基准 " +
                            std::to_string(bc) + "、sum " + std::to_string(sa) +
                            " vs 基准 " + std::to_string(bs));
                    }
                    base_c += bc; base_s += bs;
                }
                // 每区间**均值**（保持字段语义：R=1 时 == 单区间值）；总量另存 `range_total_*`
                rr.range_count_ms = c_ms / o.range_count;
                rr.range_sum_ms = s_ms / o.range_count;
                rr.range_total_count_ms = c_ms;
                rr.range_total_sum_ms = s_ms;
                rr.range_total_baseline_count = base_c;
                if (ch0 == nullptr) {
                    rr.range_pir_sets = client.node(0).queries_served() - served_range_before;
                    rr.range_total_pir_sets = rr.range_pir_sets;
                } else {
                    rr.range_rpc0 = ch0->rpc_count() - rc0b;
                    rr.range_rpc1 = ch1->rpc_count() - rc1b;
                    rr.range_sent_bytes = (ch0->sent_bytes() - rs0b) + (ch1->sent_bytes() - rs1b);
                    rr.range_recv_bytes = (ch0->recv_bytes() - rv0b) + (ch1->recv_bytes() - rv1b);
                    rr.range_total_rpc0 = rr.range_rpc0;
                    rr.range_total_rpc1 = rr.range_rpc1;
                    rr.range_total_sent_bytes = rr.range_sent_bytes;
                    rr.range_total_recv_bytes = rr.range_recv_bytes;
                }
            }
            rr.wall_ms = MsSince(t_run);
            if (ch0 != nullptr) {
                rr.rpc_count0 = ch0->rpc_count() - r0b;
                rr.rpc_count1 = ch1->rpc_count() - r1b;
                rr.sent_bytes = (ch0->sent_bytes() - s0b) + (ch1->sent_bytes() - s1b);
                rr.recv_bytes = (ch0->recv_bytes() - v0b) + (ch1->recv_bytes() - v1b);
            }
            rec.runs.push_back(rr);
        }
    });
    return rec;
}

// 人读表格里用**最后一次测量运行**（与 `bench_mpraq` 同口径）
const RunRec& LastMeasured(const PointRec& p) {
    if (p.runs.empty()) throw std::logic_error("no runs");
    if (!p.runs.back().is_warmup) return p.runs.back();
    return p.runs.front();
}

std::string RowJson(const PointRec& p, const RunRec& r) {
    Json j;
    j.U64("run_index", r.run_index)
        .Bool("is_warmup", r.is_warmup)
        .Dbl("wall_ms", r.wall_ms, 3);
    Json g;
    g.U64("n", p.window)
        .U64("attr_bits", p.attr_bits)
        .U64("lambda", p.lambda)
        .U64("real_columns", p.real_columns)
        .U64("padded_columns", p.padded_columns)
        .U64("pir_n", p.pir_n)
        .U64("part_num", p.part_num)
        .U64("part_size", p.part_size)
        .U64("hint_count", p.hints)
        .Bool("no_refresh", p.no_refresh);
    Json in;
    in.Dbl("init_ms", p.init_ms, 3)
        .U64("sent_bytes_measured", p.init_sent_bytes)
        .U64("recv_bytes_measured", p.init_recv_bytes);
    Json c1;
    c1.Dbl("ms", r.count1_ms, 4)
        .U64("answer", r.answer1)
        .U64("pir_sets_measured", r.pir_count1_measured)
        .U64("rpc_count", r.rpc_count0)
        .U64("rpc_count_server1", r.rpc_count1);
    Json c2;
    c2.Dbl("ms", r.count2_ms, 4).U64("answer", r.answer2);
    Json sm;
    sm.Dbl("ms", r.sum_ms, 4)
        .U64("answer", r.answer_sum)
        .U64("pir_sets_measured", r.pir_sum_measured);
    Json st;
    st.U64("server_kb_formula", p.server_kb_formula)
        .U64("server_kb_measured", p.server_kb_measured)
        .U64("hint_kb", p.hint_kb);
    Json cm;
        cm.U64("sent_bytes_measured", r.sent_bytes)
        .U64("recv_bytes_measured", r.recv_bytes);
    Json rg;
    rg.U64("lo", p.range_lo)
        .U64("width", p.range_width)
        .Bool("native", false)   // ⚠️ 明确：VMPQ 只原生支持点查询，区间是多点查询近似
        .Dbl("count_ms", r.range_count_ms, 4)
        .U64("count", r.range_count_answer)
        .U64("baseline_count", p.range_baseline_count)
        .Dbl("sum_ms", r.range_sum_ms, 4)
        .U64("sum", r.range_sum_answer)
        .U64("baseline_sum", p.range_baseline_sum)
        .U64("pir_sets_measured", r.range_pir_sets)
        .U64("rpc_count", r.range_rpc0)
        .U64("rpc_count_server1", r.range_rpc1)
        .U64("sent_bytes_measured", r.range_sent_bytes)
        .U64("recv_bytes_measured", r.range_recv_bytes)
          // R3：`--range-count R` 的**总量**（上面 count_ms/sum_ms/*_bytes 仍是每区间均值）
          .U64("count_reps", r.range_count_reps)
          .Dbl("total_count_ms", r.range_total_count_ms, 4)
          .Dbl("total_sum_ms", r.range_total_sum_ms, 4)
          .U64("total_recv_bytes", r.range_total_recv_bytes)
          .U64("total_sent_bytes", r.range_total_sent_bytes)
          .U64("total_rpc_count", r.range_total_rpc0)
          .U64("total_rpc_count_server1", r.range_total_rpc1)
          .U64("total_pir_sets_measured", r.range_total_pir_sets)
          .U64("total_baseline_count", r.range_total_baseline_count);
    j.Raw("geometry", g.Done())
        .Raw("init", in.Done())
        .Raw("count1", c1.Done())
        .Raw("count2", c2.Done())
        .Raw("sum", sm.Done())
        .Raw("storage", st.Done())
        .Raw("comm", cm.Done())
        .Raw("range", rg.Done());
    return j.Done();
}

std::string ConfigJson(const Options& o) {
    std::string rows_str;
    for (size_t i = 0; i < o.rows.size(); ++i) {
        if (i) rows_str += ",";
        rows_str += std::to_string(o.rows[i]);
    }
    Json j;
    j.Str("experiment", "vmpq-bench")
        .Str("mode", o.mode)
        .Raw("rows", "[" + rows_str + "]")
        .U64("attr_bits", o.attr_bits)
        .U64("lambda", o.lambda)
        .U64("seed", o.seed)
        .U64("pred_value", o.pred_value)
        .U64("range_width", o.range_width)
        .U64("range_lo", o.range_lo)
        .U64("range_budget_mb", o.range_budget_mb)
        .Bool("range_native", false)   // VMPQ 原生只有点查询；区间 = 多点查询近似
        .U64("repeat", o.repeat)
        .U64("warmup", o.warmup)
        .Bool("no_refresh", o.no_refresh)
        .Str("server0", o.server0)
        .Str("server1", o.server1)
        .Str("build_type", "Release/-O3")
        .Str("query_set_unit",
             "one PIR entry retrieval (= one entire column, N cells of 16 B)")
        .Str("storage_unit", "server KB = two servers combined, padded_columns*N*16 B each")
        .Str("local_only_note",
             "pir_sets_measured/server_kb_measured exist only in mode=local; "
             "sent_bytes/recv_bytes/rpc_count only in mode=grpc");
    return j.Done();
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    try {
        o = ParseArgs(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[FAIL-LOUDLY] 参数错误: %s\n", e.what());
        return 1;
    }
    // `--json`：人读表格改道 stderr，stdout 只留 JSONL（与 bench_mpraq 同纪律）
    FILE* human = o.json ? stderr : stdout;

    std::fprintf(human, "VMPQ benchmark (VMP-07) mode=%s\n\n", o.mode.c_str());

    // ---- Init 上传量预检（**先算清楚再跑**；与 MPRAQ 的 L14 / D34 预检同语义）----
    //   VMPQ 的 Init 上传 = `2 · padded_columns · N · 16 B`，而 `padded_columns ∝ 2^l`
    //   ⇒ 与 N、l 都是线性/指数关系。这是**真会咬人**的一条：l=8（padded 1024）、
    //   N=2^18 时是 **8 GiB**（实测 N=4096/l=8 已 144 MiB / 7–10 s）⇒ 不在开跑前拦住，
    //   就会在"什么都没测到"之前先花几十分钟上传。
    //   超预算 ⇒ **退出码 3**（驱动记 `budget_rejected`），除非 `--allow-huge-init`。
    for (uint32_t N : o.rows) {
        VmpqParams pp;
        pp.window_size = N;
        const uint32_t dom = 1u << o.attr_bits;
        pp.attr_sizes = {dom, dom};
        pp.lambda = o.lambda;
        const uint64_t init_bytes = 2ull * pp.padded_columns() * N * 16ull;
        const uint64_t budget = o.init_budget_mb << 20;
        const bool over = (o.mode == "grpc") && init_bytes > budget;
        std::fprintf(human,
                     "scale n=%u l=%u padded_columns=%llu init_upload=%llu B "
                     "budget=%llu B mode=%s pass=%d\n",
                     N, o.attr_bits, static_cast<unsigned long long>(pp.padded_columns()),
                     static_cast<unsigned long long>(init_bytes),
                     static_cast<unsigned long long>(budget), o.mode.c_str(),
                     over ? 0 : 1);
        if (over && !o.allow_huge_init) {
            std::fprintf(stderr,
                         "[FAIL-LOUDLY] 拒绝运行：Init 上传 %llu B > 预算 %llu B"
                         "（N=%u, l=%u, mode=grpc）\n"
                         "  公式 = 2 · padded_columns(%llu) · N · 16 B，且 padded_columns ∝ 2^l\n"
                         "  ⇒ 大 N 请配小 l，但 l 不能小于 5（双属性下 padded=64 时 P=8 < 16，\n"
                         "     `DerivePirParams` 会拒绝）。参考 N=2^18：l=5→1 GiB、l=6→2 GiB、l=8→8 GiB。\n"
                         "     或调大 --init-budget-mb，或 --allow-huge-init 显式放行\n",
                         static_cast<unsigned long long>(init_bytes),
                         static_cast<unsigned long long>(budget), N, o.attr_bits,
                         static_cast<unsigned long long>(pp.padded_columns()));
            return 3;   // 合法拒绝（不是失败）：驱动记 budget_rejected
        }
    }

    std::fprintf(human,
                 "%8s %3s %8s %7s %5s %6s %7s %9s %8s %8s %9s %9s %8s %8s %8s\n",
                 "N", "l", "columns", "PIR n", "P", "sigma", "hints", "Init ms",
                 "Cnt1 ms", "Cnt2 ms", "Sum ms", "PIR/Cnt1", "PIR/Sum", "Srv KB",
                 "Hint KB");

    std::vector<PointRec> points;
    std::vector<std::string> json_lines;
    for (uint32_t N : o.rows) {
        try {
            const PointRec p = RunPoint(o, N, o.no_refresh);
            points.push_back(p);
            const RunRec& r = LastMeasured(p);
            // local 用实测的 PIR 次数；grpc 没有本地计数器 ⇒ 打派生值并在脚注说明
            const bool local = (o.mode == "local");
            const uint64_t pir_c1 = local ? r.pir_count1_measured : 1;
            const uint64_t pir_sum =
                local ? r.pir_sum_measured : (1ull + p.attr_bits);
            const uint64_t srv_kb = local ? p.server_kb_measured : p.server_kb_formula;
            std::fprintf(human,
                         "%8u %3u %8llu %7llu %5u %6u %7llu %9.1f %8.2f %8.2f "
                         "%9.2f %9llu %8llu %8llu %8llu \n",
                         p.window, p.attr_bits,
                         static_cast<unsigned long long>(p.real_columns),
                         static_cast<unsigned long long>(p.pir_n), p.part_num,
                         p.part_size, static_cast<unsigned long long>(p.hints),
                         p.init_ms, r.count1_ms, r.count2_ms, r.sum_ms,
                         static_cast<unsigned long long>(pir_c1),
                         static_cast<unsigned long long>(pir_sum),
                         static_cast<unsigned long long>(srv_kb),
                         static_cast<unsigned long long>(p.hint_kb));
            if (o.range_width > 0) {
                // 区间口径单独一行（bare key=value，与本文件其余明细行同风格）
                std::fprintf(human,
                             "         range lo=%llu k=%llu native=0 count=%llu/%llu "
                             "sum=%llu/%llu count_ms=%.3f sum_ms=%.3f sets=%llu "
                             "rpc0=%llu rpc1=%llu sent_B=%llu recv_B=%llu\n",
                             static_cast<unsigned long long>(p.range_lo),
                             static_cast<unsigned long long>(p.range_width),
                             static_cast<unsigned long long>(r.range_count_answer),
                             static_cast<unsigned long long>(p.range_baseline_count),
                             static_cast<unsigned long long>(r.range_sum_answer),
                             static_cast<unsigned long long>(p.range_baseline_sum),
                             r.range_count_ms, r.range_sum_ms,
                             static_cast<unsigned long long>(r.range_pir_sets),
                             static_cast<unsigned long long>(r.range_rpc0),
                             static_cast<unsigned long long>(r.range_rpc1),
                             static_cast<unsigned long long>(r.range_sent_bytes),
                             static_cast<unsigned long long>(r.range_recv_bytes));
            }
            for (const RunRec& rr : p.runs) {
                json_lines.push_back(std::string("{\"config\":") + ConfigJson(o) +
                                     ",\"point\":" + RowJson(p, rr) + "}");
            }
        } catch (const std::exception& e) {
            std::fprintf(human, "%8u %3u  -- 跳过：%s\n", N, o.attr_bits, e.what());
        }
    }
    if (o.mode == "grpc") {
        std::fprintf(human,
                     "  ⚠️ mode=grpc：PIR/Cnt1 与 PIR/Sum 是**派生值**（远程模式没有本地\n"
                     "     服务器计数器）；上下行字节请从 JSON 的 comm.sent/recv_bytes_measured 取。\n");
    }

    // SUM 的 PIR 次数：本实现（bit 面） vs 旧归约（Σ_v v·Count = 2^l × 2 列）
    std::fprintf(human, "\nSUM_pir\n");
    std::fprintf(human, "%8s %12s %22s %10s\n", "N", "impl", "legacy", "speedup");
    for (const PointRec& p : points) {
        const RunRec& r = LastMeasured(p);
        const uint64_t legacy = static_cast<uint64_t>(1u << p.attr_bits) * 2ull;
        const uint64_t impl = (o.mode == "local") ? r.pir_sum_measured
                                                  : (1ull + p.attr_bits);
        std::fprintf(human, "%8u %12llu %22llu %9.1fx\n", p.window,
                     static_cast<unsigned long long>(impl),
                     static_cast<unsigned long long>(legacy),
                     static_cast<double>(legacy) /
                         static_cast<double>(impl == 0 ? 1 : impl));
    }

    // D17 开关对照（可选）：不刷新会泄露查询分区 ⇒ 只做开销对比
    if (o.refresh_cost) {
        std::fprintf(human, "\nrefresh_cost\n");
        std::fprintf(human, "%8s %14s %16s %12s %14s\n", "N", "cnt1_on_ms",
                     "cnt1_off_ms", "sum_on_ms", "sum_off_ms");
        for (uint32_t N : o.rows) {
            try {
                const PointRec safe = RunPoint(o, N, false);
                const PointRec fast = RunPoint(o, N, true);
                std::fprintf(human, "%8u %14.2f %16.2f %12.2f %14.2f\n", N,
                             LastMeasured(safe).count1_ms, LastMeasured(fast).count1_ms,
                             LastMeasured(safe).sum_ms, LastMeasured(fast).sum_ms);
            } catch (const std::exception& e) {
                std::fprintf(human, "%8u  -- 跳过：%s\n", N, e.what());
            }
        }
    }

    // ---- 机器可读输出 ----
    if (o.json) {
        for (const std::string& line : json_lines) std::printf("%s\n", line.c_str());
    }
    if (!o.json_out.empty()) {
        std::ofstream f(o.json_out);
        if (!f) {
            std::fprintf(stderr, "无法写 %s\n", o.json_out.c_str());
            return 1;
        }
        f << "{\"config\":" << ConfigJson(o) << ",\"points\":[";
        for (size_t i = 0; i < points.size(); ++i) {
            for (size_t k = 0; k < points[i].runs.size(); ++k) {
                if (i || k) f << ",";
                f << RowJson(points[i], points[i].runs[k]);
            }
        }
        size_t total = 0;
        for (const PointRec& p : points) total += p.runs.size();
        f << "],\"summary\":{\"points\":" << total
          << ",\"note\":\"each point = one run (warmup runs flagged is_warmup)\"}}\n";
    }

    // -----------------------------------------------------------------------
    // 列口径（**解释写在这里，不打印**；print 精简纪律）
    //   * columns = 真实列数（one-hot 列 Σ2^l_a + bit 面列 Σl_a）；
    //     PIR n   = 向上补齐到 2 的幂后的列数（D15(1)：V-OO-PIR 要求 n 是 2 的幂）。
    //     一个 DB 条目 = 一整列 = N 个 128 位 cell（D38）⇒ 一次列查询 = 1 个查询集。
    //   * Srv KB = local 是**节点实测**（两台合计 = 2·padded·N·16 B）；grpc 是**公式**
    //     （节点在别的进程里，客户端拿不到）。
    //   * hints = 有效 hint 数（`ValidHintCount()`）。旧版的表头写的是 `M`（公式值），
    //     但打的是这个实测值 —— 名实不符，这里按实值改名为 `hints`。
    //   * PIR/Cnt1 = 1（每列一次，D38）；PIR/Sum = |filter| + l（取回 bit 面后本地还原，D18）。
    //   * （旧列的 `Qry B` = 按 2 字节偏移的**公式**估算，比线上实际
    //     `repeated uint32` 低估 572×）⇒ **已删除**，只保留实测值：
    //     JSON 的 `comm.sent_bytes_measured` / `recv_bytes_measured`（A13）。
    // -----------------------------------------------------------------------
    return 0;
}
