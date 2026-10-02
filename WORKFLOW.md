---
tracker:
  kind: linear
  api_key: $LINEAR_API_KEY
  project_slug: mib-studio-97e2c43e910d
  active_states:
    - Todo
    - In Progress
    - Rework
  terminal_states:
    - Done
    - Closed
    - Cancelled
    - Canceled
    - Duplicate
workspace:
  root: $SYMPHONY_WORKSPACE_ROOT
hooks:
  after_create: |
    git clone --depth 1 https://github.com/KPT1020/mib-studio-qt.git .
  before_run: |
    if [ ! -d .git ]; then
      git clone --depth 1 https://github.com/KPT1020/mib-studio-qt.git .
    fi
    git status --short
    scripts/doctor.sh --quiet || scripts/bootstrap.sh --public-assets-only
  timeout_ms: 600000
agent:
  max_concurrent_agents: 2
  max_turns: 8
  max_retry_backoff_ms: 300000
codex:
  command: codex app-server
  approval_policy: never
  thread_sandbox: workspace-write
  turn_sandbox_policy:
    type: workspaceWrite
    networkAccess: true
  turn_timeout_ms: 3600000
  read_timeout_ms: 60000
  stall_timeout_ms: 300000
server:
  port: 4402
---

You are working on Linear issue {{ issue.identifier }} for MIB Studio Qt.

Issue title: {{ issue.title }}

Issue body:
{{ issue.description }}

Follow the repository guidance in AGENTS.md before editing files. Start by reading:

- AGENTS.md
- knowledge_map/Agent-Onboarding.md
- docs/golden-principles.md
- knowledge_map/current-state/Recent-Work.md

Implementation rules:

- Keep changes scoped to the issue.
- Do not revert unrelated user or agent changes.
- Use spdlog for application logging; do not add std::cout in app code.
- Keep headers mirrored under include/ when adding or moving C++ APIs.
- Update the matching vault note for every code change, following knowledge_map/Vault-Maintenance.md.
- Add a dated entry to knowledge_map/current-state/Recent-Work.md for non-trivial work.
- Prefer focused backend tests where the touched area already has coverage.

Verification target:

- Run python scripts/check_docs.py after doc or vault edits.
- Run the most relevant CMake/CTest preset available in the workspace when the issue changes C++ behavior.
- If full verification is not possible, explain the exact missing dependency or command failure in the final handoff.

Handoff:

- Summarize files changed, validation commands, and any residual risk.
- Leave the Linear issue in a human-review-ready state rather than marking it done unless the workflow explicitly calls for done.
