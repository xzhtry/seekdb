import json
from pathlib import Path

doc = json.loads(Path('agent/conversation.json').read_text(encoding='utf-8'))
total = len(Path('agent/conversation.json').read_bytes())
print('total bytes: %d (%.1f KiB)' % (total, total / 1024.0))
for key in ('user_prompts', 'conversations', 'experiments', 'evidence_index', 'limitations'):
    v = doc.get(key, [])
    size = len(json.dumps(v, ensure_ascii=False, indent=2).encode('utf-8'))
    print('  %-16s n=%-3d %7d bytes (%.1f KiB)' % (key, len(v), size, size / 1024.0))
for key in ('retrospective', 'material_scope', 'schema', 'task', 'team', 'date', 'language', 'synthetic'):
    v = doc.get(key)
    size = len(json.dumps(v, ensure_ascii=False, indent=2).encode('utf-8'))
    print('  %-16s %s %7d bytes' % (key, '', size))
# biggest experiments and instances
print()
for e in sorted(doc['experiments'], key=lambda x: -len(json.dumps(x, ensure_ascii=False)))[:8]:
    size = len(json.dumps(e, ensure_ascii=False).encode('utf-8'))
    print('  exp %-6s %6d B  %s' % (e.get('id'), size, str(e.get('title'))[:44]))
for i in sorted(doc['evidence_index'], key=lambda x: -len(json.dumps(x, ensure_ascii=False)))[:8]:
    size = len(json.dumps(i, ensure_ascii=False).encode('utf-8'))
    print('  inst #%-4d %6d B  tier %s  %s' % (i, size, i.get('tier'), i.get('dimension')))
