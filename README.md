# TSB：VMPQ 与 MPRAQ 双服务器隐私聚合查询

本仓库实现两套**双服务器（two-server）隐私聚合查询**系统，共用同一套底座
（AES-PRF、秘密共享、SecureMul 与验证、gRPC 传输），因此可以用同一批查询、同一套账目口径相互对照。

| 系统 | 安全模型 | 说明 |
|---|---|---|
| **VMPQ** | 半诚实（honest-but-curious） | 复现已发表方案的基线实现（V-OO-PIR + 逐列 PIR） |
| **MPRAQ** | **恶意（malicious，默认）** / 半诚实 | 自研方案：**Plinko PIR** + LCTE 编码 + **xmac** 值层完整性 + SPDZ MAC 聚合 |

---

## 1. 仓库结构

```
src/core/     环运算、AES-PRF、哈希、随机、字段与配置
src/shared/   秘密共享、表抽象、SecureMul、MAC 验证
src/pir/      V-OO-PIR（Plinko 查询/应答、hint、共享）
src/mpraq/    MPRAQ：LCTE 编码、谓词解析、聚合（COUNT / SUM / AVG）、Init 与账目
src/vmpq/     VMPQ：逐列 PIR、区间（多点查询 + 本地 OR）
src/net/      传输层：进程内通道 + 真实 gRPC（服务端/客户端、Relay、远程 SecureMul）
src/apps/     可执行入口：mpraq_client / mpraq_server / vmpq_client / vmpq_server
proto/        gRPC 协议（mpraq.proto、vmpq.proto；gen/ 为构建产物）
bench/        基准程序：bench_mpraq、bench_vmpq
tests/        CTest 单元/集成测试（6 个套件 / 54 个用例）
config/       示例规模配置
cmake/        自定义 CMake 查找模块（FindCryptopp）
third_parts/  仅入库 json/json.hpp（其余 vendor 依赖需自备，见 §2）
```

> 本仓库**只包含代码**：论文/实验的方案、点清单、重跑脚本与实测数据不在其中。

---

## 2. 依赖与构建

### 2.1 依赖

| 依赖 | 验证过的版本 |
|---|---|
| CMake | ≥ **3.20**（验证于 4.2.3） |
| 编译器 | GCC **15.2**（C++17） |
| protobuf / protoc | **31.1** |
| gRPC | **1.78.1** |
| Crypto++ | **8.9.0** |
| Boost | **1.76**（gRPC 运行期需要） |

默认布局（`~/.local` 前缀）：

```
<prefix>/bin/{protoc,grpc_cpp_plugin}
<prefix>/cryptopp/{include,lib}
<prefix>/{include,lib}/…        # protobuf、gRPC、Boost
```

### 2.2 依赖装在别处（可移植）

依赖前缀由 CMake 缓存变量 **`TSB_DEPS_PREFIX`** 控制（默认 `$HOME/.local`），三种写法等价：

```bash
cmake -S . -B build                                   # 默认 $HOME/.local
cmake -S . -B build -DTSB_DEPS_PREFIX=/opt/deps       # 依赖装在别处
cmake -S . -B build -DCMAKE_PREFIX_PATH=/a;/b         # CMake 惯例写法
```

还可单项覆盖：`-DCRYPTOPP_ROOT=…`、`-D_PROTOBUF_PROTOC=…`、`-D_GRPC_CPP_PLUGIN=…`。
`protoc` / `grpc_cpp_plugin` 不在前缀下时会**自动回退到 PATH**，两者都找不到则给出**明确报错**。

### 2.3 构建与测试

```bash
cmake -S . -B build        # 默认 Release（-O3）
cmake --build build -j
ctest --test-dir build     # 6 个套件 / 54 个用例
```

产物在 `build/bin/`：`mpraq_client`、`mpraq_server`、`vmpq_client`、`vmpq_server`、
`bench_mpraq`、`bench_vmpq` 与 6 个 `test_*`。

> `build/`、`build_*/`、`proto/gen/` 均为生成物，不入库。

---

## 3. 运行 MPRAQ（两进程）

```bash
./build/bin/mpraq_server --listen 127.0.0.1:0 --id 0 --port-file /tmp/mp0.port --security-mode malicious &
./build/bin/mpraq_server --listen 127.0.0.1:0 --id 1 --port-file /tmp/mp1.port --security-mode malicious &
sleep 1
./build/bin/mpraq_client \
    --server0 127.0.0.1:$(cat /tmp/mp0.port) \
    --server1 127.0.0.1:$(cat /tmp/mp1.port) \
    --rows 16384 --columns 256 --attributes 2 --predicates 2 --security-mode malicious
pkill -x mpraq_server
```

规模只需管**三个量**：`--rows N`（记录数，2 的幂）、`--columns C`（每属性 LCTE 列数 = 取值域层数）、
`--attributes A`；其余（`M = A·C`、补齐后的 `m`、`κ = m/w`、hint 预算 `N_T = λw/2`）自动派生并打印。
`--security-mode` **必须与两台服务器一致**，不一致会在 Init 阶段被拒绝。

**完整性主线**

* **计数路径（xmac）**：服务器持数据与 tag 的共享，客户端逐 chunk 校验 `M == γ ⊙ R`；
* **聚合路径（SPDZ MAC）**：重建 z 与其 MAC 后校验，另有一道"`z` 与 `f·(e+b)` 确定性复算"的本地检查；
* **⟨αE⟩ 持久化（恶意档）**：Init 时客户端把 `α·E` 的**加法秘密共享**下发并由服务器保存
  （`share₀ ⊕ share₁ = α⊙E`，Init 内自检）。因此服务器侧 LCTE 表 = **E + MAC + αE** 三张等大的表；
  **属性值单独成表**（按记录索引对齐），与 LCTE 表分开计账。

---

## 4. 运行 VMPQ（半诚实基线）

```bash
./build/bin/vmpq_server --listen 127.0.0.1:0 --id 0 --port-file /tmp/vm0.port &
./build/bin/vmpq_server --listen 127.0.0.1:0 --id 1 --port-file /tmp/vm1.port &
sleep 1
./build/bin/vmpq_client --server0 127.0.0.1:$(cat /tmp/vm0.port) \
                        --server1 127.0.0.1:$(cat /tmp/vm1.port) \
                        --rows 16384 --attr-bits 8 --lambda 24
```

VMPQ 把**一整列**当作一个条目（逐 cell 的 `Z_{2^128}`，不做 GF(2) 打包），条目数与列数绑死；
区间查询不是原生能力 ⇒ 客户端用**多点查询 + 本地 OR** 拼出来。

---

## 5. 基准与测试

```bash
# MPRAQ：进程内或真实 gRPC 两进程（未给端点时自动拉起同目录的 mpraq_server）
./build/bin/bench_mpraq --exp online --mode grpc --rows 262144 \
    --columns-per-attribute 256 --attributes 8 --predicates 8 --security-mode malicious
# VMPQ
./build/bin/bench_vmpq --rows 262144 --attr-bits 8 --lambda 24 --range-width 128 --mode grpc
# 测试
ctest --test-dir build --output-on-failure
```

---

## 6. 已知边界（如实声明）

* **VMPQ 只有半诚实档**：其模型不含恶意行为下的完整性保证。
* **半诚实档下服务器篡改会被静默接受** —— 这是该威胁模型的范围，不是缺陷；需要验证请用恶意档（默认）。
* **VMPQ 的区间查询**是"多点查询 + 本地 OR"，不是原生能力。
* **只测本地回环**：本项目不含任何 WAN / 限带宽实验的结论。
* **服务器单会话串行**：未做并发/吞吐测量，也没有相应承诺。
* 第三方 vendor（Crypto++/protobuf/gRPC/Boost）需预先安装（见 §2）；`third_parts/` 只入库 `json/json.hpp`。

---

## 7. 约定（改代码前必读）

* 规模派生：`A` = 属性数 = 谓词数，`C` = 每属性列数（= 取值域层数，2 的幂），`M = A·C`，
  `m` = `M` 补齐到 `2w` 的倍数；`κ = m/w`；hint 预算 `N_T = λw/2`，备用 hint `H = 3λw/2`。
* MPC 层的 `q = 2^127−1`；GF(2^128) 固定多项式 `x^128 + x^7 + x^2 + x + 1`。
* 账目分**应用层**（帧字节）与**传输层**（protobuf 封套）两个口径，任何时候不得混用。
* 规模与档位的不满足一律**拒绝运行**，绝不静默取整或静默降级。

## 8. 许可

见 [LICENSE](LICENSE)。
