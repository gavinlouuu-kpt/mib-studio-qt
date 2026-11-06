// Regenerate masks dialog (plan 2026-10-01-standalone-review-app, PR 4; #867): the Qt
// BatchMaskDialog over the review job, in its three groups. Input source: the open file's
// images (the whole file, or a set with From / Count), an AVI or a folder of TIFF/PNG/JPEG.
// Preview & ROI: the source's frames with Prev / Next and "Frame i / n", the ROI drawn on
// the frame (pre-filled from the file's /experiment_info), Set as Background / Clear and the
// synthetic-background box. Processing Config: the eight Qt fields, pre-filled from the file's
// recorded config (the defaults for AVI / folder), "Reset to recorded config". The run goes to
// a new HDF5 file the panel then opens.
//
// Review ABI 8 (#867): the setup and preview come from fetch_review_regenerate_setup /
// fetch_review_regenerate_preview, the run from review_regenerate_masks_json (the same job as
// review_regenerate_masks, with the dialog's config fields, ROI and background).
//
// The job never replaces a file (#512 S8, decision D6: it runs on the reanalysis engine,
// partial file + no-replace publish). "Save as…" proposes Qt's `<base>_remasked.h5` beside the
// source (inside an image folder), then `_2`, `_3`, … when taken; if the operator picks an
// existing file anyway, the backend refuses, the dialog stays open with the reason and the
// operator chooses again.
import { useEffect, useMemo, useRef, useState } from "react";
import { open, save } from "../transport/dialogs";
import {
  REGENERATE_SOURCE,
  packetToImageData,
  reviewBridge,
  type RegenerateMasksJsonRequest,
  type RegenerateSetup,
  type ReviewInfo,
} from "./reviewBridge";
import { baseName, defaultRegenerateName, dirName, joinPath } from "./exports/exportHelpers";
import {
  DEFAULT_REGEN_CONFIG,
  backgroundLabel,
  configFromImageProcessing,
  imageProcessingOverride,
  previewWindow,
  remaskedDefault,
  roiError,
  type RegenBackground,
  type RegenConfig,
  type Roi,
} from "./regenerate/regenerateForm";
import { ProcessingConfigGroup, RoiPreviewPanel, type PreviewFrame } from "./regenerate/RegeneratePanels";

/** The request the panel starts (contract review_regenerate_masks_json). */
export type RegenerateRequest = RegenerateMasksJsonRequest;

type Source = (typeof REGENERATE_SOURCE)[keyof typeof REGENERATE_SOURCE];

const isExternal = (source: Source) => source === REGENERATE_SOURCE.Avi || source === REGENERATE_SOURCE.Folder;
const isRange = (source: Source) => source === REGENERATE_SOURCE.CurrentValid || source === REGENERATE_SOURCE.CurrentInvalid;

/** Validate the form; the request, or why it cannot run (pure; tested). */
export function regenerateRequest(form: {
  source: Source;
  sourcePath: string;
  from: string;
  count: string;
  outputPath: string;
  setSize: number;
  config: RegenConfig;
  roi: Roi | null;
  background: RegenBackground;
  synthesizeBackground: boolean;
  frameWidth?: number;
  frameHeight?: number;
}): { ok: true; request: RegenerateRequest } | { ok: false; error: string } {
  const external = isExternal(form.source);
  if (external && !form.sourcePath) return { ok: false, error: form.source === REGENERATE_SOURCE.Avi ? "Choose an AVI file" : "Choose a folder of images" };
  let start = 0;
  let count = 0;
  if (isRange(form.source)) {
    const from = Number(form.from || "1");
    const n = Number(form.count || "0");
    if (!Number.isInteger(from) || from < 1) return { ok: false, error: "Images are numbered from 1" };
    if (form.setSize > 0 && from > form.setSize) return { ok: false, error: `This set has ${form.setSize} images` };
    if (!Number.isInteger(n) || n < 0) return { ok: false, error: "Count must be 0 (to the end) or more" };
    start = from - 1;
    count = n;
  }
  const roiProblem = roiError(form.roi, form.frameWidth, form.frameHeight);
  if (roiProblem) return { ok: false, error: roiProblem };
  if (!form.outputPath) return { ok: false, error: "Choose where to save the new file" };
  const request: RegenerateRequest = {
    source: form.source,
    source_path: external ? form.sourcePath : "",
    start,
    count,
    output_path: form.outputPath,
    // The file's recorded factor, config and (with "recorded") background are the base;
    // AVI and folder sources start from the defaults at the default factor.
    use_recorded_config: !external,
    synthesize_background: form.synthesizeBackground,
    image_processing: imageProcessingOverride(form.config),
    roi: form.roi ?? { x: 0, y: 0, w: 0, h: 0 },
    background: form.background.kind,
  };
  if (form.background.kind === "frame") {
    request.background_index = form.background.index;
    if (!external) request.background_dataset = form.background.dataset;
  }
  return { ok: true, request };
}

/** What the run will be validated at and compute E with (REVIEW-064), before it runs. The
 * job's completion message names what it used. */
export function regenerateCalibration(info: ReviewInfo | null, external: boolean): string[] {
  if (external || !info?.file_open) {
    return ["Validated at 0.4886 µm/px (the default: AVI and folder sources record no factor)", "E from the review app's own E-LUT, if one is loaded"];
  }
  const factor = Number(info.pixel_to_micron);
  const lines = [
    Number.isFinite(factor) && factor > 0
      ? `Validated at ${factor.toFixed(4)} µm/px (${info.pixel_to_micron_from_file ? "recorded in the file" : "fallback from Preferences: the file records none"})`
      : "Validated at the file's recorded px→µm factor",
  ];
  const lut = info.identity?.emodulus_lut ?? null;
  const own = lut && info.identity?.emodulus_lut_provenance !== "host";
  if (own && lut.loaded) lines.push(`E from the source run's E-LUT ${[lut.lut_id, lut.revision].filter(Boolean).join(" ") || "(unnamed)"}`);
  else if (own) lines.push("E not computed: the source run loaded no E-LUT");
  else lines.push("E from the review app's own E-LUT, if one is loaded: the source records none");
  return lines;
}

function setupConfig(setup: RegenerateSetup | null): RegenConfig {
  return setup?.ok && setup.config ? configFromImageProcessing(setup.config) : DEFAULT_REGEN_CONFIG;
}

/** `onRun` resolves once the job started (null; the panel closes the dialog)
 * or with the backend's refusal, which the dialog shows. */
export function RegenerateMasksDialog(props: { info: ReviewInfo | null; onRun: (r: RegenerateRequest) => Promise<string | null>; onCancel: () => void }) {
  const { info } = props;
  const fileOpen = !!info?.file_open;
  const recording = !!info?.recording_file;
  const [source, setSource] = useState<Source>(fileOpen ? REGENERATE_SOURCE.WholeFile : REGENERATE_SOURCE.Folder);
  const [sourcePath, setSourcePath] = useState("");
  const [from, setFrom] = useState("1");
  // Qt's Count default (Start 0 / Count 100); 0 still runs to the end.
  const [count, setCount] = useState("100");
  const [outputPath, setOutputPath] = useState("");
  const [setup, setSetup] = useState<RegenerateSetup | null>(null);
  const [config, setConfig] = useState<RegenConfig>(DEFAULT_REGEN_CONFIG);
  const [roi, setRoi] = useState<Roi | null>(null);
  const [background, setBackground] = useState<RegenBackground>({ kind: info?.has_background ? "recorded" : "none" });
  // Qt: "Use synthetic background when none selected", on by default.
  const [synthetic, setSynthetic] = useState(true);
  const [previewIndex, setPreviewIndex] = useState(0);
  const [frame, setFrame] = useState<PreviewFrame | null>(null);
  const [previewError, setPreviewError] = useState("");
  const [previewLoading, setPreviewLoading] = useState(false);
  // The backend's reason the last Regenerate was not started (e.g. the
  // output exists), shown until the operator picks another output.
  const [refusal, setRefusal] = useState("");
  const [starting, setStarting] = useState(false);
  const external = isExternal(source);
  const kind = source === REGENERATE_SOURCE.Avi ? "avi" : source === REGENERATE_SOURCE.Folder ? "folder" : "hdf";

  // ---- setup: re-read when the source changes (Qt onPreviewSourceChanged) ----
  const setupKey = `${source === REGENERATE_SOURCE.CurrentInvalid ? "invalid" : external ? kind : "file"}|${external ? sourcePath : ""}`;
  const configKey = external ? `ext|${sourcePath}` : "file";
  const lastConfigKey = useRef<string | null>(null);
  useEffect(() => {
    if (external && !sourcePath) {
      setSetup(null);
      return;
    }
    let live = true;
    setSetup(null);
    void reviewBridge
      .regenerateSetup(source, external ? sourcePath : "")
      .catch((e) => ({ ok: false, error: String(e) }) as RegenerateSetup)
      .then((s) => {
        if (!live) return;
        setSetup(s);
        // Qt resets the background and re-reads the ROI on every source change; the
        // config fields keep the operator's edits unless the source file itself changed.
        setBackground({ kind: !external && s.ok && s.has_background ? "recorded" : "none" });
        setRoi(s.ok && s.roi_recorded && s.roi ? s.roi : null);
        if (s.ok && lastConfigKey.current !== configKey) {
          lastConfigKey.current = configKey;
          setConfig(setupConfig(s));
        }
      });
    return () => {
      live = false;
    };
  }, [setupKey]); // eslint-disable-line react-hooks/exhaustive-deps

  const setSize = setup?.ok ? Number(setup.frame_count ?? "0") : 0;
  const previewWin = useMemo(
    () => previewWindow(setSize, !isRange(source), Number(from || "1") - 1, Number(count || "0")),
    [setSize, source, from, count],
  );
  useEffect(() => setPreviewIndex(0), [setupKey, previewWin.offset, previewWin.total]);

  // ---- preview frame (Qt loadPreviewFrame) ----
  const previewSerial = useRef(0);
  useEffect(() => {
    const serial = ++previewSerial.current;
    if (!setup?.ok || previewWin.total === 0 || previewIndex >= previewWin.total) {
      setFrame(null);
      setPreviewError("");
      setPreviewLoading(false);
      return;
    }
    setPreviewLoading(true);
    const index = previewWin.offset + previewIndex;
    void reviewBridge
      .regeneratePreview(source, external ? sourcePath : "", index)
      .then((packet) => {
        if (serial !== previewSerial.current) return;
        if (!packet.valid || packet.width <= 0 || packet.height <= 0) throw new Error("no frame");
        setFrame({ width: packet.width, height: packet.height, image: packetToImageData(packet) });
        setPreviewError("");
      })
      .catch(() => {
        if (serial !== previewSerial.current) return;
        setFrame(null);
        setPreviewError(`Cannot show frame ${previewIndex + 1} of this source`);
      })
      .finally(() => serial === previewSerial.current && setPreviewLoading(false));
  }, [setup, previewWin.offset, previewWin.total, previewIndex]); // eslint-disable-line react-hooks/exhaustive-deps

  const result = regenerateRequest({
    source,
    sourcePath,
    from,
    count,
    outputPath,
    setSize,
    config,
    roi,
    background,
    synthesizeBackground: synthetic,
    frameWidth: frame?.width,
    frameHeight: frame?.height,
  });

  // The config fields hold the source's values only once its setup was read: until then
  // (or when it was refused, e.g. while a job runs) a run would send other values.
  const setupBlock =
    external && !sourcePath ? "" : !setup ? "Reading the source…" : !setup.ok ? `Cannot read the source: ${setup.error}` : "";
  const canRun = result.ok && !setupBlock;

  const pickSource = async () => {
    const picked =
      source === REGENERATE_SOURCE.Avi
        ? await open({ title: "AVI source", filters: [{ name: "AVI", extensions: ["avi"] }], multiple: false })
        : await open({ title: "Folder of images", directory: true, multiple: false });
    if (typeof picked === "string") setSourcePath(picked);
  };
  const pickOutput = async () => {
    const auto = remaskedDefault(kind, external ? sourcePath : (info?.file_path ?? ""));
    const dir = outputPath ? dirName(outputPath) : auto.dir;
    let taken: string[] = [];
    try {
      taken = dir ? await reviewBridge.listDir(dir) : [];
    } catch {
      taken = []; // unlisted: the backend still refuses an existing file
    }
    const out = await save({
      title: "Save regenerated masks",
      defaultPath: joinPath(dir, defaultRegenerateName(auto.base, taken)),
      filters: [{ name: "HDF5", extensions: ["h5"] }],
    });
    if (out) {
      setOutputPath(out);
      setRefusal("");
    }
  };
  const run = async () => {
    if (!result.ok || !canRun || starting) return;
    const picked = result.request.output_path;
    setStarting(true);
    setRefusal("");
    const why = await props.onRun(result.request);
    setStarting(false);
    if (why) {
      setRefusal(`Not started: ${why} (${baseName(picked)})`);
      setOutputPath("");
    }
  };

  const option = (value: Source, label: string, disabled = false) => (
    <label className="radio-row">
      <input type="radio" name="regen-source" checked={source === value} disabled={disabled} onChange={() => setSource(value)} /> {label}
    </label>
  );
  const busy = starting;
  const recordedNote = setup?.ok && setup.config_recorded ? "From the file's recorded config" : "Defaults: the source records no config";
  const calibration = regenerateCalibration(info, external);

  return (
    <div className="modal-backdrop" onClick={props.onCancel}>
      <div className="modal regenerate-dialog" role="dialog" aria-label="Regenerate masks" onClick={(e) => e.stopPropagation()}>
        <h3>Regenerate masks</h3>
        <div className="regen-columns">
          <fieldset data-tour="regen-source">
            <legend>Input source</legend>
            {option(REGENERATE_SOURCE.WholeFile, recording ? "Whole file (all recorded frames)" : "Whole file (valid + invalid)", !fileOpen)}
            {option(REGENERATE_SOURCE.CurrentValid, recording ? "Recorded frames, range" : "Valid frames, range", !fileOpen)}
            {!recording && option(REGENERATE_SOURCE.CurrentInvalid, "Invalid frames, range", !fileOpen)}
            {isRange(source) && (
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
            {option(REGENERATE_SOURCE.Avi, "AVI file")}
            {option(REGENERATE_SOURCE.Folder, "Folder of TIFF / PNG / JPEG")}
            {external && (
              <div className="row">
                <input type="text" readOnly value={sourcePath} placeholder={source === REGENERATE_SOURCE.Avi ? "No AVI chosen" : "No folder chosen"} aria-label="Source path" />
                <button className="btn" onClick={() => void pickSource()}>
                  Choose…
                </button>
              </div>
            )}
          </fieldset>
          <RoiPreviewPanel
            frame={frame}
            loading={previewLoading}
            error={previewError}
            index={previewIndex}
            total={setup?.ok ? previewWin.total : external && !sourcePath ? 0 : null}
            onIndex={(i) => setPreviewIndex(Math.max(0, i))}
            roi={roi}
            onRoi={setRoi}
            roiNote={!external && setup?.ok && setup.roi_recorded ? "pre-filled from the file" : undefined}
            backgroundLabel={backgroundLabel(background, synthetic)}
            onSetBackground={() =>
              setBackground({ kind: "frame", index: previewWin.offset + previewIndex, dataset: setup?.preview_dataset ?? "", label: previewIndex + 1 })
            }
            onClearBackground={() => setBackground({ kind: "none" })}
            extraBackground={
              !external && setup?.ok && setup.has_background ? (
                <button className="btn" disabled={busy || background.kind === "recorded"} onClick={() => setBackground({ kind: "recorded" })} title="The background saved in the file">
                  Use saved
                </button>
              ) : undefined
            }
            synthetic={synthetic}
            onSynthetic={setSynthetic}
            disabled={busy}
          />
          <ProcessingConfigGroup
            config={config}
            onChange={setConfig}
            onReset={() => setConfig(setupConfig(setup))}
            resetLabel={!external && setup?.ok && setup.config_recorded ? "Reset to recorded config" : "Reset to defaults"}
            note={`${recordedNote}. Processing parameters are local to this run.`}
            disabled={busy}
          />
        </div>
        <div className="regen-calibration" data-tour="regen-calibration">
          {calibration.map((line) => (
            <div key={line}>{line}</div>
          ))}
        </div>
        {!external && setup?.ok && setup.core_warning && (
          <p className="regen-warning" aria-live="polite">
            <strong>Processing-core mismatch. </strong>
            {setup.core_warning}
          </p>
        )}
        <div className="row" data-tour="regen-output">
          <input type="text" readOnly value={outputPath} placeholder="Output .h5" aria-label="Output file" />
          <button className="btn" onClick={() => void pickOutput()}>
            Save as…
          </button>
        </div>
        {refusal ? (
          <p className="form-error" role="alert">
            {refusal}
          </p>
        ) : (
          (!result.ok || setupBlock) && <p className="form-error">{!result.ok ? result.error : setupBlock}</p>
        )}
        <div className="actions">
          <button className="btn" onClick={props.onCancel}>
            Cancel
          </button>
          <button className="btn primary" data-tour="regen-run" disabled={!canRun || starting} onClick={() => void run()}>
            Regenerate
          </button>
        </div>
      </div>
    </div>
  );
}
