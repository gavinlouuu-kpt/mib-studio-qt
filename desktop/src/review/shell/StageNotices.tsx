// The Review app's stages that have no view of their own yet (#717). Each
// says plainly what is not available and where today's feature lives; none
// shows a number it does not have.

import { Button, Card, FactList, Notice, Toolbar } from "@mib/ui";

/** R2 Overview: per-run statistics come from the analysis worker (#523,
 * step B2). Nothing is computed in the shell, so nothing is shown. */
export function OverviewNotice() {
  return (
    <div className="rv-view" data-testid="review-overview">
      <Toolbar title="Overview">{null}</Toolbar>
      <div className="rv-body">
        <Notice tone="unknown" title="Overview · not available yet">
          Per-run counts, statistics and small multiples come from the analysis worker (#523), which this build does not include.
          The shell computes no statistics, so this view shows none.
        </Notice>
      </div>
    </div>
  );
}

/** R6 Export: today's exports stay on Inspect's toolbar until the export
 * view (step B4) moves them here, unchanged. */
export function ExportNotice({ fileOpen, onInspect }: { fileOpen: boolean; onInspect: () => void }) {
  return (
    <div className="rv-view" data-testid="review-export">
      <Toolbar title="Export">
        <Button variant="primary" onClick={onInspect}>
          Go to Inspect
        </Button>
      </Toolbar>
      <div className="rv-body">
        <Notice tone="info" title="Export · on Inspect's toolbar for now">
          The export view arrives in a later step. Every export works today from Inspect, unchanged{fileOpen ? "" : " (open a run first)"}.
        </Notice>
        <Card title="Where each export is" subtitle="Inspect toolbar; More… holds the batch exports and Regenerate masks">
          <FactList
            facts={[
              { label: "Export Metrics to CSV…", value: "Inspect toolbar · the open run's metrics" },
              { label: "Export All…", value: "Inspect toolbar · metrics, frame images and charts" },
              { label: "Export Charts…", value: "Inspect · More…" },
              { label: "Batch Metrics… / Batch Export All…", value: "Inspect · More… · several files at once" },
              { label: "Regenerate masks…", value: "Inspect · More… · writes a new file, never replaces one" },
            ]}
          />
        </Card>
      </div>
    </div>
  );
}
