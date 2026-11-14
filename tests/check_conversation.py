#!/usr/bin/env python3
"""Validate agent/conversation.json against the submission requirements.

Checks (from the task statement):
  * UTF-8, valid JSON, top level is an object
  * size <= 128 KiB
  * no duplicate JSON keys, no NaN / Infinity
  * every evidence citation resolves through its RFC6901 pointer and the quote
    is an exact substring of the referenced string field (<= 240 chars)
  * the same quote is not reused across instances
  * no obvious secrets (tokens / passwords)
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATH = os.environ.get("MVDB_CONVERSATION",
                      os.path.join(ROOT, "agent", "conversation.json"))
MAX_BYTES = 128 * 1024

errors = []
warnings = []


def fail(msg):
    errors.append(msg)


def resolve(doc, pointer):
    """Resolve an RFC6901 JSON pointer."""
    if pointer == "":
        return doc
    if not pointer.startswith("/"):
        raise KeyError("pointer must start with '/': %r" % pointer)
    cur = doc
    for raw in pointer.split("/")[1:]:
        token = raw.replace("~1", "/").replace("~0", "~")
        if isinstance(cur, list):
            cur = cur[int(token)]
        else:
            cur = cur[token]
    return cur


def no_duplicates(pairs):
    seen = {}
    for k, v in pairs:
        if k in seen:
            raise ValueError("duplicate JSON key: %r" % k)
        seen[k] = v
    return seen


def main():
    raw = open(PATH, "rb").read()
    size = len(raw)
    print("file:", PATH)
    print("size: %d bytes (%.1f KiB), limit %d KiB" % (size, size / 1024.0, MAX_BYTES // 1024))
    if size > MAX_BYTES:
        fail("file larger than 128 KiB")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        fail("not valid UTF-8: %s" % exc)
        return 1
    try:
        doc = json.loads(text, object_pairs_hook=no_duplicates,
                         parse_constant=lambda c: (_ for _ in ()).throw(
                             ValueError("non standard JSON constant: %s" % c)))
    except ValueError as exc:
        fail("invalid JSON: %s" % exc)
        return 1
    if not isinstance(doc, dict):
        fail("top level must be an object")
    print("top level keys:", ", ".join(sorted(doc.keys())))
    if doc.get("synthetic") is not True:
        print("synthetic:", doc.get("synthetic"), "(real material)")

    quotes = {}
    for i, item in enumerate(doc.get("evidence_index", [])):
        for role in ("student_evidence", "result_evidence"):
            ev = item.get(role)
            if ev is None:
                continue
            pointer = ev.get("pointer", "")
            quote = ev.get("quote", "")
            if len(quote) > 240:
                fail("%s #%d quote longer than 240 chars" % (role, i))
            try:
                target = resolve(doc, pointer)
            except Exception as exc:
                fail("%s #%d pointer %r does not resolve: %s" % (role, i, pointer, exc))
                continue
            if not isinstance(target, str):
                fail("%s #%d pointer %r does not point at a string" % (role, i, pointer))
                continue
            if quote not in target:
                fail("%s #%d quote %r is not an exact substring of %s" % (role, i, quote, pointer))
            key = (role, pointer, quote)
            if key in quotes:
                fail("%s #%d reuses an earlier citation %r" % (role, i, pointer))
            quotes[key] = i

    for key, val in doc.items():
        if isinstance(val, str) and re.search(r"(github_pat_|ghp_|AKIA|BEGIN [A-Z ]*PRIVATE KEY)", val):
            fail("possible secret in field %r" % key)

    print("citations checked: %d" % len(quotes))
    for w in warnings:
        print("WARN", w)
    for e in errors:
        print("ERROR", e)
    print("OK" if not errors else "FAILED")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
