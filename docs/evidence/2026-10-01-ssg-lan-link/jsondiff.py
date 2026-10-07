import json
import sys


def flat(value, prefix=""):
    out = {}
    if isinstance(value, dict):
        for k, v in value.items():
            out.update(flat(v, f"{prefix}.{k}" if prefix else k))
    else:
        out[prefix] = value
    return out


a = flat(json.load(open(sys.argv[1], encoding="utf-8")))
b = flat(json.load(open(sys.argv[2], encoding="utf-8")))
added = sorted(set(b) - set(a))
removed = sorted(set(a) - set(b))
changed = sorted(k for k in set(a) & set(b) if a[k] != b[k])
print(f"keys before: {len(a)}  after: {len(b)}")
print(f"added ({len(added)}):")
for k in added:
    print(f"  + {k} = {json.dumps(b[k])}")
print(f"removed ({len(removed)}):")
for k in removed:
    print(f"  - {k} = {json.dumps(a[k])}")
print(f"changed ({len(changed)}):")
for k in changed:
    print(f"  ~ {k}: {json.dumps(a[k])} -> {json.dumps(b[k])}")
