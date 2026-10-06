// Regenerate masks dialog (plan 2026-10-01-standalone-review-app, PR 4) —
// the Qt BatchMaskDialog over the review job: re-run the bundled kernel on
// the open file's images (a set with start / count, or the whole file), an
// AVI or a folder of TIFF/PNG/JPEG, with the file's recorded config, ROI and
// background by default, into a new HDF5 file the panel then opens.
import { useState } from "react";
import { open, save } from "@tauri-apps/plugin-dialog";
import { REGENERATE_SOURCE, type ReviewInfo } from "./reviewBridge";
import { exportDir, joinPath, stem } from "./exports/exportHelpers";

export interface RegenerateRequest {
  source: number;
  sourcePath: string;
  /** 0-based. */
  startIndex: number;
  /** 0 = to the end. */
  count: number;
  outputPath: string;
  useRecordedConfig: boolean;
  synthesizeBackground: boolean;
}

type Source = (typeof REGENERATE_SOURCE)[keyof typeof REGENERATE_SOURCE];

/** Validate the form; the request, or why it cannot run (pure; tested). */
export function regenerateRequest(form: {
  source: Source;
  sourcePath: string;
  from: string;
  count: string;
  outputPath: string;
  useRecordedConfig: boolean;
  synthesizeBackground: boolean;
  setSize: number;
}): { ok: true; request: RegenerateRequest } | { ok: false; error: string } {
  const external = form.source === REGENERATE_SOURCE.Avi || form.source === REGENERATE_SOURCE.Folder;
  if (external && !form.sourcePath) return { ok: false, error: form.source === REGENERATE_SOURCE.Avi ? "Choose an AVI file" : "Choose a folder of images" };
  if (!form.outputPath) return { ok: false, error: "Choose where to save the new file" };
  let startIndex = 0;
  let count = 0;
  if (form.source === REGENERATE_SOURCE.CurrentValid || form.source === REGENERATE_SOURCE.CurrentInvalid) {
    const from = Number(form.from || "1");
    const n = Number(form.count || "0");
    if (!Number.isInteger(from) || from < 1) return { ok: false, error: "Images are numbered from 1" };
    if (form.setSize > 0 && from > form.setSize) return { ok: false, error: `This set has ${form.setSize} images` };
    if (!Number.isInteger(n) || n < 0) return { ok: false, error: "Count must be 0 (to the end) or more" };
    startIndex = from - 1;
    count = n;
  }
  return {
    ok: true,
    request: {
      source: form.source,
      sourcePath: external ? form.sourcePath : "",
      startIndex,
      count,
      outputPath: form.outputPath,
      useRecordedConfig: !external && form.useRecordedConfig,
      synthesizeBackground: form.synthesizeBackground,
    },
  };
}

export function RegenerateMasksDialog(props: { info: ReviewInfo | null; onRun: (r: RegenerateRequest) => void; onCancel: () => void }) {
  const { info } = props;
  const fileOpen = !!info?.file_open;
  const recording = !!info?.recording_file;
  const [source, setSource] = useState<Source>(fileOpen ? REGENERATE_SOURCE.WholeFile : REGENERATE_SOURCE.Folder);
  const [sourcePath, setSourcePath] = useState("");
  const [from, setFrom] = useState("1");
  const [count, setCount] = useState("0");
  const [outputPath, setOutputPath] = useState("");
  const [useRecordedConfig, setUseRecordedConfig] = useState(true);
  const [synthesizeBackground, setSynthesizeBackground] = useState(!info?.has_background);
  const external = source === REGENERATE_SOURCE.Avi || source === REGENERATE_SOURCE.Folder;
  const setSize = !info
    ? 0
    : source === REGENERATE_SOURCE.CurrentInvalid
      ? Number(info.invalid_images.count)
      : Number(recording ? info.recorded_images.count : info.valid_images.count);
  const result = regenerateRequest({ source, sourcePath, from, count, outputPath, useRecordedConfig, synthesizeBackground, setSize });

  const pickSource = async () => {
    const picked =
      source === REGENERATE_SOURCE.Avi
        ? await open({ title: "AVI source", filters: [{ name: "AVI", extensions: ["avi"] }], multiple: false })
        : await open({ title: "Folder of images", directory: true, multiple: false });
    if (typeof picked === "string") setSourcePath(picked);
  };
  const pickOutput = async () => {
    const base = fileOpen && !external ? stem(info!.file_path) : sourcePath ? stem(sourcePath) : "masks";
    const out = await save({
      title: "Save regenerated masks",
      defaultPath: joinPath(exportDir(info?.file_path ?? ""), `${base}_remasked.h5`),
      filters: [{ name: "HDF5", extensions: ["h5"] }],
    });
    if (out) setOutputPath(out);
  };

  const option = (value: Source, label: string, disabled = false) => (
    <label className="radio-row">
      <input type="radio" name="regen-source" checked={source === value} disabled={disabled} onChange={() => setSource(value)} /> {label}
    </label>
  );

  return (
    <div className="modal-backdrop" onClick={props.onCancel}>
      <div className="modal regenerate-dialog" role="dialog" aria-label="Regenerate masks" onClick={(e) => e.stopPropagation()}>
        <h3>Regenerate masks</h3>
        <fieldset>
          <legend>Source</legend>
          {option(REGENERATE_SOURCE.WholeFile, recording ? "Whole file (all recorded frames)" : "Whole file (valid + invalid)", !fileOpen)}
          {option(REGENERATE_SOURCE.CurrentValid, recording ? "Recorded frames, range" : "Valid frames, range", !fileOpen)}
          {!recording && option(REGENERATE_SOURCE.CurrentInvalid, "Invalid frames, range", !fileOpen)}
          {option(REGENERATE_SOURCE.Avi, "AVI file")}
          {option(REGENERATE_SOURCE.Folder, "Folder of TIFF / PNG / JPEG")}
        </fieldset>
        {(source === REGENERATE_SOURCE.CurrentValid || source === REGENERATE_SOURCE.CurrentInvalid) && (
          <div className="row">
            <label>
              From image <input type="number" min={1} value={from} onChange={(e) => setFrom(e.target.value)} aria-label="From image (1-based)" />
            </label>
            <label>
              Count <input type="number" min={0} value={count} onChange={(e) => setCount(e.target.value)} aria-label="Count (0 = to the end)" />
            </label>
            <span className="hint">of {setSize}; 0 = to the end</span>
          </div>
        )}
        {external && (
          <div className="row">
            <input type="text" readOnly value={sourcePath} placeholder={source === REGENERATE_SOURCE.Avi ? "No AVI chosen" : "No folder chosen"} aria-label="Source path" />
            <button className="btn" onClick={() => void pickSource()}>
              Choose…
            </button>
          </div>
        )}
        <label className="check-row" title={external ? "AVI and folder sources use the default processing config" : undefined}>
          <input type="checkbox" checked={!external && useRecordedConfig} disabled={external} onChange={(e) => setUseRecordedConfig(e.target.checked)} /> Use the file's
          recorded config, ROI and background
        </label>
        <label className="check-row">
          <input type="checkbox" checked={synthesizeBackground} onChange={(e) => setSynthesizeBackground(e.target.checked)} /> Synthesize a background when
          none is recorded
        </label>
        <div className="row">
          <input type="text" readOnly value={outputPath} placeholder="Output .h5" aria-label="Output file" />
          <button className="btn" onClick={() => void pickOutput()}>
            Save as…
          </button>
        </div>
        {!result.ok && <p className="form-error">{result.error}</p>}
        <div className="actions">
          <button className="btn" onClick={props.onCancel}>
            Cancel
          </button>
          <button className="btn primary" disabled={!result.ok} onClick={() => result.ok && props.onRun(result.request)}>
            Regenerate
          </button>
        </div>
      </div>
    </div>
  );
}
