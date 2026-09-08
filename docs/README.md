# Documentation

This folder hosts living documentation as we build functionality. Keep content concise and actionable; link to source where applicable.

## Structure
- `manual/` — user-facing operator manual with generated screenshots ([index](manual/README.md))
- `architecture/` — system and component overviews
- `integration/` — external integrations (e.g., Euresys EGrabber)
- `decisions/` — Architecture Decision Records ([index](decisions/README.md))
- `howto/` — task-oriented guides and runbooks
- `exec-plans/` — execution plans and tech debt ([conventions](exec-plans/README.md))
- [golden-principles.md](golden-principles.md) — mechanical rules that keep the repo agent-legible

## Conventions
- Use clear headings and short sections.
- Prefer links to code over duplicating snippets.
- When adding a guide, consider if a brief ADR is needed in `decisions/`.

## Quick Links
- User manual (operators) — see [manual/README.md](manual/README.md);
  screenshots regenerate via `screenshot_tour`
  (`python3 scripts/check_screenshots.py` keeps pages and images in sync)
- Integration: EGrabber — see `integration/egrabber.md`
- Integration: Ultra96 FPGA image pipeline — see
  [integration/ultra96-fpga-image-pipeline.md](integration/ultra96-fpga-image-pipeline.md)
- Tasks/issues live in `knowledge_map/task/`
- Cloudflare R2 app updates and profile catalogs — see
  [howto/auto-update-r2.md](howto/auto-update-r2.md)
- Portable processing contract + sync manifests (config/LUT/engine for
  non-Qt consumers like Biowork) — see
  [gold_standard_metrics.md](gold_standard_metrics.md) and
  [portable-processing-sync.md](portable-processing-sync.md)
- Post-processing tools (export, reanalyse) — see [howto/tools.md](howto/tools.md)
- Branching model & release pipeline (develop → main) — see
  [howto/branching-and-releases.md](howto/branching-and-releases.md)
- Known debt — see [exec-plans/tech-debt-tracker.md](exec-plans/tech-debt-tracker.md)
- Agent map — see [../AGENTS.md](../AGENTS.md)

Run `python3 scripts/check_docs.py` after editing any markdown; CI enforces it.
