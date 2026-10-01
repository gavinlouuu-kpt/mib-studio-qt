"""Set the rf_generator block of a MIB Studio config.json (same edit the
Config tab makes: only this section is touched, file layout as Qt writes it).

Usage: python set_rf_generator.py <config.json> <enabled true|false> <resource>
"""
import json
import sys

path, enabled, resource = sys.argv[1], sys.argv[2].lower() == "true", sys.argv[3]
if resource == "EMPTY":  # PowerShell 5.1 drops empty-string arguments
    resource = ""
with open(path, encoding="utf-8") as f:
    doc = json.load(f)
block = doc.get("rf_generator", {})
block.update({"enabled": enabled, "transport": "lan", "resource": resource})
block.setdefault("timeout_ms", 1000)
doc["rf_generator"] = block
with open(path, "w", encoding="utf-8", newline="\n") as f:
    json.dump(doc, f, indent=4, sort_keys=True, ensure_ascii=False)
    f.write("\n")
print(json.dumps(doc["rf_generator"]))
