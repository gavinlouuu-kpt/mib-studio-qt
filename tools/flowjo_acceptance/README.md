# FlowJo reference and engine qualification

These are independent **qualification harnesses**, not a production worker,
an analysis module demonstration or a claim of FlowJo parity. They do not
write sources, launch acquisition, change Penpot, or invoke hardware.
See the [ledger](../../docs/design/flowjo-analysis/capability-ledger.md) and
[plan](../../docs/exec-plans/active/2026-10-22-flowjo-realignment.md).

## Historical reference

```sh
python3 tools/flowjo_acceptance/verify_reference.py "$FLOWJO_REFERENCE_ROOT"
```

Supply the authorized local `flowjo-windows-10.8.1` research directory.
No local path or vendor dataset is bundled. The script checks exact SHA-256
pins before decoding the known reference (it is **not** a general FCS reader).
It requires the source FCS and both original histogram TSVs. Missing, oversized,
changed or numerically inconsistent input exits 1. Success exits 0.

Observed: 229,585 events, 190,705 selected, zero boundary events and zero
mismatches in each 4,096-bin vector. This does not establish inclusive gate
edges, saved statistic estimators, independent workspace reopening, a new
FlowJo runtime run, FlowJo 11 parity, or MIB product acceptance.

## Real Cytoflow probe

Candidate: Cytoflow 1.3.2 at `02f40704b24ad8dcd32cee69b9fe50e3aeb56c49`,
fcsparser submodule `04efc3dcd2e3a58989f2f4dae3773cf39c727f1d`.
The upstream source declares Python >=3.12 and GPL-2.0-or-later. Its pip metadata
explicitly says it is not a complete installation recipe. The configured
package index did not resolve `cytoflow==1.3.2`; a source build succeeded.
This headless development environment is not the upstream conda environment
or an installer-qualified distribution. Shipping still needs the owner decision.

Reproduce in a disposable directory with Python 3.12, a C++ compiler and `uv`:

```sh
git clone --filter=blob:none --no-checkout https://github.com/cytoflow/cytoflow.git cytoflow-source
git -C cytoflow-source checkout 02f40704b24ad8dcd32cee69b9fe50e3aeb56c49
git -C cytoflow-source submodule update --init fcsparser
uv venv --python 3.12 qualification-env
uv pip install --python qualification-env/bin/python swig==4.5.1
PATH="$PWD/qualification-env/bin:$PATH" uv pip install \
  --python qualification-env/bin/python \
  -c /path/to/mib-studio-qt/env/analysis-worker/runtime-constraints.txt \
  ./cytoflow-source ./cytoflow-source/fcsparser
timeout 60 qualification-env/bin/python \
  /path/to/mib-studio-qt/tools/flowjo_acceptance/qualify_cytoflow.py
```

Use an external timeout (60 seconds above) and one Numba thread. No Python
package or host configuration is installed globally. Constraints record the
observed runtime resolution, not a binary lock or distribution approval;
build-isolation tool versions and platform wheels require release qualification.
Without SWIG the optional Logicle extension may silently fail to build, then
import fails. Without the pinned submodule, fcsparser is missing.

The probe emits all checks plus installed dependency versions and exits **1**
when any required behavior is unmet. The current 8/10 result is intentionally
red: exact string identities, both windings, edges/vertices, missing exclusions,
query/reorder and input immutability pass in these small fixtures. Bare RangeOp
rejects equal endpoints, and inversion of its boolean-style missing placeholder
is not three-valued MIB population logic. These are **adapter obligations**, not
a reason to replace Cytoflow. MIB's existing native scalar/mechanics authority
and declared eligibility/unknown composition must resolve them, with regression
fixtures, after shared contracts/ownership agreement. No general geometry,
concavity, transforms, QC, compensation, embeddings or statistics qualification
is implied by these ten checks.

## Recorded evidence and review

Sanitized results live in `docs/design/flowjo-analysis/qualification-*.json`.
The Cytoflow result records the source commit separately from the package version;
the harness enforces version 1.3.2 but does not attest installed source contents.
Reproduction must use the pinned checkout above. No vendor sources or figures
are committed. Test exit codes must be captured directly, not masked by a later
`cat` command. Failing qualifications must remain visible in acceptance reports.

## Implemented headless analysis-core slice (F-A1)

The production candidate now includes the native C++ population core, a real
Cytoflow polygon adapter, Rust process supervision, and a durable private job
journal. These are not mounted in Studio/Review yet. No source reader or UI is
substituted by this synthetic demo; all remaining capability obligations persist.
See [AnalysisWorker](../../knowledge_map/services/AnalysisWorker.md).

Build/test the native module without camera/Qt/HDF5 dependencies:

```sh
cmake -S tools/flowjo_acceptance -B build/analysis-module -DCMAKE_BUILD_TYPE=Debug
cmake --build build/analysis-module -j2
ctest --test-dir build/analysis-module --output-on-failure
cargo test --locked --manifest-path crates/mib-analysis-worker/Cargo.toml
```

With the pinned Python environment prepared above:

```sh
/path/to/qualification-env/bin/python -m unittest discover -s tests/analysis -p test_worker.py -v
MIB_CYTOFLOW_PYTHON=/path/to/qualification-env/bin/python cargo test --locked \
  --manifest-path crates/mib-analysis-worker/Cargo.toml real_engine -- --ignored
cargo run --locked --manifest-path crates/mib-analysis-worker/Cargo.toml --example module -- \
  run /path/to/qualification-env/bin/python "$PWD/workers/cytoflow/worker.py" \
  "$PWD/data/analysis-demo" < tools/flowjo_acceptance/module-fixture.json
```

The four rows must be `true, unknown, unknown, false`. This combines real
Cytoflow geometry with the same native C++ inclusive E predicate (equal 2..2)
and preserves source/set/row identity beyond 2^53. Scalar values here are
**synthetic**, not an acquired or LUT-computed measurement. Nothing actuates.
The command emits a durable job ID; reopen/query with:

```sh
cargo run --locked --manifest-path crates/mib-analysis-worker/Cargo.toml --example module -- \
  query "$PWD/data/analysis-demo" JOB_ID
```

Repeating the identical run returns its existing immutable terminal result.
Changing inputs under that job ID is refused; use a new job for an explicit
retry/new revision. Crash recovery marks unfinished work Interrupted, not success.
The audit's original bare-engine 8/10 probe remains intentionally red: it records
why the native adapter is necessary. The adapted tests are distinct evidence.

Unix supervisor only so far; Windows execution explicitly refuses. Shipping
qualification, real-recording adapter and full workspace/module demo remain open.

The native harness also builds `analysis.graph`: bounded dependency-ordered
population composition with parent/QC restrictions, cycle/arity/key/revision
refusal and empty/unknown cases. See `PopulationGraph.h`; this is not a full
serialized AnalysisPlan or renderer editor.

Clean pip-build note: the `swig` wheel's Python launcher can lose its module in
pip's isolated build environment. CI puts `swig.DATA/bin` on PATH and sets
`SWIG_LIB` to its pinned `share/swig/4.5.1`, then explicitly imports FastLogicle.
A clean-source isolated wheel build and fresh-wheel native import verified this
fix locally; installation success alone is not proof the optional extension built.

## Native production-host sanitizer qualification

Enable `-DMIB_ANALYSIS_HOST_TESTS=ON` to compile source transforms, plots, comparisons,
report/library persistence and namespace fencing without Qt/HDF5. Requires SQLite3,
nlohmann-json and spdlog development packages. With ASan/UBSan flags, all four
standalone targets pass locally (2026-10-23), including native analysis.host.
On the P5 host select `-Dfmt_DIR=/usr/lib/x86_64-linux-gnu/cmake/fmt` to pair the
system spdlog with system fmt rather than the unrelated /usr/local installation.
The existing analysis CI now enables this host target and all real worker/scheduler
tests, including seeded embedding; that updated remote CI has not yet been run.
