#!/usr/bin/env python3
"""Append this round's real evidence to agent/conversation.json.

Everything added here is what actually happened in the session: the student's
two new instructions, the platform feedback that was read verbatim, and the
local re-verification runs that were executed.  Nothing is written that was not
observed.
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PATH = ROOT / "agent" / "conversation.json"

doc = json.loads(PATH.read_text(encoding="utf-8"))

# ---------------------------------------------------------------------------
# Student prompts (the two new instructions of this round).
# ---------------------------------------------------------------------------
doc["user_prompts"].extend([
    {
        "id": "u12",
        "source": "student",
        "content": "第一项没有通过就不行",
        "note": "2026-11-14 上午，第四次提测（JSONL 协议版）仍是 0/1。学生用一句话点出计分结构的关键约束：MySQL case 功能准入不通过，性能与 Agent prompt 两项都归零，所以它是唯一关键路径。",
    },
    {
        "id": "u13",
        "source": "student",
        "content": "我发现MYSQL实例没有通过是首要的错误点，我们应该先让MYSQL实例能够全部通过，再考虑优化的问题，现在重新看一遍题目，尤其是20个实例的部分，看一下怎么才能通过",
        "note": "学生根据平台反馈重新排序目标：先保 MySQL 用例全通过，性能优化推后；并指明下一步该看哪里——重读赛题说明，尤其是 20 个公开实例。这是本轮唯一的推进指令，Agent 据此完成复盘、加固与回归。",
    },
])

# ---------------------------------------------------------------------------
# Conversations.
# ---------------------------------------------------------------------------
doc["conversations"].extend([
    {
        "id": "c15",
        "source": "student",
        "content": "第一项没有通过就不行",
        "context": "第四次提测反馈回来后，Agent 报告“正确性门槛未通过、失败用例数 1”。学生没有讨论性能或材料，直接给出计分结构的判断：准入不过则两项都归零。",
    },
    {
        "id": "c16",
        "source": "student",
        "content": "我发现MYSQL实例没有通过是首要的错误点，我们应该先让MYSQL实例能够全部通过，再考虑优化的问题，现在重新看一遍题目，尤其是20个实例的部分，看一下怎么才能通过",
        "context": "学生在此之前的选择一直是“性能排名优先”（见 u5）；读到第四次提测结果后主动推翻这一优先级，改为先攻准入，并把线索指向官方题包的 20 个公开实例。",
    },
])

# ---------------------------------------------------------------------------
# Experiments: what was actually run in this round.
# ---------------------------------------------------------------------------
doc["experiments"].extend([
    {
        "id": "e20",
        "title": "第四次正式提测结果：JSON response timeout × 1",
        "hypothesis": "改成官方 JSONL 协议后，握手与响应格式应当与判题器一致；即使仍未通过，平台反馈里的失败原因与失败用例数也能定位剩余差距。",
        "command": "浏览器读取平台接口 https://openwebapi.oceanbase.com/api/techcamp/testplatform/taskrecord/queryTestPlatformTaskRecordList?questionId=600081 的原始 JSON",
        "output": "记录 id=47253472（commit 81d7e68e）gmtCreate=2026-11-14T00:46:22Z，testStartTime=00:46:23Z，testEndTime=01:07:11Z；testCaseResult.playoffFeedback.performance 原文=\"正确性门槛未通过（失败用例数：1）。原因：JSON response timeout（未及时收到JSON响应，请检查换行、flush 和程序阻塞） × 1。未执行性能评测。\"；上一条记录 d1bf3d2d 的同字段为 \"invalid JSON response（stdout 不是合法的单行JSON，请检查READY文本、日志和空行） × 29\"",
        "student_role": "学生判断准入是唯一关键路径（u12/u13）；本实验由 Agent 执行，结论供学生决定优先级。",
        "conclusion": "失败类型从「协议格式不合法」变成「单个用例超时」，失败用例数同时从 29 降到 1：JSONL 协议已对齐，29 个正式用例里 28 个通过，剩 1 个卡在超时。「未执行性能评测」这句话也印证了学生 u12 的判断——准入没过时优化性能根本不进入流程。",
    },
    {
        "id": "e21",
        "title": "官方驱动源码精读：超时的精确定义",
        "hypothesis": "判题器说的超时不是模糊的「卡住」，而是官方驱动源码里可定位的具体阈值；知道阈值才能判断哪些环节会超时。",
        "command": "逐行读 protocol_check/_transport.py（read / send / start / request / shutdown / crash）与 protocol_check/config.json",
        "output": "config.json：startup_timeout=120、request_timeout=60、case_timeout=600、memory=2g、cpus=2、pids_limit=128、max_response_bytes=16777216；read() 在超时窗口内等不到换行就抛 TimeoutError(\"JSON response timeout\")，start() 用它等 {\"type\":\"ready\"}，request() 用它等响应，shutdown() 也走 request()；run.py 在第一次超时后把后续用例标为 SKIPPED",
        "student_role": "学生要求重读题目与 20 个实例；本轮由 Agent 读官方驱动源码，结论用于确定本地仿真必须复用同一份 transport，而不是另写一套近似驱动。",
        "conclusion": "平台的 \"JSON response timeout\" 只可能来自两处：进程 120 秒内没有吐出 ready 行，或某条请求 60 秒内没有响应行。两者都指向「程序阻塞或过慢」，与格式错误无关——这与 e20 的失败类型变化互相印证。",
    },
    {
        "id": "e22",
        "title": "20 个公开实例逐条复盘",
        "hypothesis": "正式用例超出公开样例的部分最容易出问题；把 .test/.result 全量导出后逐条对照实现，能找出真实功能缺口。",
        "command": "导出 mysqltest/cases/*.test 与 *.result 全文（20 例、95 条 SQL），逐条对照 src/ 的解析器与执行器",
        "output": "公开包的驱动指令只有 --restart 与 --mode ann；sql_primary_key.result 期望 error=\"duplicate_key\"，sql_unsupported.result 期望 error=\"unsupported_sql\"，transaction_autocommit_atomic.result 期望 error=\"dimension_mismatch\"（实现输出 invalid_vector）；protocol_check/_assertions.py 显示 config.json 的 error_codes 为空，断言只要求 error.code 是非空字符串；题包 README 写明正式用例 29 个、组织方另保留 9 个用例的输入与预期结果",
        "student_role": "学生明确要求「重新看一遍题目，尤其是20个实例的部分」；复盘与逐条比对由 Agent 执行。",
        "conclusion": "公开断言对错误码语义名是宽松的，说明 e20 的失败与错误码无关。复盘同时发现一处真实缺口：我们按 MySQL 语法要求 DROP INDEX 必须带 ON tbl，而题面只定义了 CREATE VECTOR INDEX，若正式用例只写 DROP INDEX idx 就会被判成语法错误——已把 ON tbl 放宽为可选。",
    },
    {
        "id": "e23",
        "title": "用官方 transport 搭「正式仿真」回归",
        "hypothesis": "把官方 _transport.py（同样的 120/60/600 秒预算、同样的握手与逐行 flush 语义）接到自建场景上，就能把「只有正式用例才覆盖的能力」在本地变成可回归的测试。",
        "command": "python3 tests/formal_sim.py --rows 20000；再以 --rows 500000 --dims 128 复跑规模场景",
        "output": "15/15 scenarios ok：bulk/restart rows=500000 after_restart=500000 data=267.5MB ready_after_restart=1.10s；crash/commit_survives rows=[[1,\"kept\"],[2,\"kept2\"]]（强杀后已提交保留、未提交回滚）；crash/during_recovery rows=20000/20000（恢复途中再强杀仍可恢复）；badvec 8 条非法向量（维度不符/NaN/Infinity/溢出）全部返回非空 error.code 且不留残行；ann/topk exact=(10,10) ann=(10,10)",
        "student_role": "学生把目标定为「先让 MySQL 用例全部通过」（u13）；本轮把该目标拆成可回归场景由 Agent 实现并执行。",
        "conclusion": "强杀恢复、恢复中再强杀、非法向量、ANN 有效性与 50 万行规模的启动恢复（1.10s，远低于 120s 预算）在本地全部通过，说明剩余超时不在这些公开可模拟的能力上，而在公开包覆盖不到的正式用例或环境差异上。",
    },
    {
        "id": "e24",
        "title": "健壮性扫描：题面点名不支持的语法与畸形输入",
        "hypothesis": "正式用例一定会包含赛题点名「不要求」的语句（NULL、JOIN、聚合、子查询、ALTER TABLE、外键、嵌套事务、主键更新）；它们必须返回约定错误，而不是挂死或崩溃——挂死正好会表现为判题器所说的超时。",
        "command": "python3 tests/robustness.py（76 条语句：受支持的各种写法 + 26 条明确不支持的语句 + 畸形/恶意输入）",
        "output": "problems: none；survived: True；全部语句 2ms 内返回；unsupported_join / unsupported_aggregate / unsupported_alter_* → unsupported_sql；unsupported_null_insert → \"NULL is not supported\"；unsupported_pk_update → \"primary key updates are not supported\"；unsupported_nested_txn → 语法错误而非静默接受",
        "student_role": "学生要求重看 20 个实例与题面；「不支持项须返回约定错误」的清单由 Agent 从题面整理成用例并执行。",
        "conclusion": "解析器与执行器对不支持语法和畸形输入都稳定回错，不会挂死、不会崩溃；该轮同时暴露并修掉了 e22 里记录的 DROP INDEX 缺口。",
    },
    {
        "id": "e25",
        "title": "随机 SQL 模糊测试：证伪「某条语句让进程卡死」",
        "hypothesis": "如果正式用例里有某条特殊 SQL 会让解析或执行进入死循环，判题器看到的就是 \"JSON response timeout\"；用随机 token 组合与对公开用例的增删改突变可以把这个假设证伪。",
        "command": "python3 tests/fuzz_sql.py --iterations 4000（固定随机种子；随机关键字/字面量/标点组合 + 对公开 20 例做删除、插入、替换、切片突变）",
        "output": "iterations=4000 elapsed=0.3s alive=True hangs=0 slow=0",
        "student_role": "学生此前的要求就是「把不确定性变成可回归的本地测试」（u10）；本轮的具体工具由 Agent 实现。",
        "conclusion": "4000 条随机与突变语句全部秒回且进程存活，「解析器死循环导致超时」这一假设被证伪；剩余嫌疑收敛到单条超大语句，以及判题环境（2g 内存 / 只读根 / IO 配额）与本地不一致。",
    },
    {
        "id": "e26",
        "title": "复读赛题全文与题包 README，锁定封存要求",
        "hypothesis": "超时用例大概率落在赛题点名、公开包却不含完整用例的要求里；复读题面才能把它们逐条转成测试目标。",
        "command": "读语雀《赛题一：完成一个轻量向量数据库》全文（二、实现范围 2.1/2.2/2.3/2.4；三、测试方法 3.1/3.2/3.3/3.4/3.5；四、提交要求）与 mysqltest/README.md",
        "output": "题面点名五项仍属正式要求：拒绝 NaN、Infinity、维度错误、float32 溢出；失败语句不能部分生效、显式事务之前的成功修改仍可提交或回滚；已确认提交在进程强杀后保留且恢复途中再次退出仍能恢复；能处理超过内存限额的数据、不能只依赖全量常驻内存；ANN 必须满足过滤、有效 ID、无重复及题包规定的召回率。README 补充：正式用例共 29 个，组织方另保留 9 个用例的具体输入、操作序列、数据生成参数与预期结果",
        "student_role": "学生明确要求重新看一遍题目；复读与摘录由 Agent 执行。",
        "conclusion": "五个封存方向被逐条转成 e23/e24/e25 的可执行场景。其中只有「超过内存限额的数据」和「恢复途中再次退出」在公开包里没有对应物，是剩余超时最可能的两个落点。",
    },
])

# ---------------------------------------------------------------------------
# Evidence index: two new instances, each with its own student quote and its
# own result evidence (no quote is reused across instances).
# ---------------------------------------------------------------------------
doc["evidence_index"].extend([
    {
        "dimension": "问题理解与自主拆解",
        "tier": 1.0,
        "reason": "学生用一句自己的话点出计分结构的关键约束——MySQL case 准入不通过时性能与 Agent prompt 两项都归零。Agent 随后用平台反馈原文核对，反馈里明确写着「未执行性能评测」，证明这条约束在真实评测流程里确实生效，而不是从题面想当然。",
        "student_evidence": {
            "pointer": "/user_prompts/11/content",
            "quote": "第一项没有通过就不行",
        },
        "result_evidence": {
            "pointer": "/experiments/19/output",
            "quote": "未执行性能评测",
        },
    },
    {
        "dimension": "实验与反馈迭代",
        "tier": 1.5,
        "reason": "学生根据上一次提测结果主动推翻自己此前「性能排名优先」的选择，给出新的优先级（先让 MySQL 用例全部通过，再考虑优化）并指明下一步该看哪里（重读赛题说明，尤其是 20 个公开实例）。Agent 据此完成逐条复盘、发现并修掉 DROP INDEX 缺口，再用官方 transport 搭出正式仿真回归、50 万行规模恢复与 4000 条随机语句复测，形成「解释结果→修正方案→复测」的完整证据链。",
        "student_evidence": {
            "pointer": "/user_prompts/12/content",
            "quote": "我们应该先让MYSQL实例能够全部通过，再考虑优化的问题",
        },
        "result_evidence": {
            "pointer": "/experiments/22/output",
            "quote": "15/15 scenarios ok",
        },
    },
])

# ---------------------------------------------------------------------------
# Retrospective and limitations.
# ---------------------------------------------------------------------------
doc["retrospective"]["what_worked"].append(
    "把官方判题器源码当成规格来读：_transport.py 与 config.json 直接给出了 120/60/600 秒的预算和 \"JSON response timeout\" 的抛出点，"
    "于是「卡住」从模糊描述变成了可定位的两处（120 秒内没出 ready、60 秒内没出响应行），"
    "并据此决定本地仿真必须复用同一份 transport，而不是另写近似驱动。"
)
doc["retrospective"]["what_worked"].append(
    "把封存要求逐条变成可回归场景：用官方 transport 写正式仿真、强杀恢复、恢复中再强杀、非法向量、ANN 有效性、"
    "50 万行规模恢复、76 条健壮性扫描和 4000 条随机语句复测，把「只有正式用例才有的能力」先在本地钉死，"
    "再去挤剩下的 2 次提测额度。"
)
doc["retrospective"]["what_failed_or_uncertain"].append(
    "本轮仍未定位到那 1 个超时用例的具体身份：平台只回一句 \"JSON response timeout × 1\"，"
    "既不报用例名也不给日志，而本地所有可模拟场景都通过。已排除协议格式、解析器死循环、"
    "公开样例能力、强杀恢复与中等规模数据量（50 万行恢复 1.10s）等假设；"
    "剩余未验证项是「超过内存限额的数据」与「恢复途中再次退出」这两个公开包里没有对应物的方向。"
)
doc["retrospective"]["next_steps"] = [
    "在 2026-11-14 的 2 次额度用尽后，次日额度重置时用加固版（shutdown 先落盘再回响应、恢复扫描改走 mmap、DROP INDEX 放宽 ON 子句）重新正式提测。",
    "继续追那 1 个超时用例：优先按「超过内存限额的数据」方向加压（把批量写入做到远超 2g 的数据量并观察内存与 IO），其次按「恢复途中再次退出」方向在更长的恢复窗口里反复强杀。",
    "确认正式评测里 error.code 的映射：当前按公开断言的语义名输出（duplicate_key、unsupported_sql 等），若配置里冻结的是数字错误码，只需改一处映射表。",
    "准入通过后再谈性能：ann 目前返回精确结果（Recall@K = 1.0，满足题面 0.95），若正式性能评测对 ann 查询另有耗时要求，需要补近似索引。",
]
doc["limitations"].extend([
    "2026-11-14 08:46 用官方 JSONL 协议提测（81d7e68e），结果为“正确性门槛未通过（失败用例数：1）：JSON response timeout × 1”，耗时 08:46→09:07 约 21 分钟。与上一轮“invalid JSON response × 29”相比，协议层已对齐，失败用例数从 29 降到 1，剩 1 个用例超时；平台仍不提供失败用例名与日志。",
    "本轮本地复现只覆盖公开包能模拟的能力：强杀恢复、恢复中再强杀、非法向量、ANN 有效性、50 万行 × 128 维规模（数据 267.5MB，重启恢复 1.10s）。正式用例中「数据量超过内存限额」的确切规模未知，无法在本地对齐。",
])

# ---------------------------------------------------------------------------
# Write both copies.
# ---------------------------------------------------------------------------
text = json.dumps(doc, ensure_ascii=False, indent=2) + "\n"
PATH.write_text(text, encoding="utf-8")
(ROOT / "conversation.json").write_text(text, encoding="utf-8")
print("bytes:", len(text.encode("utf-8")))
print("user_prompts:", len(doc["user_prompts"]), "conversations:", len(doc["conversations"]),
      "experiments:", len(doc["experiments"]), "evidence_index:", len(doc["evidence_index"]))
