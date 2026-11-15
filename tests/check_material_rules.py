#!/usr/bin/env python3
"""Compare the submitted material (5a0292b0) with the new one (HEAD).

The platform validates the material it reads from the commit: UTF-8, valid
JSON, top level object, at most 128 KiB, no duplicate keys, no NaN/Infinity,
no secrets.  The sixth submission was accepted with the old material, so the
same checks on the old file tell us which rules still hold for the new one.
"""
import json
import re
import subprocess
import sys

REPO = "/mnt/d/oceanbase大赛/round2"


def strict_load(raw):
    def boom(x):
        raise ValueError("non-standard JSON constant: %s" % x)
    return json.loads(raw.decode("utf-8"), parse_constant=boom)


def dup_keys(raw):
    """Report duplicate object keys."""
    seen = []

    def hook(pairs):
        keys = [k for k, _ in pairs]
        if len(keys) != len(set(keys)):
            seen.append([k for k in keys if keys.count(k) > 1])
        return dict(pairs)

    json.loads(raw.decode("utf-8"), object_pairs_hook=hook)
    return seen


def check(label, raw):
    print("== %s: %d bytes" % (label, len(raw)))
    problems = []
    try:
        doc = strict_load(raw)
    except Exception as exc:  # noqa: BLE001
        print("   PARSE FAIL: %s" % exc)
        return
    if not isinstance(doc, dict):
        problems.append("top level is %s, not an object" % type(doc).__name__)
    if len(raw) > 128 * 1024:
        problems.append("size %d > 128 KiB" % len(raw))
    dups = dup_keys(raw)
    if dups:
        problems.append("duplicate keys: %s" % dups[:3])
    text = raw.decode("utf-8")
    for word in ("NaN", "Infinity"):
        if re.search(r'(?<![\w"])%s(?![\w"])' % word, text):
            problems.append("contains literal %s" % word)
    for pat, name in ((r'github_pat_', 'github PAT'), (r'(?i)\bapi[_-]?key\b', 'api key'),
                      (r'(?i)\baccess[_-]?token\b', 'access token'), (r'(?i)\bsecret\b', 'secret'),
                      (r'(?i)\bpassword\b', 'password')):
        hits = len(re.findall(pat, text))
        if hits:
            problems.append("keyword %s x%d" % (name, hits))
    print("   top-level keys: %d, instances: %d" % (len(doc), len(doc.get("evidence_index", []))))
    print("   problems: %s" % (problems if problems else "none"))


for label, rev in (("5a0292b0 (已提测通过)", "5a0292b0"), ("HEAD (待提测)", "HEAD")):
    raw = subprocess.run(["git", "-C", REPO, "show", "%s:agent/conversation.json" % rev],
                         capture_output=True, check=True).stdout
    check(label, raw)
    print()
