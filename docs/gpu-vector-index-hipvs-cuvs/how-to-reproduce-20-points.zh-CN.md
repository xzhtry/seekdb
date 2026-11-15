# 复现 20 分：AMD hipVS / cuVS 加速 seekdb HNSW 建图

> 这份文档是给**没有参与过这 17 轮实验**的人写的：目标是让你在冻结基线上重现同一个官方成绩
> **20.000 / 20 分（`build=3.220x, query=1.699x, gates=pass`）**。
>
> 详细的推演过程、每一轮的失败记录和全部原始证据在同目录的
> [`path-to-20-points.zh-CN.md`](./path-to-20-points.zh-CN.md) /
> [`build-performance.zh-CN.md`](./build-performance.zh-CN.md)。
> 本文只保留复刻必需的部分。

---

## 0. 一句话

建图主耗时不是 GPU 计算，而是 cuVS CAGRA **host 侧的 `optimize()`**，代价 ∝ `n × intermediate_degree`；
把 `intermediate_degree` 从 256 砍到 64（`graph_degree` 被 cuVS 强制跟随），再用「64 度图检索快一倍」
省下来的查询预算把候选表 `itopk` 从 512 拉到 2048 买回召回，构建加速比就从 1.105x 冲到 3.220x。

---

## 1. 起点与终点

| 项 | 值 |
|---|---|
| 仓库 | `https://github.com/xzhtry/seekdb` |
| 分支 | `amd-cagra-submit` |
| **冻结基线（起点）** | commit `634888d9b037153fa143d2c913ba4b25503e1d62`（tag `amd-cagra-contest-base-v5`） |
| **20 分代码提交（终点）** | commit `5619d5c4`（`perf(hipvs): halve the CAGRA intermediate degree, buy the recall back with a larger candidate list`） |
| 文档 tip | commit `0cc9b130`（只改文档，代码与 `5619d5c4` 逐字节相同） |
| hipVS（只读，不可改） | `AMD-Ecosystem/hipVS@f96a663d0a4441d6f5344bd3ef552b71ae7b370b` |
| 官方成绩 | 第 17 轮，平台记录 id `47253435`，`build=3.220x, query=1.699x, gates=pass`，`machineScore=20000`，`maxScoreFlag=1` |

**起点必须是 `634888d9` 的后代**，否则评测机直接判 `failMsg=8`（基线校验失败）。

### 从冻结基线到 20 分，一共只改了 4 个文件

```bash
git diff --stat 634888d9b037153fa143d2c913ba4b25503e1d62 5619d5c4 -- src tools
```

```
 src/oblib/lib/vector/ob_cuvs_snapshot.h  | 172 +++++++    # 快快照：索引快照只存图，不存向量
 src/oblib/lib/vector/ob_vsag_adaptor.cpp  | 263 ++++++-    # 建图后端选择 + 快快照落盘
 tools/hipvs_bridge/seekdb_cuvs_bridge.c  | 763 ++++++++++-  # CAGRA 参数、图导出、检索与降级兜底
 tools/hipvs_bridge/seekdb_cuvs_bridge.h  |  43 ++         # C ABI
 4 files changed, 1213 insertions(+), 28 deletions(-)
```

**这是本方案最大的优点：改动面极小、可逐块审阅、可直接 cherry-pick。**

---

## 2. 真正起作用的参数（`tools/hipvs_bridge/seekdb_cuvs_bridge.c`）

| 参数 | 冻结基线 | 第 14 轮（15.977 分） | **第 17 轮（20.000 分）** | 说明 |
|---|---|---|---|---|
| `SEEKDB_CUVS_GRAPH_DEGREE` | 128 | 128 | **64** | cuVS 要求 `igd ≥ gd`，降 igd 必须带着降 gd |
| `SEEKDB_CUVS_INTERMEDIATE_DEGREE` | 256 | 128 | **64** | **主杠杆**：host 侧 `optimize()` ∝ `n × igd` |
| `SEEKDB_CUVS_NN_ITER` | 100 | 20 | 20 | 迭代只值 ~11 ms/次（评测机），不值得再砍 |
| `SEEKDB_CUVS_ITOPK_MIN` | 512（写死） | 1024 | **2048** | 召回补偿：候选表变长，`itopk × gd` 不变 ⇒ 查询耗时不变 |
| `SEEKDB_CUVS_SEARCH_WIDTH` | 4 | 4 | 4 | 无所谓 |
| 检索 `algo` | SINGLE_CTA（默认！） | **AUTO** | **AUTO** | **必须显式指定**，见 §5 坑 ① |
| 快快照 | 无 | 有 | 有 | 索引快照只存图 ⇒ 省 3.11 s |

对应的配置文件 key（同名小写）：`graph_degree` / `intermediate_degree` / `nn_iter` /
`build_algo` / `itopk_min` / `search_width` / `log` / `fast_snapshot`。

---

## 3. 编译（评测机固定命令，一字不改）

```bash
# 1) 桥接层
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so

# 2) seekdb
cmake -S . -B build_release -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so
cmake --build build_release --target seekdb
```

- **离线**：不得新增任何联网依赖（`deps/3rd` 要用缓存目录软链，见 `validation/run_cloud_benchmark.sh`）。
- 云端自建环境的已知偏差（照抄即可，不必重新踩）：
  Rust 走 stable 通道（1.99.1）、`deps` 裁掉 `obshell` / `obclient` / `ob-deploy`、
  `NEED_PARSER_CACHE=OFF`、系统装 `m4` / `cpio` / `rpm` / `libaio1t64`。

---

## 4. 评测输入（**不可改**）

```
FP32 + L2
CREATE VECTOR INDEX ... WITH (type=hnsw, lib=cuvs, m=16, ef_construction=200, ef_search=64)
```

评测 SQL、隐藏集、`m` / `ef_construction` / `ef_search` 全部由平台给定，**改任何一个都算作弊**。

---

## 5. 四个必须知道的坑（踩中一个，这一轮直接 0 分或归零）

### ① 检索 `algo` 必须显式设成 `AUTO`

`cuvsCagraSearchParamsCreate()` 的默认值是 `SINGLE_CTA`，而 **`SINGLE_CTA` 拒绝 `itopk > 512`**。
后果不是报错，而是：检索全部失败 → seekdb **静默回退到 CPU 路径**（`lib=cuvs` 根本没有 CPU 索引）
→ 返回空结果 → 挂「结果完整性」门槛，召回掉到 VSAG 的 0.766 / 0.652。

桥接层里已经做了保护（`sk_cagra_search()`）：失败先换 multi-CTA kernel，再把候选表
2048 → 1024 → 512 逐级减半重试。**但兜底只保「不空结果」，不保召回**——它救不了 `itopk` 被拒的情况。

### ② 图导出不能用 `cudaMemcpy2D`

评测机编译桥接层用的是 hipVS 的 `cuvs/cuda_runtime.h`（CUDA→HIP 重定向 shim）。它重定向
`cudaMemcpy` / `cudaMemcpyDefault` / `cudaMemcpyDeviceToHost` / `cudaSuccess`，
**唯独不重定向 `cudaMemcpy2D`**（只有 `cudaMemcpy2DAsync`）。
⇒ 用 `cudaMemcpy2D` 导图会**编译期报未声明标识符**，整轮 15 秒就挂（`failMsg=3`）。
正确做法：一维 `cudaMemcpy` + host 端把 padded block 压紧。

### ③ 恢复线程栈必须用 32 MB pthread

250,000 条数据的恢复路径原本跑在 OB 请求线程（约 1.5 MB 栈）上会爆栈。
必须改成和 blob 路径一样的 32 MB pthread。

### ④ 编译校验必须用真实头文件

本地能编译过不算数。每个被 stub 的头文件都要问一句「新代码有没有用到它」。
仓库里的校验脚本对**真实** `cuvs/cuda_runtime.h` 和 `dlpack/dlpack.h` 编译，并带一个
**必须拒绝 `cudaMemcpy2D` 的负控**。

> 诊断口诀：`testEndTime − testStartTime` 只有 ~15 秒 ⇒ build 阶段就挂了；
> 3~14 分钟 ⇒ 真的跑完了测试。**先用时间差判「编译挂」还是「测试挂」，别急着读错误信息。**

---

## 6. 为什么这组参数能拿 20 分

### 6.1 评分式（从官方成绩反推，已被 17 轮验证）

```
总分 = 4 + 14·ln(构建比)/ln(3.0) + min(2, 2·ln(查询比)/ln(1.5))
```

⇒ **构建比 ≥ 3.0x、查询比 ≥ 1.5x 就是满分 20**。查询项在第 11 轮就已封顶，
**所有余量全在构建侧**。

### 6.2 建图耗时分解（评测机口径，CPU VSAG 基线建图 ≈ 23.0 s）

| 阶段 | 改动 | T_ours | 构建比 |
|---|---|---|---|
| 冻结基线 | gd128 / igd256 / it100 / itopk512 | 20.81 s | 1.105x |
| 第 10 轮 | igd 256→128 + it 100→50 + itopk 512→1024 | 13.96 s | 1.648x |
| 第 11 轮 | itopk 钳位 ⇒ **快快照生效**（快照不写向量） | 10.85 s | 2.120x |
| 第 14 轮 | it 50→20 | 10.51 s | 2.188x |
| **第 17 轮** | **gd 128→64 + igd 128→64 + itopk 1024→2048** | **7.14 s** | **3.220x** |

最后一步省了 **3.37 s / 64 度 ≈ 53 ms/度**，与前几轮独立测出的 **≈48 ms/度**吻合
—— 这就是 host 侧 `optimize()` 的斜率。

### 6.3 为什么 gd 必须跟着 igd 降

cuVS 的 CAGRA 里（`cuvs/neighbors/cagra/cagra_build.cuh`）：

```cpp
// NN-descent 的图度被强制对齐到 CAGRA 的 intermediate degree
if (nn_descent_params.graph_degree != intermediate_degree) {
  RAFT_LOG_WARN("... aligning nn-descent graph_degree");
  nn_descent_params = graph_build_params::nn_descent_params(intermediate_degree, params.metric);
}
build_knn_graph<T, IdxT>(res, dataset, knn_graph->view(), nn_descent_params);

// 最终图由 host 侧 optimize() 产出：读中间图，每个点保留 graph_degree 条边
cagra_graph = raft::make_host_matrix<IdxT, int64_t>(dataset.extent(0), graph_degree);
optimize<IdxT>(res, knn_graph->view(), cagra_graph.view(), params.guarantee_connectivity);
```

`intermediate_degree < graph_degree` **不是报错**，只是 warning + 把 `graph_degree` 下调到 `igd`。
所以两者一起降到 64 合法且自洽。

### 6.4 召回怎么补回来（关键换算）

64 度的图更稀疏，召回会掉（250K 合成数据，`itopk=1024`，`sw=4`，AUTO）：

| gd / igd / it | recall@100 | recall@10 | ms / 100 次查询 |
|---|---|---|---|
| 128 / 128 / 20 | 0.9832 | 0.9920 | 6.26 |
| 64 / 128 / 20 | 0.9342 | 0.9660 | **3.17** |
| 64 / 128 / 50 | 0.9542 | 0.9780 | 3.09 |

冻结基线（同轮）recall@100 ≈ 0.965，门槛是「相比同轮冻结基线下降 ≤ 0.005（相对）」⇒ 需要 ≥ 0.960。
**只把 gd 降到 64 不够**（0.9342 会挂门槛）。

但 64 度图**检索快约 2 倍**（束搜索代价 ∝ `itopk × gd`）。把省下的查询预算换成更长的候选表
（`itopk` 1024 → 2048），每查询的候选数 `itopk × gd` 回到原来的 `2048×64 = 1024×128`：

> **查询耗时不变，召回买回来了。**

实测证实：第 17 轮查询比不降反升（1.583x → **1.699x**），召回门槛全过。

### 备选方案（如果你的 cuVS 不接受 `itopk=2048`）

`itopk=2048` 在本轮的 hipVS（`f96a663d`）上是**被接受的**，但 cuVS 的 MULTI_CTA kernel 上限随版本而变，
源码里没有硬校验。如果你的环境拒绝了 2048，有一个**不依赖大候选表**的等效配置：

| 配置 | recall@100 | 构建比（估算） | 说明 |
|---|---|---|---|
| `gd=64 / igd=64 / it=20 / itopk=2048` | ≈0.99 | **3.22x**（实测） | 本轮采用的配置，召回余量最大 |
| `gd=64 / igd=64 / it=100 / itopk=1024` | 0.9633 | ≈3.03x | **不依赖 itopk>1024**；代价是多 80 次迭代（≈0.45 s），召回余量只有 0.003 |

第二条路线的召回余量很薄（0.9633 vs 门槛 0.9601），只有在 2048 确实被拒时才用。
**优先按第一条走。**

---

## 7. 已证伪的方向（别再花轮次）

| 方向 | 结论 | 证据 |
|---|---|---|
| **减少索引快照字节数** | ❌ 不是杠杆。字节数 ÷2（258→130 MB）反而**慢** 0.45 s；写入是缓冲的，不按字节计费 | 第 16 轮 `1d89dcea`（15.375） |
| **改 LOB 分片大小** | ❌ 无收益（分片 ÷4） | 第 15 轮 `c60fd055`（15.937） |
| **GPU 运行时预热** | ❌ 零收益。同 commit 重投仍是 2.114x，不是 2.188x | 第 12 / 13 轮 |
| `IVF_PQ` 建图 | ❌ 快 5.4× 但召回只有 0.877 | 云端 sweep |
| `ITERATIVE_CAGRA_SEARCH` | ❌ 比 nn-descent 还慢 | 云端 sweep |
| 「降 NN-descent 内部图度、保留 gd=128」 | ❌ **不存在**。cuVS C API 没有 `graph_build_params`，只有 `intermediate_graph_degree` | `hipvs_cpp_include_cuvs_neighbors_cagra.h` |
| 降 `nn_iter` 换召回 | ❌ 换不回来。评测机只值 ~11 ms/次（igd=128），30 次才 0.34 s；而 gd=64 时 `it=50 / itopk=1024` 的 recall@100 只有 **0.9542**，仍低于门槛 0.960 | 云端 sweep + 平台实测 |

---

## 8. 提测前自检清单

- [ ] 起点 commit 是 `634888d9` 的后代（否则 `failMsg=8`）
- [ ] 桥接层对**真实** hipVS 头文件编译通过，负控能拒绝 `cudaMemcpy2D`
- [ ] `docs/gpu-vector-index-hipvs-cuvs/validation/run_gpu_smoke.sh` 全绿
      （GPU 路径抽检、`lib=vsag` 隔离、过滤/删除/新鲜度回退、`batch_knn` 召回、GPU fd）
- [ ] 建图日志出现 `fast_snapshot_supported=1`（否则快快照没生效，少赚 3.11 s）
- [ ] 检索日志里的 `itopk` 被钳位到 ≤ 索引点数（快快照生效的前提）
- [ ] 检索参数 `algo=AUTO`（不是 `SINGLE_CTA`）
- [ ] 评测输入 / SQL / `m` / `ef_construction` / `ef_search` 一个字没改
- [ ] 提测平台：详情页 → 「立即提测」→ 填 **Commit id** + **Branch** → 「提 交」
      （提测次数是消耗品，平台取最高分，失败轮不覆盖已有成绩）

---

## 9. 证据与脚本

| 内容 | 位置 |
|---|---|
| 完整推演与 17 轮台账 | [`path-to-20-points.zh-CN.md`](./path-to-20-points.zh-CN.md) / [`build-performance.zh-CN.md`](./build-performance.zh-CN.md) |
| 成本模型（§11）、hipVS 源码证据（§13.1） | `path-to-20-points.zh-CN.md` |
| 建图耗时 × 召回 Pareto 图 | [`CPU-GPU构建性能对比-pareto.svg`](./CPU-GPU构建性能对比-pareto.svg) |
| 快快照设计 | [`fast-snapshot.md`](./fast-snapshot.md) |
| GPU 正确性冒烟 | `validation/run_gpu_smoke.sh` |
| 构建基准 | `validation/run_build_benchmark.sh`、`validation/run_cloud_benchmark.sh` |
| CAGRA 建图/召回扫描 | `validation/run_cagra_sweep.sh`、`validation/sweep_cagra.c`、`validation/analyze_cagra_sweep.py` |
| `itopk` / `algo` 上限探针 | `validation/itopk_probe.c` |
| 扫描原始日志 | `validation/evidence/sweep2_out.txt`、`sweep3_out.txt`、`itopk_probe_out.txt` |

---

## 10. 一页纸速查（贴给队友）

```
目标：20/20 = 构建比 ≥3.0x + 查询比 ≥1.5x，其余是门槛分。

唯一有效的改动：
  intermediate_degree 256 → 64   （host 侧 optimize() ∝ n×igd，≈48~53 ms/度）
  graph_degree         128 → 64   （cuVS 强制 gd ≤ igd，必须一起降）
  itopk 下限           512 → 2048 （64 度图检索快 2 倍，预算换候选表买回召回）
  nn_iter              100 → 20
  检索 algo            必须 AUTO   （SINGLE_CTA 拒绝 itopk>512，会静默回退 CPU）

必须做到：
  快快照生效（itopk 钳位到 ≤ 索引点数）——省 3.11 s
  图导出不用 cudaMemcpy2D（hipVS 的 shim 不重定向它）——否则编译挂
  恢复线程栈 32 MB pthread——否则 250K 行爆栈

已证伪，别试：快照压缩、LOB 分片、GPU 预热、IVF_PQ、ITER_CAGRA_SEARCH。
```

---

> 附：本次全部 17 轮提交的完整台账、每一轮的失败原因与平台原始回显，都在
> [`path-to-20-points.zh-CN.md`](./path-to-20-points.zh-CN.md) 里，可直接作为评审材料。
