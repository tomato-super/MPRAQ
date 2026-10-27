// `test_vmpq_range` —— VMPQ **区间查询（多点查询近似，非原生）** 的契约测试。
//
// ===========================================================================
// 为什么需要这份测试
// ===========================================================================
// VMPQ 论文**只原生支持点查询**；区间 [lo, hi] 由"对区间内每个取值各做一次点查询 +
// 客户端本地按位或"拼出来（见 `vmpq.hpp` 的"区间查询"一节）。这条路径有三处
// **写错也不会崩、只会静默给错值**的地方：
//   ① 合并方向写错（用了 AND 而不是 OR ⇒ 结果偏小甚至为 0）；
//   ② **分块 OR** 的边界处理错（`max_batch_bytes` 触发的多批路径与单批路径不一致）；
//   ③ 区间端点/取值域校验漏了（越界取值会被当成 0 静默吞掉）。
// 此外本实现引入了"分块"来控制峰值内存（`RetrieveEntries` 会为**每个**目标列
// 分配 N 个 128 位 cell），而分块会让一次区间查询变成**多次 RPC** ⇒
// "查询集数仍恒为 k"这条代价口径也需要被断言钉住。
//
// ===========================================================================
// 覆盖的契约
// ===========================================================================
//   R1  `RetrieveRangeFilter` == 明文"取值 ∈ [lo, hi]"的位向量（逐位）
//   R2  `CountRange` == 明文区间计数；`SumRange` == 明文区间内 sum_attr 之和
//   R3  单点区间退化：`CountRange(a, v, v) == CountSinglePredicate(a, v)`
//   R4  **分块一致性**：`max_batch_bytes` 极小（强制多批）与极大（单批）结果逐位相同
//   R5  **代价口径**：`CountRange` 消耗 **k** 个查询集；`SumRange` 消耗 **k + l_a** 个
//       （用服务器侧 `queries_served()` 增量实测，不靠公式）
//   R6  非法区间必须**抛异常**（lo > hi / hi 越出取值域 / attr_id 越界），绝不静默
//   R7  确定性（铁律 D6）：同参数两个独立 client ⇒ 区间 filter 逐位相同
//   R8  区间结果与"逐点查询后本地或"这一展开式**等价**（语义独立复核）

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "test_framework.hpp"
#include "vmpq/vmpq.hpp"

namespace {

using namespace tsb;

constexpr uint32_t kN = 128;      // 记录数（= window_size = 每列 cell 数）
constexpr uint32_t kDomain = 64;  // 每属性取值域 2^6（l=6）
constexpr uint32_t kBits = 6;

// 确定性记录：自包含 LCG（不依赖 core/random 的实现细节，便于独立复算基准）
std::vector<std::vector<uint64_t>> MakeRecords(uint32_t n, uint64_t seed) {
    std::vector<std::vector<uint64_t>> recs(n, std::vector<uint64_t>(2, 0));
    uint64_t s = seed;
    for (auto& rec : recs) {
        for (auto& v : rec) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            v = (s >> 33) % kDomain;
        }
    }
    return recs;
}

std::vector<uint8_t> Key16() {
    std::vector<uint8_t> key(16);
    for (int i = 0; i < 16; ++i) key[i] = static_cast<uint8_t>(i + 1);
    return key;
}

// 夹具：几何用 `attr_sizes={64,64}`（l=6）—— 真实列 64+64+6+6=140 ⇒ 补齐 256，
// 分区数 P=16 恰好满足 `DerivePirParams` 的 `P >= 16` 硬要求（否则建不起来）。
struct Fixture {
    VmpqParams params;
    std::vector<std::vector<uint64_t>> recs;
    std::vector<uint8_t> key;   // ⚠️ 必须比 client 活得久（构造顺序：key → prf → client）
    AesPrf prf;
    std::unique_ptr<VmpqClient> client;

    explicit Fixture(uint64_t seed = 20261010)
        : recs(MakeRecords(kN, seed)), key(Key16()), prf(key) {
        // ⚠️ 必须**先填好 params 再构造 client**（构造时要读它派生 PIR 参数）；
        //    成员声明顺序保证 `key` 比 `prf` 先构造、`prf` 比 `client` 活得久。
        params.window_size = kN;
        params.attr_sizes = {kDomain, kDomain};
        params.lambda = 24;
        client = std::make_unique<VmpqClient>(params, prf);
        client->Init(recs);
    }

    // ---- 明文基准（与实现完全独立的算法：直接遍历记录）----
    std::vector<uint8_t> PlaBits(uint32_t attr, uint64_t lo, uint64_t hi) const {
        std::vector<uint8_t> bits(kN, 0);
        for (uint32_t r = 0; r < kN; ++r) {
            const uint64_t v = recs[r][attr];
            bits[r] = (v >= lo && v <= hi) ? 1 : 0;
        }
        return bits;
    }
    uint64_t PlaCount(uint32_t attr, uint64_t lo, uint64_t hi) const {
        uint64_t c = 0;
        for (uint32_t r = 0; r < kN; ++r) {
            const uint64_t v = recs[r][attr];
            if (v >= lo && v <= hi) ++c;
        }
        return c;
    }
    uint64_t PlaSum(uint32_t attr, uint64_t lo, uint64_t hi, uint32_t sum_attr) const {
        uint64_t s = 0;
        for (uint32_t r = 0; r < kN; ++r) {
            const uint64_t v = recs[r][attr];
            if (v >= lo && v <= hi) s += recs[r][sum_attr];
        }
        return s;
    }
};

// R1：区间 filter 必须与明文逐位相同（合并方向若写成 AND，这里立刻炸）
TEST(VmpqRange, FilterMatchesPlaintextOr) {
    Fixture f;
    const std::vector<std::pair<uint64_t, uint64_t>> ranges = {
        {1, 7}, {0, 0}, {63, 63}, {10, 40}, {1, kDomain - 1}};
    for (const auto& r : ranges) {
        const std::vector<uint8_t> got = f.client->RetrieveRangeFilter(0, r.first, r.second);
        const std::vector<uint8_t> want = f.PlaBits(0, r.first, r.second);
        EXPECT_EQ(got.size(), want.size());
        EXPECT_EQ(got, want);
    }
}

// R2：计数与求和都要与明文一致（确定性随机源下的期望值测试，铁律 D6）
TEST(VmpqRange, CountAndSumMatchPlaintext) {
    Fixture f;
    const std::vector<std::pair<uint64_t, uint64_t>> ranges = {
        {1, 7}, {0, 0}, {30, 33}, {1, kDomain - 1}};
    for (const auto& r : ranges) {
        EXPECT_EQ(f.client->CountRange(0, r.first, r.second),
                  f.PlaCount(0, r.first, r.second));
        EXPECT_EQ(f.client->SumRange(0, r.first, r.second, 1),
                  f.PlaSum(0, r.first, r.second, 1));
    }
}

// R3：单点区间必须与既有点查询**同值**（两条路径的一致性）
TEST(VmpqRange, SinglePointEqualsPointQuery) {
    Fixture f;
    for (uint64_t v : {0ull, 1ull, 17ull, 63ull}) {
        EXPECT_EQ(f.client->CountRange(0, v, v), f.client->CountSinglePredicate(0, v));
    }
}

// R4：**分块一致性** —— 这条直接盯住 `RetrieveRangeFilter` 的分块 OR 边界。
//     极小预算强制"一批只取 1 列"，与"一批取完"必须逐位相同。
TEST(VmpqRange, ChunkedEqualsUnchunked) {
    Fixture f;
    const uint64_t lo = 3, hi = 40;
    const std::vector<uint8_t> single =
        f.client->RetrieveRangeFilter(0, lo, hi, /*max_batch_bytes=*/1ull << 30);
    // 每列 = N 个 128 位 cell = kN*16 B ⇒ 预算 0 字节时每批至少 1 列（下限保护），
    // 预算 = 一列的字节数时每批恰好 1 列 ⇒ 8 列要 8 批。
    const std::vector<uint8_t> chunked =
        f.client->RetrieveRangeFilter(0, lo, hi, /*max_batch_bytes=*/kN * 16ull);
    EXPECT_EQ(single, chunked);
    // 预算给到"半列"也必须得到同样结果（不能因为除零/取整退化成空批）
    const std::vector<uint8_t> tiny =
        f.client->RetrieveRangeFilter(0, lo, hi, /*max_batch_bytes=*/1);
    EXPECT_EQ(single, tiny);
}

// R5：代价口径 = k（Count）/ k + l_a（Sum），用**服务器侧计数器增量**实测。
//     ⚠️ 分块会改变 RPC 次数，但**不应改变查询集数** —— 这条把它钉住。
TEST(VmpqRange, CostIsExactlyKSets) {
    Fixture f;
    for (uint64_t k : {1ull, 8ull, 20ull}) {
        const uint64_t lo = 2, hi = lo + k - 1;
        const uint64_t before = f.client->node(0).queries_served();
        (void)f.client->CountRange(0, lo, hi);
        EXPECT_EQ(f.client->node(0).queries_served() - before, k);

        const uint64_t before_sum = f.client->node(0).queries_served();
        (void)f.client->SumRange(0, lo, hi, 1);
        // SUM 还要取回 sum_attr 的 l_a 个取值位面列
        EXPECT_EQ(f.client->node(0).queries_served() - before_sum, k + kBits);
    }
}

// R6：非法区间必须抛异常（绝不静默按 0 处理）
TEST(VmpqRange, BadRangesAreRejected) {
    Fixture f;
    EXPECT_THROW(f.client->CountRange(0, 9, 3), std::invalid_argument);          // lo > hi
    EXPECT_THROW(f.client->CountRange(0, 1, kDomain), std::out_of_range);        // hi 越界
    EXPECT_THROW(f.client->CountRange(2, 0, 1), std::out_of_range);              // attr_id 越界
    EXPECT_THROW(f.client->CountRange(1, 9, 3), std::invalid_argument);          // 另一属性同规则
    EXPECT_THROW(f.client->SumRange(0, 9, 3, 1), std::invalid_argument);
    EXPECT_THROW(f.client->SumRange(0, 1, kDomain, 1), std::out_of_range);
}

// R7：确定性 —— 同参数的两个独立 client 必须给出逐位相同的区间 filter
TEST(VmpqRange, DeterministicAcrossClients) {
    Fixture a(777), b(777);
    const std::vector<uint8_t> fa = a.client->RetrieveRangeFilter(0, 5, 25);
    const std::vector<uint8_t> fb = b.client->RetrieveRangeFilter(0, 5, 25);
    EXPECT_EQ(fa, fb);
    EXPECT_EQ(a.client->CountRange(0, 5, 25), b.client->CountRange(0, 5, 25));
}

// R8：区间 == "逐点查询再本地求或"的展开式（语义独立复核，不依赖内部实现）
TEST(VmpqRange, EquivalentToUnrolledPointQueries) {
    Fixture f;
    const uint64_t lo = 4, hi = 12;
    std::vector<uint8_t> unrolled(kN, 0);
    for (uint64_t v = lo; v <= hi; ++v) {
        const std::vector<uint8_t> col = f.client->RetrieveColumn(0, v);
        for (size_t r = 0; r < unrolled.size(); ++r) {
            unrolled[r] = static_cast<uint8_t>(unrolled[r] | col[r]);
        }
    }
    EXPECT_EQ(f.client->RetrieveRangeFilter(0, lo, hi), unrolled);
}

}  // namespace
