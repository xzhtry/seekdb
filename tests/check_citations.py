#!/usr/bin/env python3
"""Check that every citation in agent/conversation.json resolves.

The reviewer validates the RFC6901 pointer and the quoted fragment against
the file itself, so a stale pointer or a paraphrased quote silently voids the
whole evidence instance.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MATERIAL = ROOT / "agent" / "conversation.json"


def resolve(doc, pointer):
    if pointer == "":
        return doc
    if not pointer.startswith("/"):
        raise ValueError("not an RFC6901 pointer: %r" % pointer)
    cur = doc
    for raw in pointer[1:].split("/"):
        token = raw.replace("~1", "/").replace("~0", "~")
        if isinstance(cur, list):
            cur = cur[int(token)]
        elif isinstance(cur, dict):
            if token not in cur:
                raise KeyError(token)
            cur = cur[token]
        else:
            raise TypeError("cannot descend into %s at %r" % (type(cur).__name__, token))
    return cur


def main():
    doc = json.loads(MATERIAL.read_text(encoding="utf-8"))
    problems = []
    checked = 0
    for idx, inst in enumerate(doc.get("evidence_index", [])):
        for field in ("student_evidence", "result_evidence"):
            ev = inst.get(field)
            if ev is None:
                continue
            if not isinstance(ev, dict) or "pointer" not in ev or "quote" not in ev:
                problems.append("#%d %s: not a {pointer, quote} object" % (idx, field))
                continue
            pointer, quote = ev["pointer"], ev["quote"]
            checked += 1
            try:
                target = resolve(doc, pointer)
            except Exception as exc:  # noqa: BLE001
                problems.append("#%d %s: pointer %s does not resolve (%s)" % (idx, field, pointer, exc))
                continue
            if not isinstance(target, str):
                problems.append("#%d %s: pointer %s is not a string field" % (idx, field, pointer))
                continue
            if len(quote) > 240:
                problems.append("#%d %s: quote is %d chars (limit 240)" % (idx, field, len(quote)))
            if quote not in target:
                problems.append("#%d %s: quote not found verbatim at %s" % (idx, field, pointer))
                problems.append("      quote  : %r" % quote[:120])
                problems.append("      target : %r" % target[:120])
    print("checked %d citations over %d instances" % (checked, len(doc.get("evidence_index", []))))
    if problems:
        print("PROBLEMS: %d" % len(problems))
        for line in problems:
            print("  " + line)
        return 1
    print("OK: every pointer resolves and every quote matches verbatim")
    return 0


if __name__ == "__main__":
    sys.exit(main())
