import json
from collections import Counter

d = json.load(open('agent/conversation.json', encoding='utf-8'))
ei = d['evidence_index']
dims = Counter()
tiers = Counter()
total = 0.0
for e in ei:
    dims[e['dimension']] += 1
    tiers[e['tier']] += 1
    total += float(e['tier'])
print('instances: %d, claimed tier sum: %.1f' % (len(ei), total))
print('by dimension:')
for k, v in dims.most_common():
    print('  %-20s %d' % (k, v))
print('by tier:')
for k, v in sorted(tiers.items()):
    print('  %-4s %d' % (k, v))
print('inputs: %d prompts, %d conversations, %d experiments' %
      (len(d['user_prompts']), len(d['conversations']), len(d['experiments'])))
missing = [i for i, e in enumerate(ei) if e.get('result_evidence') is None]
print('instances without result_evidence (must be tier 0.5):', missing)
for i in missing:
    print('   tier', ei[i]['tier'])
