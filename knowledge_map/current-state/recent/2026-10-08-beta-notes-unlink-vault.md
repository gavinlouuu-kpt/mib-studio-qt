## 2026-10-08 — Beta release notes drop vault wikilinks

`scripts/release_notes.py --beta` builds the uncurated beta notes from `recent/` fragments. Those notes reach operators through the updater and Help ▸ What's New, but the fragments' vault wikilinks (double-bracketed note paths) were shown verbatim. They are now rendered as plain text: the note name, or the link's label when it has one. On develop's current fragments that removes 59 links; the fragments themselves are unchanged. Curated stable notes never contained wikilinks.
