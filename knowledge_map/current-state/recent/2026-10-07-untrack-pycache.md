## 2026-10-07 — Stop tracking scripts/__pycache__ bytecode

Three `scripts/__pycache__/*.cpython-313.pyc` files were committed before `**/__pycache__/` entered `.gitignore`. Python rewrote them whenever scripts ran, which dirtied worktrees and blocked `git merge origin/develop` during branch re-syncs. They are now untracked; the ignore rule covers them.
