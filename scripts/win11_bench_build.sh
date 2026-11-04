#!/usr/bin/env bash
# win11_bench_build.sh - build any branch of mib-studio-qt on the Windows bench
# PC (win11-agent) with the real Euresys eGrabber SDK, from P5, in one command.
#
#   scripts/win11_bench_build.sh feat/my-camera-change
#
# What it does: resolves the branch to a commit, pushes it over ssh to a bare
# repo on win11-agent (no GitHub credentials ever leave P5), then runs the
# Windows-side script scripts/win11_bench_build.ps1 detached (conan install,
# CMake configure with the eGrabber SDK, Release build of the Qt GUI
# mib_studio_qt) and waits for it. On success it prints the path of the runnable
# exe; on failure it prints the build log location and the end of the log.
#
# Guide: docs/howto/win11-bench-build.md
set -euo pipefail

HOST="${WIN11_BENCH_HOST:-win11-agent}"
REMOTE_ROOT_WIN='C:\MIBBuild'          # Windows spelling (used in remote commands)
REMOTE_ROOT_GIT='C:/MIBBuild'          # git/scp spelling
REMOTE_PS1_WIN="${REMOTE_ROOT_WIN}\\bin\\win11_bench_build.ps1"
POLL_SECONDS="${WIN11_BENCH_POLL_SECONDS:-30}"
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=15 -o ServerAliveCountMax=4)

usage() {
  cat <<'EOF'
Usage: scripts/win11_bench_build.sh [options] <branch|tag|sha>
       scripts/win11_bench_build.sh --status | --attach [RUN_ID] | --setup | --log [RUN_ID]

Builds the given commit's Release GUI (mib_studio_qt, MIB_HAS_EGRABBER=1) on
win11-agent and prints the exe path. Uncommitted changes are not included:
the commit is what gets built.

Options:
  --clean          delete the CMake build tree first (the Conan cache stays)
  --target NAME    CMake target to build (default mib_studio_qt)
  --all            build every target, tests included (target "all")
  --jobs N         parallel compile jobs on the bench PC (default 12 of 16)
  --no-smoke       skip the short launch-and-close check after the build
  --no-wait        start the build and return; follow it with --attach
  --attach [ID]    wait for a running or finished build (default: the latest)
  --status         one-shot status of the latest build
  --log [ID]       print the last 200 log lines of a build (default: the latest)
  --setup          run only the idempotent one-time setup on win11-agent
  -h, --help       this text

Environment: WIN11_BENCH_HOST (default win11-agent), WIN11_BENCH_POLL_SECONDS (30).
Exit status: 0 built, 1 build failed, 2 usage/ssh/push problem, 3 another build is running.
EOF
}

die() { echo "win11_bench_build: $*" >&2; exit 2; }

# Run a PowerShell command line on the bench PC (its default ssh shell is
# PowerShell). Strips the CRs Windows adds.
rps() { ssh "${SSH_OPTS[@]}" "$HOST" "$1" | tr -d '\r'; }

ps1() {  # ps1 <args...>: run the shipped script with Bypass execution policy
  rps "powershell -NoProfile -ExecutionPolicy Bypass -File ${REMOTE_PS1_WIN} $*"
}

ship_script() {
  local here
  here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  ssh "${SSH_OPTS[@]}" "$HOST" "New-Item -ItemType Directory -Force ${REMOTE_ROOT_WIN}\\bin | Out-Null" >/dev/null
  scp -q "${SSH_OPTS[@]}" "${here}/win11_bench_build.ps1" "${HOST}:${REMOTE_ROOT_GIT}/bin/win11_bench_build.ps1" \
    || die "could not copy win11_bench_build.ps1 to ${HOST}"
}

kv() { sed -n "s/^$1=//p" <<<"$2" | head -n1; }

fmt_secs() { local s=$1; printf '%dm%02ds' $((s / 60)) $((s % 60)); }

print_failure() {
  local status="$1" run log
  run="$(kv RUN "$status")"; log="$(kv LOG "$status")"
  echo
  echo "BUILD FAILED  branch=$(kv BRANCH "$status") sha=$(kv SHA "$status" | cut -c1-9) step=$(kv STEP "$status")"
  echo "  error: $(kv ERROR "$status")"
  echo "  build log on ${HOST}: ${log}"
  echo "  last log lines:"
  ps1 -Action Status -RunId "$run" -Tail 40 | sed -n '/^--- log tail ---$/,$p' | tail -n +2 | sed 's/^/    | /'
  echo "  more:  scripts/win11_bench_build.sh --log ${run}   (or: ssh ${HOST} \"Get-Content ${log} -Tail 200\")"
}

print_success() {
  local status="$1"
  echo
  echo "BUILD OK  branch=$(kv BRANCH "$status") sha=$(kv SHA "$status" | cut -c1-9) in $(fmt_secs "$(kv ELAPSED "$status")")"
  echo "  steps: $(kv STEPS "$status")"
  echo "  smoke: $(kv SMOKE "$status")"
  echo "  build log on ${HOST}: $(kv LOG "$status")"
  echo "EXE: $(kv EXE "$status")"
}

# Poll until the run leaves "running". Tolerates short ssh outages (the build
# runs detached on the bench PC and is not affected by them).
wait_for() {
  local run="$1" now status state step errors=0 start_ts
  start_ts=$(date +%s)
  while :; do
    if status="$(ps1 -Action Status -RunId "$run" -Tail 1 2>/dev/null)" && [[ -n "$status" ]]; then
      errors=0
      state="$(kv STATE "$status")"
      step="$(kv STEP "$status")"
      case "$state" in
        ok)     print_success "$status"; return 0 ;;
        failed) print_failure "$status"; return 1 ;;
        none)   die "run ${run} not found on ${HOST}" ;;
      esac
      now=$(date +%s)
      printf '[%s] %s  %s\n' "$(fmt_secs $((now - start_ts)))" "${step:-?}" \
        "$(sed -n '/^--- log tail ---$/,$p' <<<"$status" | tail -n +2 | tail -n1 | cut -c1-110)"
    else
      errors=$((errors + 1))
      echo "[ssh to ${HOST} failed (${errors}); the build keeps running there, retrying]" >&2
      if ((errors >= 20)); then
        die "lost contact with ${HOST}; later: scripts/win11_bench_build.sh --attach ${run}"
      fi
    fi
    sleep "$POLL_SECONDS"
  done
}

# ---- argument parsing -------------------------------------------------------
CLEAN=0 TARGET=mib_studio_qt JOBS=12 SMOKE=1 WAIT=1 MODE=build REF="" RUN_ARG=""
while (($#)); do
  case "$1" in
    --clean) CLEAN=1 ;;
    --target) TARGET="${2:?--target needs a name}"; shift ;;
    --all) TARGET=all ;;
    --jobs) JOBS="${2:?--jobs needs a number}"; shift ;;
    --no-smoke) SMOKE=0 ;;
    --no-wait) WAIT=0 ;;
    --status) MODE=status ;;
    --attach) MODE=attach; if [[ "${2:-}" =~ ^[0-9]{8}-[0-9]{6}-[0-9a-f]{7}$ ]]; then RUN_ARG="$2"; shift; fi ;;
    --log) MODE=log; if [[ "${2:-}" =~ ^[0-9]{8}-[0-9]{6}-[0-9a-f]{7}$ ]]; then RUN_ARG="$2"; shift; fi ;;
    --setup) MODE=setup ;;
    -h|--help) usage; exit 0 ;;
    -*) usage >&2; die "unknown option $1" ;;
    *) [[ -z "$REF" ]] || die "only one branch can be built at a time"; REF="$1" ;;
  esac
  shift
done
[[ "$JOBS" =~ ^[0-9]+$ ]] || die "--jobs must be a number"
[[ "$TARGET" =~ ^[A-Za-z0-9_.+-]+$ ]] || die "unsafe --target"

ssh "${SSH_OPTS[@]}" "$HOST" "hostname" >/dev/null 2>&1 \
  || die "cannot ssh to ${HOST} (BatchMode); is the bench PC up and the key accepted? Try: ssh ${HOST} hostname"

ship_script

case "$MODE" in
  setup)  ps1 -Action Setup; exit 0 ;;
  status) ps1 -Action Status -Tail 5; exit 0 ;;
  log)    ps1 -Action Status ${RUN_ARG:+-RunId "$RUN_ARG"} -Tail 200; exit 0 ;;
  attach)
    if [[ -z "$RUN_ARG" ]]; then
      RUN_ARG="$(kv RUN "$(ps1 -Action Status)")"
      [[ -n "$RUN_ARG" ]] || die "no build has been run on ${HOST} yet"
    fi
    wait_for "$RUN_ARG"; exit $?
    ;;
esac

# ---- build ------------------------------------------------------------------
[[ -n "$REF" ]] || { usage >&2; die "give the branch (or tag/sha) to build"; }
git rev-parse --git-dir >/dev/null 2>&1 || die "run this inside a mib-studio-qt checkout"

SHA=""
for cand in "$REF" "origin/$REF"; do
  if SHA="$(git rev-parse --verify --quiet "${cand}^{commit}")"; then break; fi
  SHA=""
done
if [[ -z "$SHA" ]]; then
  echo "'${REF}' is not known locally; fetching it from origin"
  GIT_TERMINAL_PROMPT=0 git fetch --quiet origin "$REF" 2>/dev/null \
    || die "cannot resolve '${REF}'. Fetch it first (GitHub needs credentials: vault-agent-run github -- git fetch origin ${REF})"
  SHA="$(git rev-parse --verify "FETCH_HEAD^{commit}")"
fi
BRANCH="${REF#origin/}"
BRANCH="${BRANCH#refs/heads/}"
if [[ ! "$BRANCH" =~ ^[A-Za-z0-9._/+@-]+$ ]]; then
  BRANCH="sha-${SHA:0:12}"   # a sha or exotic ref name: use a safe label
fi
[[ "$SHA" =~ ^[0-9a-f]{40}$ ]] || SHA="$(git rev-parse "${SHA}")"
SUBJECT="$(git log -1 --format=%s "$SHA" | cut -c1-80)"

echo "win11-agent bench build"
echo "  branch : ${BRANCH}"
echo "  commit : ${SHA:0:12}  ${SUBJECT}"
if ((CLEAN)); then echo "  target : ${TARGET}  (clean build tree)"; else echo "  target : ${TARGET}"; fi
if [[ -n "$(git status --porcelain --untracked-files=no 2>/dev/null)" ]] && [[ "$(git rev-parse HEAD)" == "$SHA" ]]; then
  echo "  note   : the working tree has uncommitted changes; they are NOT built (only the commit is)"
fi

echo "== setup check on ${HOST} (fast when already done)"
ps1 -Action Setup

echo "== pushing ${SHA:0:12} to ${HOST}:${REMOTE_ROOT_GIT}/repo.git"
git push --quiet --force "${HOST}:${REMOTE_ROOT_GIT}/repo.git" "${SHA}:refs/heads/bench/${BRANCH}" \
  || die "git push to ${HOST} failed"
git push --quiet "${HOST}:${REMOTE_ROOT_GIT}/repo.git" "refs/tags/*:refs/tags/*" 2>/dev/null || true

echo "== starting the detached build"
ARGS=(-Action Launch -Sha "$SHA" -Branch "'${BRANCH}'" -Target "$TARGET" -Jobs "$JOBS")
((CLEAN)) && ARGS+=(-Clean)
((SMOKE)) || ARGS+=(-SkipSmoke)
set +e
LAUNCH_OUT="$(ps1 "${ARGS[@]}")"
LAUNCH_RC=$?
set -e
BUSY="$(kv BUSY "$LAUNCH_OUT")"
if [[ -n "$BUSY" ]]; then
  echo "another build (${BUSY}) is already running on ${HOST}. Wait for it: scripts/win11_bench_build.sh --attach ${BUSY}" >&2
  exit 3
fi
RUN="$(kv RUN_ID "$LAUNCH_OUT")"
if ((LAUNCH_RC != 0)) || [[ -z "$RUN" ]]; then
  echo "$LAUNCH_OUT" >&2
  die "could not start the build on ${HOST}"
fi
echo "run id ${RUN}; log ${REMOTE_ROOT_WIN}\\logs\\${RUN}.log"

if ((WAIT == 0)); then
  echo "not waiting; follow it with: scripts/win11_bench_build.sh --attach ${RUN}"
  exit 0
fi
wait_for "$RUN"
