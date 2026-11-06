import json
import os
from pathlib import Path
import resource
import sys

# Prevent numerical-library thread stacks from exhausting the address-space bound.
os.environ["OPENBLAS_NUM_THREADS"] = "1"
os.environ["OMP_NUM_THREADS"] = "1"

# Bound expansion of adversarial CAD, not just upload bytes.
resource.setrlimit(resource.RLIMIT_AS, (3 * 1024**3, 3 * 1024**3))
resource.setrlimit(resource.RLIMIT_CPU, (280, 290))
resource.setrlimit(resource.RLIMIT_FSIZE, (3 * 1024**3, 3 * 1024**3))
from pipeline import inspect, generate

phase, folder = sys.argv[1:]
p = Path(folder)
try:
    meta = json.loads((p / "request.json").read_text(encoding="utf-8"))
    ext = (p / "extension").read_text(encoding="utf-8")
    result = (
        inspect(p / f"input.{ext}", meta["units"], p)
        if phase == "inspect"
        else generate(p / "input.dxf", meta, p)
    )
    (p / "result.json").write_text(
        json.dumps(result, allow_nan=False), encoding="utf-8"
    )
except Exception as e:
    (p / "error.txt").write_text(str(e)[:1000], encoding="utf-8")
    sys.exit(1)
