## 2026-10-08 — Host recovery as the backstop when the PL self-heal gives up

At the first bundle slot (develop 03d65eb1 + pl-results10) the PL self-heal gave up after its 8 tries
on 4 of 10 Run to Align flips, and Studio, which sent no host reset when RXH1 is present, showed the
"Align preview not locking" error. `awaitAlignLock` now gives the PL block the first go, and when it
reports gave-up (or does not finish in 6 s) the existing host recovery (up to eight 100 ms receiver
resets) runs. The PL failure stays visible: `mode.align_lock.pl_gave_ups`, `last_pl_gave_up_ms` (stream
start to gave-up), the log line "PL receiver self-heal gave up ... host recovery takes over" and
"pl_gave_up -> host recovered", and `plGaveUp` in the flip script's columns. The operator error names
both the PL and the host attempts. The Preflight page no longer lists the "Autofocus / nanopositioner"
row on the PZ7035; check rows carry `data-check-id`, and the acceptance script matches the accepted
RAM-storage warning and the PL identity on the check id instead of on wording. See [[Desktop-Shell]].
