# Handover: generate the YOFO Review update key on the local server

Status: active

Owner of the next step: whoever operates the team's local (on-premises) server.
Parent plan: [2026-10-01-standalone-review-app.md](2026-10-01-standalone-review-app.md) (PR 6).

## Goal

YOFO Review's auto-updater is built and merged on branch
`plan/standalone-review-app`, but it is **off** until the app carries a
minisign public key. The key pair is generated **on the local server**
(decision 2026-10-04): the private key and its password never enter the
repository, a chat or a cloud session; only the public key is committed.
When this handover is done, release builds sign update bundles and
`v*` tags publish them to `https://updates.yofo.bio/review-<channel>/`.

## What already exists (do not rebuild)

| Piece | Where |
|---|---|
| Updater commands (check / install, minisign + SHA-256 pin, fail closed) | `desktop/src-tauri/src/review_update.rs` |
| Plugin registered only when a public key is configured | `desktop/src-tauri/src/lib.rs` (`run()` setup) |
| UI: Help ▸ Check for updates…, channel in Preferences | `desktop/src/review/updates.tsx`, `ReviewApp.tsx` |
| "Is the updater configured?" check | `scripts/release/review-updater-enabled.py` |
| Signed artifacts in release builds (key + secret gated) | `.github/workflows/review-bundles.yml` |
| Publish to R2 (Tauri `latest.json` + `sha256`) | `scripts/release/publish-review-update.py`, `review-release.yml` job `publish-updates` |
| Full background | `docs/howto/auto-update-r2.md` → "YOFO Review (Tauri updater)" |

## Acceptance criteria

- [ ] Key pair generated on the local server, private key password-protected
      (verified: signing fails with an empty or wrong password).
- [ ] Private key + password stored in the team password manager, plus an
      offline backup; the server copy is mode `600`.
- [ ] Repository secrets `TAURI_SIGNING_PRIVATE_KEY` and
      `TAURI_SIGNING_PRIVATE_KEY_PASSWORD` set on `gavinlouuu-kpt/mib-studio-qt`.
- [ ] Public key committed to `desktop/src-tauri/tauri.review.conf.json`
      (`plugins.updater`), `scripts/release/review-updater-enabled.py` exits 0,
      pushed to `plan/standalone-review-app`, YOFO Review CI green.
- [ ] This plan set to `Status: completed`, moved to `completed/`, parent plan
      PR 6 "Remaining" line closed.

## Decision log

- 2026-10-04: generate on the local server, not in a cloud agent session
  (owner's choice) — the private key exists only there, in the password
  manager / offline backup, and as a GitHub Actions secret.
- 2026-10-04: verified Tauri CLI 2.11 behaviour that the procedure depends on:
  `tauri signer generate` **ignores** `TAURI_SIGNING_PRIVATE_KEY_PASSWORD`, and
  `--ci` without `-p` silently writes an **unencrypted** key — so the password
  is passed with `-p`. `tauri signer sign` and the bundler read the password
  from `TAURI_SIGNING_PRIVATE_KEY_PASSWORD`. Without `umask 077` the key files
  are written world-readable.

## Procedure

Prerequisites on the server: `git`, Node 22 + npm, `openssl`, Python 3, and
the GitHub CLI `gh` logged in as a user who can manage Actions secrets on the
repository (`gh auth status`). If `gh` is not available, the secrets can be
pasted in the web UI (Settings ▸ Secrets and variables ▸ Actions) instead.

```bash
# 0. Repository
git clone https://github.com/gavinlouuu-kpt/mib-studio-qt.git && cd mib-studio-qt   # or: git fetch && cd
git checkout plan/standalone-review-app && git pull --ff-only
(cd desktop && npm install)

# 1. Key pair, password-protected, private files only (outside the repo)
umask 077
mkdir -p ~/.tauri
test -e ~/.tauri/yofo-review.key && { echo "key already exists — stop and ask"; exit 1; }
openssl rand -base64 32 > ~/.tauri/yofo-review.key.password
(cd desktop && npx tauri signer generate --ci -w ~/.tauri/yofo-review.key \
    -p "$(cat ~/.tauri/yofo-review.key.password)")
ls -l ~/.tauri/                 # yofo-review.key, .key.pub, .key.password — all -rw-------

# 2. Prove the key works and is protected
echo probe > /tmp/yofo-probe.bin
TAURI_SIGNING_PRIVATE_KEY_PASSWORD="$(cat ~/.tauri/yofo-review.key.password)" \
  npx --prefix desktop tauri signer sign -f ~/.tauri/yofo-review.key /tmp/yofo-probe.bin
test -s /tmp/yofo-probe.bin.sig && echo "signing works"
TAURI_SIGNING_PRIVATE_KEY_PASSWORD="" \
  npx --prefix desktop tauri signer sign -f ~/.tauri/yofo-review.key /tmp/yofo-probe.bin \
  && echo "PROBLEM: key is not password-protected" || echo "empty password refused (good)"
rm -f /tmp/yofo-probe.bin /tmp/yofo-probe.bin.sig

# 3. GitHub Actions secrets (values come from files; nothing is echoed)
gh secret set TAURI_SIGNING_PRIVATE_KEY --repo gavinlouuu-kpt/mib-studio-qt < ~/.tauri/yofo-review.key
gh secret set TAURI_SIGNING_PRIVATE_KEY_PASSWORD --repo gavinlouuu-kpt/mib-studio-qt < ~/.tauri/yofo-review.key.password
gh secret list --repo gavinlouuu-kpt/mib-studio-qt | grep TAURI_SIGNING

# 4. Public key into the app config (the only file that changes)
python3 - <<'EOF'
import json, pathlib
conf = pathlib.Path("desktop/src-tauri/tauri.review.conf.json")
pub = (pathlib.Path.home() / ".tauri/yofo-review.key.pub").read_text().strip()
data = json.loads(conf.read_text())
data.setdefault("plugins", {})["updater"] = {
    "pubkey": pub,
    "endpoints": ["https://updates.yofo.bio/review-stable/latest.json"],
}
conf.write_text(json.dumps(data, indent=2) + "\n")
EOF
python3 scripts/release/review-updater-enabled.py      # must print "updater: configured", exit 0
python3 scripts/check_docs.py
git status --short                                     # ONLY tauri.review.conf.json (+ this plan)
git diff                                               # contains the public key, nothing secret

# 5. Close this plan, commit, push
#    - set "Status: completed" above, tick the criteria, add a decision-log line
#      with the date and the public key's key ID:
#        base64 -d ~/.tauri/yofo-review.key.pub | head -1   # "... minisign public key: <ID>"
#    - git mv this file to docs/exec-plans/completed/
#    - in the parent plan, replace PR 6's "Remaining: the user-generated public
#      key …" with "public key committed <date>"
git add desktop/src-tauri/tauri.review.conf.json docs/exec-plans
git commit -m "feat(review): YOFO Review update public key"
git push origin plan/standalone-review-app

# 6. Afterwards: move the key material off the plain filesystem
#    Put ~/.tauri/yofo-review.key and its password into the team password
#    manager and an offline backup, then shred the server's password file:
#    shred -u ~/.tauri/yofo-review.key.password
```

Then confirm on GitHub that the "YOFO Review CI" run for the push is green
(Linux, macOS DMG, Windows NSIS). Push builds do not sign; the first signed
updates appear on the next `v*` tag (`review-release.yml` → `publish-updates`),
and need the R2 secrets that `publish-update.py` already uses
(`R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY`, `MIB_STUDIO_R2_ENDPOINT`).

**Losing the private key** strands every installed copy: they can only verify
updates signed by this key. Keep the backup. Rotating the key later means
shipping a release signed with the old key that contains the new public key.

## Prompt for an agent on the local server

Paste the block below into Claude Code (or another coding agent) running on
the local server, from a checkout of the repository.

```text
You are on the team's local server, in a checkout of
github.com/gavinlouuu-kpt/mib-studio-qt. Your task: generate YOFO Review's
auto-update signing key here and wire up its public half, following
docs/exec-plans/active/2026-10-04-yofo-review-update-key-handover.md exactly
(read it first; it is the source of truth, including the Tauri CLI quirks in
its decision log).

Hard rules:
- The private key and its password must never be printed, logged, pasted into
  this conversation, committed, or copied anywhere except: ~/.tauri/ (mode
  600) and the two GitHub Actions secrets via `gh secret set ... < file`.
  Never `cat` the private key or the password file; read them only through
  shell redirection or $(cat ...) inside the commands the plan gives.
- If ~/.tauri/yofo-review.key already exists, stop and ask me — do not
  overwrite (-f) an existing key.
- Generate with `-p` (the plan explains why); use `umask 077`.
- The only repository change is plugins.updater in
  desktop/src-tauri/tauri.review.conf.json (public key + stable endpoint),
  plus closing the handover plan. Check `git status` and `git diff` before
  committing; if anything else appears, stop and ask.
- If `gh` is missing or not authorised to set secrets, do steps 1, 2 and 4,
  then tell me which two secrets to add in the GitHub web UI and from which
  files — without showing their contents.

Steps: 0 checkout plan/standalone-review-app and npm install in desktop/;
1 generate the password-protected key pair; 2 prove signing works with the
password and fails with an empty one; 3 set the two secrets; 4 add the
public key to the config and run scripts/release/review-updater-enabled.py
and scripts/check_docs.py; 5 close the plan as it describes, commit
"feat(review): YOFO Review update public key", push; 6 remind me to move the
key + password into the password manager and an offline backup, and to shred
the password file afterwards.

Report back: the public key (it is public), its key ID, the commit SHA, the
secret names set (names only), and the YOFO Review CI result for the push.
```
