# View calibration measurement history

Open **Prepare → Calibration → Calibration measurement history**.

- Choose Diameter CV, Valid objects, Median bead diameter, Proposed pixel → µm,
  Median focus or Event-rate CV.
- The chart plots recorded measurements by the time they were derived, not
  the later time they were applied. The table retains unavailable values and
  records with missing dates.
- Cross marks show the limits stored with each record. Open markers mean its
  recorded gate failed. A historical Pass is not a current readiness verdict.
- Select a point (click, Enter or Space) or table row to open that record's
  versions in the existing history panel.
- **All records** displays separate measurements, not a compatible comparison.
  Filter by recorded setup and method revision to narrow the history.
  Incomplete identities are not assumed compatible with one another.

Nothing in this view applies calibration, edits limits or modifies recordings.
Missing focus or an event-rate measurement from a run that was too short stays
unavailable. Changing the selected metric never recalculates old records.

Editable trend policies and persistent reversible exclusions are still pending;
the view does not pretend a display filter is a saved QC exclusion.
