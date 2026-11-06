"""Produce reviewable D1 bootstrap SQL; never connects or changes cloud state."""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts/dot_grid"))
from dotgrid.registry import Registry

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--tenant-id", required=True)
p.add_argument("--tenant-name", required=True)
p.add_argument("--admin-email", required=True)
p.add_argument(
    "--registry",
    action="append",
    default=[],
    help="Existing production local registry to reserve before accepting uploads",
)
a = p.parse_args()


def quote(s):
    return "'" + str(s).replace("'", "''") + "'"


bundled = Registry.load(
    str(
        Path(__file__).resolve().parents[3]
        / "resources/defaults/dot_grid/registry.json"
    )
)
known = Registry(bundled.designs)
imports = []
for path in a.registry:
    for design in Registry.load(path).designs:
        old = known.find(design.id)
        if old and old.to_dict() == design.to_dict():
            continue
        known.add(design)
        imports.append(design)
print("BEGIN TRANSACTION;")
print(
    f"INSERT INTO tenants(id,name) VALUES({quote(a.tenant_id)},{quote(a.tenant_name)});"
)
print(
    f"INSERT INTO members(email,tenant_id,role) VALUES({quote(a.admin_email.lower())},{quote(a.tenant_id)},'admin');"
)
for d in imports:
    print(
        "INSERT INTO designs(seed,id,name,extension,units,status,registry_entry) VALUES("
        + ",".join(
            [
                str(d.seed),
                quote(d.id),
                quote(d.name),
                "''",
                "'um'",
                "'reserved'",
                quote(json.dumps(d.to_dict())),
            ]
        )
        + ");"
    )
print("COMMIT;")
