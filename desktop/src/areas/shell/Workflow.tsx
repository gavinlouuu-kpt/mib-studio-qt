// Shell (epic #862): the workflow stage tabs and the "Next" bar. Moved from App.tsx without change (#869 wave 0).
import type { StageTab } from "../../workflow";
import { useStudio } from "../../studio/StudioContext";
import { CameraActions } from "../connect/CameraActions";
import { STAGE_LOCK_REASON, isRunLockedTab } from "../monitoring/runStatus";

export function StageTabs() {
  const { tab, setTab, stageByTab, runLocked } = useStudio();
  return (
    <div className="tabs-header">
      <div className="tabbar" role="tablist" aria-label="Workflow stages">
        {(["connect", "overview", "experiment", "review"] as StageTab[]).map((t) => {
          const stage = stageByTab[t];
          // #585 run-lock (epic #866 MON-013): Overview and Review are shut for the whole run, as in the Qt app.
          const locked = runLocked && isRunLockedTab(t);
          return (
            <button
              key={t}
              role="tab"
              aria-selected={tab === t}
              aria-label={`${stage.title}: ${stage.statusLabel}`}
              className={`stage-tab${tab === t ? " active" : ""}`}
              title={locked ? STAGE_LOCK_REASON : stage.blocking.length ? stage.blocking.join("; ") : stage.summary}
              disabled={locked}
              onClick={() => setTab(t)}
            >
              <span className={`stage-dot ${stage.status}`} aria-hidden="true" />
              <span className="stage-tab-text">
                <span className="stage-tab-name">{stage.title}</span>
                <span className="stage-tab-status">{stage.statusLabel}</span>
              </span>
            </button>
          );
        })}
      </div>
      <div className="spacer" />
      <CameraActions />
    </div>
  );
}

export function WorkflowNext() {
  const { ready, workflow, currentStage, doRecommended, recDisabled } = useStudio();
  return (
    <>
      {ready && (
        <div className="workflow-next" role="status" aria-live="polite">
          {workflow.recommended ? (
            <>
              <span className="workflow-next-text">
                <strong>Next:</strong> {currentStage.title} — {currentStage.summary}
              </span>
              <button
                className="workflow-next-action"
                onClick={doRecommended}
                disabled={recDisabled}
                title={
                  recDisabled
                    ? "Setup is locked while an experiment is active"
                    : undefined
                }
              >
                {workflow.recommended.label}
              </button>
            </>
          ) : (
            <span className="workflow-next-text">
              <strong>Workflow complete</strong> — all stages ready and data reviewed.
            </span>
          )}
        </div>
      )}
    </>
  );
}
