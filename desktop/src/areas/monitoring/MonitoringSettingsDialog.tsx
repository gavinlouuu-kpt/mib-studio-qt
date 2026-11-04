// Monitoring Settings dialog (epic #866, MON-082, MON-087..091): Settings > Monitoring Settings... Qt's
// modal MonitoringSettingsDialog with OK / Cancel / Apply over the fixed chart axes and the histogram
// bin width. The fields come from MONITORING_SETTINGS_GROUPS (monitoringSettings.ts): the Density (KDE)
// fields of MON-083..085 are the first group there (PR E2), and this component renders whatever groups it is
// given. The values are this client's view preference; the density ones reach the backend through the
// settings push of density/useMonitoringDensity.ts, not from here. The reference contour buttons of MON-086
// (Pin current, From file…, Clear) sit under the density group's fields and act at once.
import { useEffect, useRef, useState } from "react";
import { useStudio } from "../../studio/StudioContext";
import { DensityReferenceControls } from "./density/DensityReferenceControls";
import type { DensityReferenceControlsModel } from "./density/useMonitoringDensity";
import {
  applyMonitoringSettings, MONITORING_SETTINGS_GROUPS, parseSettingText, settingText,
  type MonitoringSettingKey, type MonitoringSettings, type MonitoringSettingsValues, type SettingsGroup, type SettingPair,
} from "./monitoringSettings";

export interface MonitoringSettingsDialogViewProps {
  settings: MonitoringSettings;
  groups?: readonly SettingsGroup[];
  /** Apply the values (Apply and OK); returns the ranges that were not applied. */
  onApply: (values: MonitoringSettingsValues) => SettingPair["id"][];
  onClose: () => void;
  /** The reference contour row of the density group (PR E2); absent, the group is its fields only. */
  reference?: DensityReferenceControlsModel;
}

type Draft = Partial<Record<MonitoringSettingKey, string>>;

const draftOf = (settings: MonitoringSettings, groups: readonly SettingsGroup[]): Draft =>
  Object.fromEntries(groups.flatMap((g) => g.fields).map((f) => [f.key, settingText(f, settings[f.key])]));

export function MonitoringSettingsDialogView({ settings, groups = MONITORING_SETTINGS_GROUPS, onApply, onClose, reference }: MonitoringSettingsDialogViewProps) {
  const [draft, setDraft] = useState<Draft>(() => draftOf(settings, groups));
  const [ignored, setIgnored] = useState<SettingPair["id"][]>([]);
  const first = useRef<HTMLInputElement>(null);
  useEffect(() => first.current?.focus(), []);

  // What a spin box holds after its edit: the text as a clamped, rounded number, or the last value when
  // the text is not a number.
  const valuesOfDraft = (): MonitoringSettingsValues => {
    const values = { ...settings } as MonitoringSettingsValues;
    for (const f of groups.flatMap((g) => g.fields)) {
      const n = parseSettingText(f, draft[f.key] ?? "");
      if (n !== null) values[f.key] = n;
    }
    return values;
  };
  const apply = (): boolean => {
    const values = valuesOfDraft();
    const rejected = onApply(values);
    setIgnored(rejected);
    setDraft(draftOf({ ...settings, ...values }, groups));
    return rejected.length === 0;
  };
  const labelOf = (id: SettingPair["id"]) => groups.flatMap((g) => g.pairs).find((p) => p.id === id)?.label ?? id;

  return (
    <div className="modal-backdrop">
      <form
        className="modal mon-settings"
        role="dialog"
        aria-modal="true"
        aria-label="Monitoring Settings"
        data-tour="monitoring-settings-dialog"
        // The values are clamped and rounded on Apply, like a spin box, so the browser's own validation
        // (it would block OK on a value off the step grid, such as the default bin width 0.5 above 0.01)
        // is off.
        noValidate
        onKeyDown={(e) => {
          if (e.key === "Escape") onClose();
        }}
        onSubmit={(e) => {
          e.preventDefault();
          // OK: apply and close, unless a range could not be applied: then the reason stays on screen.
          if (apply()) onClose();
        }}
      >
        <h3>Monitoring Settings</h3>
        {groups.map((group, g) => (
          <fieldset key={group.id} className="mon-settings-group" data-tour={`monitoring-settings-${group.id}`}>
            <legend>{group.title}</legend>
            {group.fields.map((f, i) => (
              <label key={f.key} className="row" title={f.title}>
                <span className="mon-settings-label">{f.label}</span>
                <input
                  ref={g === 0 && i === 0 ? first : undefined}
                  type="number"
                  inputMode="decimal"
                  // Arrow stepping counts from `min`: a minimum that is not a multiple of the step (the bin
                  // width's 0.01 over a 0.1 step) would put 0.51, 0.61 ... on the arrows, not Qt's 0.6, 0.7.
                  min={f.min % f.step === 0 ? f.min : undefined}
                  max={f.max}
                  step={f.step}
                  value={draft[f.key] ?? ""}
                  data-tour={f.tour}
                  onChange={(e) => setDraft((d) => ({ ...d, [f.key]: e.target.value }))}
                />
                {f.suffix && <span className="mon-settings-suffix">{f.suffix}</span>}
              </label>
            ))}
            {group.id === "density" && reference && <DensityReferenceControls controls={reference} />}
          </fieldset>
        ))}
        {ignored.length > 0 && (
          <p className="mon-settings-note" role="status">
            Not applied: {ignored.map(labelOf).join(", ")}. The minimum must be below the maximum; the previous range is kept.
          </p>
        )}
        <div className="actions">
          <button type="submit" className="btn">OK</button>
          <button type="button" className="btn" onClick={onClose}>Cancel</button>
          <button type="button" className="btn" onClick={apply} data-tour="monitoring-settings-apply">Apply</button>
        </div>
      </form>
    </div>
  );
}

/** The dialog in the classic view: open while `showMonitoringSettings`, applying into the studio state.
 *  It is remounted on every open, so the fields start from the current settings (Qt builds a new dialog
 *  each time). */
export function MonitoringSettingsDialog() {
  const { showMonitoringSettings, setShowMonitoringSettings, monSettings, setMonSettings, kdeReferenceControls } = useStudio();
  if (!showMonitoringSettings) return null;
  return (
    <MonitoringSettingsDialogView
      settings={monSettings}
      reference={kdeReferenceControls}
      onApply={(values) => {
        const { settings, ignored } = applyMonitoringSettings(monSettings, values);
        setMonSettings(settings);
        return ignored;
      }}
      onClose={() => setShowMonitoringSettings(false)}
    />
  );
}
