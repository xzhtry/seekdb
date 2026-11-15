#!/usr/bin/env python3
"""Strict validation of the material against the rules the platform states.

The platform reads agent/conversation.json from the locked commit and rejects
it when it is not UTF-8, not valid JSON, not a top level object, larger than
128 KiB, has duplicate keys, or contains non-standard JSON numbers.
"""
import json
import re
import sys
from pathlib import Path

PATH = Path(sys.argv[1] if len(sys.argv) > 1 else "agent/conversation.json")
raw = PATH.read_bytes()

problems = []

try:
    text = raw.decode("utf-8")
except UnicodeDecodeError as exc:
    print("FAIL: not UTF-8: %s" % exc)
    sys.exit(1)

if len(raw) > 128 * 1024:
    problems.append("size %d bytes > 128 KiB (%d)" % (len(raw), 128 * 1024))

dups = []


def hook(pairs):
    keys = [k for k, _ in pairs]
    if len(keys) != len(set(keys)):
        dups.append(sorted({k for k in keys if keys.count(k) > 1}))
    return dict(pairs)


def boom(token):
    raise ValueError("non-standard JSON constant: %s" % token)


try:
    doc = json.loads(text, object_pairs_hook=hook, parse_constant=boom)
except Exception as exc:  # noqa: BLE001
    print("FAIL: JSON parse: %s" % exc)
    sys.exit(1)

if dups:
    problems.append("duplicate keys: %s" % dups[:5])
if not isinstance(doc, dict):
    problems.append("top level is %s, not an object" % type(doc).__name__)

for pat, name in ((r'github_pat_[A-Za-z0-9_]+', 'github PAT'),
                  (r'(?i)"(api[_-]?key|access[_-]?token|password|secret)"\s*:', 'credential field')):
    hits = re.findall(pat, text)
    if hits:
        problems.append("%s x%d" % (name, len(hits)))

inst = doc.get("evidence_index", [])
for i, e in enumerate(inst):
    tier = e.get("tier")
    se = e.get("student_evidence")
    re_ = e.get("result_evidence")
    if tier is None or tier > 0.5 and not re_:
        problems.append("instance %d: tier %s without result_evidence" % (i, tier))
    if not se:
        problems.append("instance %d: no student_evidence" % i)

print("file: %s" % PATH)
print("size: %d bytes (%.1f KiB), limit 128 KiB" % (len(raw), len(raw) / 1024.0))
print("top level keys: %d" % len(doc))
print("instances: %d, claimed tier sum: %.1f" % (len(inst), sum(float(e.get("tier", 0)) for e in inst)))
print("problems: %s" % (problems if problems else "none"))
sys.exit(1 if problems else 0)
