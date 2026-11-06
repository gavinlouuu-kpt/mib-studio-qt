// The Monitoring tune panel (epic #866, MON-069..081): the Qt panel beside the charts
// (src/frontend/tabs/ExperimentMonitoringTab.cpp setupTuneParamsPanel). A scrolling body of
// checkable criteria (a criterion's value controls disable but keep their values while it is
// unchecked) and a fixed footer with the draft's state, Apply and Revert. The draft and its rules are
// tuneDraft.ts; syncing with the running configuration, Apply and Revert are useTune.ts.
import { useEffect, useState, type ReactNode } from "react";
import { ViewerGate } from "../../components/ViewerGate";
import { PL_IGNORED_SETTINGS_TEXT } from "../../platformCapabilities";
import { useStudio } from "../../studio/StudioContext";
import { EMODULUS_TABLE_UNKNOWN, emodulusTableHint } from "./emodulusLut";
import {
  RUN_LOCKED_FIELDS, RUN_LOCKED_REASON, appliedValueText, formatTuneValue, normalizeTuneValue, tuneApplyBlocker, tuneCanApply,
  tuneCanRevert, tuneChanged, tuneSpec, tuneStateText, tuneValidationList, type TuneFieldId, type TuneState,
} from "./tuneDraft";

const kebab = (id: string) => id.replace(/[A-Z]/g, (c) => `-${c.toLowerCase()}`);
const tour = (id: string) => `monitoring-tune-${kebab(id)}`;

interface Ctx {
  s: TuneState;
  set: (field: TuneFieldId, value: unknown) => void;
  /** The controls are read-only: the running configuration is not known yet, or an apply is in flight. */
  frozen: boolean;
  /** Fields a running experiment keeps locked. */
  locked: ReadonlySet<TuneFieldId>;
}

/** The tooltip of a value: its name, plus the applied value while the draft differs from it (Qt). */
function fieldTitle(ctx: Ctx, id: TuneFieldId): string {
  const spec = tuneSpec(id);
  const was = ctx.s.baseline?.[id];
  return tuneChanged(ctx.s, id) && was !== undefined ? `${spec.label} — unapplied change (applied value: ${appliedValueText(was)})` : spec.label;
}

/** A spin box: shows the value at its decimals, commits what is typed once it is a number (rounded
 * and clamped to the range like Qt's), and shows the normalised value again when it loses focus. */
function TuneNumber({ ctx, id, enabled }: { ctx: Ctx; id: TuneFieldId; enabled: boolean }) {
  const spec = tuneSpec(id);
  const [typed, setTyped] = useState<string | null>(null);
  const value = ctx.s.draft?.[id];
  const changed = tuneChanged(ctx.s, id);
  const locked = ctx.locked.has(id);
  // What is typed stays on screen while it still means the draft's value (a half-typed "0." is 0); a
  // draft that moved for another reason (Revert, a newer configuration) shows its own value again.
  useEffect(() => {
    if (typed === null) return;
    const meant = normalizeTuneValue(spec, typed);
    if (meant !== undefined && meant !== value) setTyped(null);
  }, [value]); // eslint-disable-line react-hooks/exhaustive-deps
  const shown = typed ?? (value === undefined ? "" : formatTuneValue(spec, value));
  return (
    <label className={`tune-row${changed ? " tune-changed" : ""}`} title={locked ? RUN_LOCKED_REASON : fieldTitle(ctx, id)}>
      <span className="tune-row-label">{spec.rowLabel}{changed ? " *" : ""}</span>
      <span className="tune-row-field">
        <input
          type="number" inputMode="decimal" min={spec.min} max={spec.max} step={spec.step} value={shown}
          aria-label={spec.label} data-tour={tour(id)} data-changed={changed ? "true" : undefined}
          disabled={ctx.frozen || !enabled || locked}
          onChange={(e) => {
            setTyped(e.target.value);
            const next = normalizeTuneValue(spec, e.target.value);
            if (next !== undefined) ctx.set(id, next);
          }}
          onBlur={() => setTyped(null)}
        />
        {spec.unit && <span className="tune-unit">{spec.unit.trim()}</span>}
      </span>
    </label>
  );
}

/** A checkable criterion: its enable switch and, below, the controls it enables. */
function Criterion({ ctx, id, name, tooltip, children, description, parentOff = false }: {
  ctx: Ctx; id: TuneFieldId; name: string; tooltip: string; children?: ReactNode; description?: string;
  /** A box nested in another one (the Young's modulus range in the target group) is disabled with it. */
  parentOff?: boolean;
}) {
  const on = Boolean(ctx.s.draft?.[id]);
  const changed = tuneChanged(ctx.s, id);
  const locked = ctx.locked.has(id);
  return (
    <div className={`tune-criterion${on ? "" : " tune-off"}${changed ? " tune-changed" : ""}`} role="group" aria-label={name} data-tour={tour(id.replace(/Enabled$/, ""))}>
      <label className="tune-criterion-title" title={locked ? RUN_LOCKED_REASON : changed && ctx.s.baseline ? `${tooltip} — unapplied change (applied value: ${appliedValueText(ctx.s.baseline[id])})` : tooltip}>
        <input
          type="checkbox" checked={on} disabled={ctx.frozen || locked || parentOff} data-changed={changed ? "true" : undefined}
          onChange={(e) => ctx.set(id, e.target.checked)}
        />
        <span>{name}{changed ? " *" : ""}</span>
      </label>
      {description && <p className="tune-hint">{description}</p>}
      {children && <div className="tune-values">{children}</div>}
    </div>
  );
}

export function TunePanel() {
  const { tune: s, setTuneField, onTuneApply, onTuneRevert, tuneScopeNow: scope, checkedConfig, hostProcessing, emodulusTable = EMODULUS_TABLE_UNKNOWN } = useStudio();
  const frozen = !s.draft || s.applying;
  const locked: ReadonlySet<TuneFieldId> = new Set(scope.midRun ? RUN_LOCKED_FIELDS : []);
  const ctx: Ctx = { s, set: setTuneField, frozen, locked };
  const issues = tuneValidationList(s);
  const blocker = s.draft && s.changed.length ? tuneApplyBlocker(s, scope) : "";
  const doc = checkedConfig.doc;
  const note = blocker
    ? blocker
    : s.target && !s.changed.length
      ? s.target.kind === "file"
        ? `Applied and saved to ${s.target.path}.`
        : `Applied to the running configuration only. Not saved to a file: ${s.target.why || "no configuration file is open"}.`
      : scope.midRun
        ? scope.reason
        : doc
          ? `Apply also saves to ${doc.path.split(/[\\/]/).pop()} when it matches the running configuration.`
          : "Apply changes the running configuration; open a configuration file in App config to save them too.";
  return (
    <aside className="tune-panel" aria-label="Tune panel" data-tour="monitoring-tune-panel">
      <div className="tune-inner">
      <ViewerGate>
        <div className="tune-scroll">
          <h5 className="tune-heading" data-tour="monitoring-tune-acceptance-heading">Cell acceptance filters</h5>
          <p className="tune-hint">An object must pass every enabled criterion to count as a valid cell.</p>
          {!hostProcessing && <p className="tune-hint" data-testid="tune-pl-note">The PL runs the science on this instrument. It does not implement {PL_IGNORED_SETTINGS_TEXT}: changing those has no effect.</p>}
          <Criterion ctx={ctx} id="areaEnabled" name="Area (µm²)" tooltip="Accept objects whose projected area lies within this range (µm², after pixel-to-micron conversion).">
            <TuneNumber ctx={ctx} id="areaMin" enabled={Boolean(s.draft?.areaEnabled)} />
            <TuneNumber ctx={ctx} id="areaMax" enabled={Boolean(s.draft?.areaEnabled)} />
          </Criterion>
          <Criterion ctx={ctx} id="deformabilityEnabled" name="Deformability" tooltip="Accept objects whose deformability (1 − circularity, unitless) lies within this range.">
            <TuneNumber ctx={ctx} id="deformabilityMin" enabled={Boolean(s.draft?.deformabilityEnabled)} />
            <TuneNumber ctx={ctx} id="deformabilityMax" enabled={Boolean(s.draft?.deformabilityEnabled)} />
          </Criterion>
          <Criterion ctx={ctx} id="ringRatioEnabled" name="Ring ratio" tooltip="Accept objects whose ring ratio (focus indicator reported by the processing core, unitless) lies within this range.">
            <TuneNumber ctx={ctx} id="ringRatioMin" enabled={Boolean(s.draft?.ringRatioEnabled)} />
            <TuneNumber ctx={ctx} id="ringRatioMax" enabled={Boolean(s.draft?.ringRatioEnabled)} />
          </Criterion>
          <Criterion ctx={ctx} id="areaRatioEnabled" name="Area ratio" tooltip="Reject objects whose convex-hull to contour area ratio (unitless) exceeds the maximum.">
            <TuneNumber ctx={ctx} id="areaRatioMax" enabled={Boolean(s.draft?.areaRatioEnabled)} />
          </Criterion>
          <Criterion ctx={ctx} id="borderEnabled" name="Border exclusion" tooltip="Border exclusion: an object whose contour touches the ROI edge is rejected." description="Reject objects touching the ROI edge." />
          <Criterion ctx={ctx} id="singleInnerContourEnabled" name="Single inner contour" tooltip="Single inner contour: objects with zero or several inner contours are rejected." description="Reject objects with zero or several inner contours." />

          <h5 className="tune-heading" data-tour="monitoring-tune-target-heading">Target group / sorting gate</h5>
          <p className="tune-hint">Applied to valid cells only: selects which of them fire the sort trigger. Never changes validity.</p>
          <Criterion ctx={ctx} id="targetGroupEnabled" name="Target group gate" tooltip="When enabled, a valid cell inside every enabled range is reported as target group and fires the sort trigger.">
            <TuneNumber ctx={ctx} id="targetAreaMin" enabled={Boolean(s.draft?.targetGroupEnabled)} />
            <TuneNumber ctx={ctx} id="targetAreaMax" enabled={Boolean(s.draft?.targetGroupEnabled)} />
            <TuneNumber ctx={ctx} id="targetDeformabilityMin" enabled={Boolean(s.draft?.targetGroupEnabled)} />
            <TuneNumber ctx={ctx} id="targetDeformabilityMax" enabled={Boolean(s.draft?.targetGroupEnabled)} />
            <Criterion ctx={ctx} id="targetEmodulusEnabled" name="Young's modulus" parentOff={!s.draft?.targetGroupEnabled}
              tooltip="When enabled, a target cell must also have a Young's modulus (kPa, looked up in the loaded E-modulus table) inside this range. A cell whose lookup falls outside the table is never target group. When no table is loaded this range is skipped and the target group is the area and deformability box alone.">
              <TuneNumber ctx={ctx} id="targetEmodulusMin" enabled={Boolean(s.draft?.targetGroupEnabled && s.draft?.targetEmodulusEnabled)} />
              <TuneNumber ctx={ctx} id="targetEmodulusMax" enabled={Boolean(s.draft?.targetGroupEnabled && s.draft?.targetEmodulusEnabled)} />
              <p className="tune-hint" data-testid="tune-emodulus-hint" data-tour="monitoring-tune-emodulus-table">{emodulusTableHint(emodulusTable)}</p>
            </Criterion>
          </Criterion>

          <h5 className="tune-heading" data-tour="monitoring-tune-multi-image-heading">Multi-image acquisition</h5>
          <Criterion ctx={ctx} id="multiImageEnabled" name="Record image series" tooltip="Capture N consecutive frames per valid detection; metrics come from the first frame.">
            <TuneNumber ctx={ctx} id="multiImageCount" enabled={Boolean(s.draft?.multiImageEnabled)} />
          </Criterion>
          {scope.midRun && <p className="tune-hint" data-testid="tune-multi-locked">{RUN_LOCKED_REASON}</p>}
        </div>
        <div className="tune-footer" data-tour="monitoring-tune-footer">
          <p className="tune-state" role="status" data-testid="tune-state">{tuneStateText(s)}</p>
          {issues.length > 0 && <ul className="tune-issues" data-testid="tune-issues">{issues.map((m) => <li key={m}>{m}</li>)}</ul>}
          <p className="tune-note" data-testid="tune-note">{note}</p>
          <div className="tune-buttons">
            <button
              className="btn" data-tour="monitoring-tune-apply" disabled={!tuneCanApply(s, scope)}
              title={blocker || "Apply the changed criteria to the running configuration, and save them to the open configuration file when it matches."}
              onClick={() => void onTuneApply()}
            >Apply changes</button>
            <button data-tour="monitoring-tune-revert" disabled={!tuneCanRevert(s)} title="Discard unapplied edits and reload the current configuration." onClick={() => void onTuneRevert()}>Revert</button>
          </div>
        </div>
      </ViewerGate>
      </div>
    </aside>
  );
}
