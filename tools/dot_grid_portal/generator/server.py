"""Private container interface; no public ingress or internet access."""

import json
import os
from pathlib import Path
import subprocess
import sys
from fastapi import FastAPI, Request, HTTPException
from fastapi.responses import FileResponse

app = FastAPI()
WORK = Path(os.environ.get("CAD_WORKDIR", "/tmp/cad-job"))
WORK.mkdir(parents=True, exist_ok=True)
MAX = 10 * 1024 * 1024
ALLOWED = {
    "normalized.dxf",
    "design_with_dotgrid.dxf",
    "dotgrid_layer.gds",
    "dotgrid_layer.oas",
    "design_with_dotgrid.dwg",
    "mib-map.zip",
}


@app.get("/health")
def health():
    return {"ok": True}


@app.put("/input")
async def upload(req: Request):
    ext = req.headers.get("X-Extension")
    if ext not in ("dxf", "dwg"):
        raise HTTPException(400, "DXF or DWG required")
    size = 0
    with (WORK / f"input.{ext}").open("wb") as f:
        async for chunk in req.stream():
            size += len(chunk)
            if size > MAX:
                raise HTTPException(413, "Input too large")
            f.write(chunk)
    (WORK / "extension").write_text(ext, encoding="utf-8")
    return {"bytes": size}


@app.post("/{phase}")
async def process(phase: str, req: Request):
    if phase not in ("inspect", "generate"):
        raise HTTPException(404)
    chunks = []
    size = 0
    async for chunk in req.stream():
        size += len(chunk)
        if size > 16 * 1024 * 1024:
            raise HTTPException(413, "Metadata too large")
        chunks.append(chunk)
    try:
        meta = json.loads(b"".join(chunks))
    except ValueError:
        raise HTTPException(400, "Invalid metadata")
    (WORK / "request.json").write_text(json.dumps(meta), encoding="utf-8")
    # Blocking work is isolated in a bounded subprocess. Files persist only for this job.
    import asyncio

    try:
        result = await asyncio.to_thread(
            subprocess.run,
            [
                sys.executable,
                str(Path(__file__).with_name("run_job.py")),
                phase,
                str(WORK),
            ],
            capture_output=True,
            timeout=300,
        )
    except subprocess.TimeoutExpired:
        raise HTTPException(
            422, "CAD job exceeded five minutes; simplify or split the design"
        )
    if result.returncode:
        error = (
            (WORK / "error.txt").read_text(encoding="utf-8")
            if (WORK / "error.txt").exists()
            else "CAD worker failed"
        )
        raise HTTPException(422, error[:1000])
    return json.loads((WORK / "result.json").read_text(encoding="utf-8"))


@app.get("/files/{name}")
def file(name: str):
    if name not in ALLOWED or not (WORK / name).is_file():
        raise HTTPException(404)
    return FileResponse(WORK / name)
