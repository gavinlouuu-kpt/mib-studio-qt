// Shell (epic #862): the workflow stage tabs and the "Next" bar. Moved from App.tsx without change (#869 wave 0).
import type { StageTab } from "../../workflow";
import { useStudio } from "../../studio/StudioContext";
import { CameraActions } from "../connect/CameraActions";
import { OVERVIEW_DISABLED_TITLE } from "../connect/cameraStage";

export function StageTabs() {
  const { tab, setTab, stageByTab, expActive } = useStudio();
  return (
    <div className="tabs-header">
      <div className="tabbar" role="tablist" aria-label="Workflow stages">
        {(["connect", "overview", "experiment", "review"] as StageTab[]).map((t) => {
          const stage = stageByTab[t];
          // Qt MainWindow::onTabChanged: Overview is disabled while an experiment is active (#863, CONNECT-063).
          const locked = t === "overview" && expActive;
          return (
            <button
              key={t}
              role="tab"
              aria-selected={tab === t}
              aria-label={`${stage.title}: ${stage.statusLabel}`}
              className={`stage-tab${tab === t ? " active" : ""}`}
              title={locked ? OVERVIEW_DISABLED_TITLE : stage.blocking.length ? stage.blocking.join("; ") : stage.summary}
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
