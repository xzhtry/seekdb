#!/usr/bin/env python3
"""Record this round's container-level verification in agent/conversation.json.

These are experiment records only: the student did not take part in running
them, so they are not student evidence.  They document what was verified.
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PATH = ROOT / "agent" / "conversation.json"

doc = json.loads(PATH.read_text(encoding="utf-8"))

doc["experiments"].extend([
    {
        "id": "e27",
        "title": "单请求延迟压测：200 万行 × 128 维",
        "hypothesis": "判题器对单条请求只给 60 秒，如果超时的原因是某条语句太慢，那么把数据量推到接近内存限额时应该能看到耗时逼近预算。",
        "command": "python3 tests/stress_single_request.py --rows 2000000 --dims 128 --one-shot-rows 100000",
        "output": "one-shot INSERT（单条语句 36.9 MiB）2.15s；exact TopK 无过滤 2.82s / 带过滤 2.72s；ann 模式带过滤 2.75s；全表过滤扫描 2.65s；重启握手 2.77s；重启后 TopK 2.72s——全部远低于 60s / 120s 预算",
        "student_role": "学生未参与本轮压测；记录在此用于说明验证过程，不作为学生学习证据。",
        "conclusion": "在 200 万行、约 1GB 数据（已接近容器 2GiB 内存限额）下，最慢的单条语句也只有 2.8 秒，比预算低两个数量级。“单条语句太慢”这条假设在本地被证伪，剩余差异只能来自判题环境本身或某个本地无法模拟的用例语义。",
    },
    {
        "id": "e28",
        "title": "强杀恢复延迟压测：100 万行且杀在未提交事务上",
        "hypothesis": "赛题 2.3 写明“正式测试包含数据量超过内存限额的场景”“向量索引重建成本计入恢复时间与资源限制”，判题器从启动到 ready 只给 120 秒；恢复若靠全量重算就会超时。",
        "command": "python3 tests/stress_recovery.py --rows 1000000 --dims 128（装载后 BEGIN + UPDATE，再 kill -9，然后重启；随后在恢复过程中再 kill -9 一次再重启）",
        "output": "loaded 1000000 rows in 19.8s, data 558.1 MiB；killed with an open transaction；ready after crash: 5.13s；rows after recovery: 1000000/1000000；ready after second kill: 1.30s；rows after repeated recovery: 1000000/1000000",
        "student_role": "学生未参与本轮压测；记录在此用于说明验证过程，不作为学生学习证据。",
        "conclusion": "带未提交事务被强杀后，恢复（含 WAL 回放与文件压实）只需 5.13 秒，未提交的修改正确消失；恢复途中再次强杀后仍能在 1.30 秒内就绪且数据完整。恢复耗时这条路径也远低于 120 秒预算。",
    },
    {
        "id": "e29",
        "title": "容器内回归与一个只在内核能力被裁剪时暴露的坑",
        "hypothesis": "本地与判题环境的差别主要在容器约束（2GiB 内存、2 核、只读根、pids 128、cap-drop ALL），把整套场景放进真实镜像里跑才能补上这个差别。",
        "command": ".github/workflows/minivectordb-ci.yml：用 protocol_check/run.py --image 跑公开用例，再跑 formal_sim / robustness / fuzz_sql / stress_single_request（均带 --image）",
        "output": "第一次 CI：Build image / Protocol handshake / Public cases 全部 success，只有新加的 formal simulation 步骤 failure（1 秒内失败）。定位到原因是测试脚手架而非数据库：formal_sim 用 tempfile 建了 0700 权限的宿主机目录并 bind mount 进容器，而判题器 cap-drop ALL 掉了所有能力（没有 CAP_DAC_OVERRIDE），容器内进程写不进 /data 立刻退出——官方自测脚本正是因此显式 chmod 777 数据目录，官方 driver 则改用 named volume。改为与 protocol_check/run.py 一致的 volume 模式后问题消失。",
        "student_role": "学生未参与本轮 CI 排障；记录在此用于说明验证过程，不作为学生学习证据。",
        "conclusion": "同一份镜像里，构建、握手与 20 个公开用例都通过，说明新代码在判题式约束下协议正确；另外学会了一个只有在“容器裁掉全部能力”时才会暴露的权限坑：容器内进程对自己不拥有的目录没有写权限，而 bind mount 会把宿主机的 0700 权限带进去。",
    },
])

doc["retrospective"]["what_worked"].append(
    "把超时这种模糊失败拆成可测的量：官方 driver 的 config.json 给出 120s（启动）/60s（单请求）/600s（用例）三个预算，"
    "于是“卡住”变成“哪个环节超过哪个预算”。随后按最坏单请求（2.8s @200 万行）、最坏恢复（5.13s @100 万行强杀）分别压测，"
    "把两个主要嫌疑都量化成了离预算两个数量级的实测值。"
)
doc["retrospective"]["what_failed_or_uncertain"].append(
    "第一次把场景搬进容器时踩了权限坑：bind mount 保留了宿主机 tempfile 的 0700 权限，而判题器 cap-drop ALL 后容器内进程没有 DAC_OVERRIDE，"
    "写不进 /data 直接退出。这条经验对提交物本身也有意义——如果判题方用 bind mount 挂 0700 目录，任何以 root 运行且依赖 DAC 覆盖的实现都会起不来；"
    "我们的镜像不设 USER（以 root 运行）且只依赖目录属主权限，是更安全的一侧。"
)
doc["limitations"].append(
    "本轮未能复现那 1 个超时用例：平台只回一句 “JSON response timeout × 1”，不报用例名也不给日志；"
    "本地已排除协议格式、解析器死循环、公开样例能力、强杀恢复、中等与大规模数据量（200 万行）等假设。"
    "另有一条外部信息：社区里已有队伍（Selenaj，2026-11-14）公开表示“MySQL 正式用例通过”，说明准入本身可过，但其实现未公开，无法直接比对。"
)
doc["retrospective"]["next_steps"] = [
    "2026-11-14 的两次额度已用尽（08:27 d1bf3d2d、08:46 81d7e68e）。次日额度重置后，先提交加固版（6ef3c836 起的改动：shutdown 先落盘再回响应、恢复扫描改走 mmap、DROP INDEX 放宽 ON 子句）。",
    "若仍是同一个 “JSON response timeout × 1”，则说明超时与恢复速度无关；下一版应把注意力放到“公开包里没有对应物的两个方向”——数据量超过内存限额的确切规模，以及恢复途中的反复强杀，并用更贴近判题 cgroup 限额的方式复现。",
    "准入通过后再谈性能与材料：ann 目前返回精确结果（Recall@K = 1.0，满足题面 0.95）；材料评审存在独立风险（社区里已有队伍因三轮评审分歧触发 NEEDS_REVIEW 而未参与排名），需要保证各维度证据稳定。",
    "确认正式评测里 error.code 的映射：当前按公开断言的语义名输出（duplicate_key、unsupported_sql 等），若配置里冻结的是数字错误码，只需改一处映射表。",
]

text = json.dumps(doc, ensure_ascii=False, indent=2) + "\n"
PATH.write_text(text, encoding="utf-8")
(ROOT / "conversation.json").write_text(text, encoding="utf-8")
print("bytes:", len(text.encode("utf-8")))
print("experiments:", len(doc["experiments"]), "evidence_index:", len(doc["evidence_index"]))
