import { joinPath, stem, withFcsExtension } from "./review/exports/exportHelpers";

/** Backend-owned transactional export. Counters and identities use decimal strings. */
export interface ReviewExportRequest {
  output_root: string;
  /** Optional independent batch sources; does not replace the open review file. */
  source_paths?: string[];
  isoelastic_overlays?:boolean;
  format: "metrics_csv" | "images" | "all" | "charts" | "fcs";
  /** FCS only: which detections (default valid, as the Qt tab). */
  fcs_frames?: "valid" | "invalid" | "both";
  /** `recorded`: each file at its recorded px→µm factor, `conversion_factor` only for a file
   * that records none (#867, TD-17). The results then name the factor used. */
  conversion_factor_source?: "recorded" | "request";
  /** Skip raw recordings (Qt batch rule); FCS always refuses them. */
  refuse_recordings?: boolean;
  frames?: "valid" | "invalid" | "both";
  series?: {enabled:boolean;start:number;end?:number};
  conversion_factor?: number;
  keep_partial_on_failure?: boolean;
  explicit_destination?: string;
}
export interface ReviewExportStatus {
  state: "idle" | "running" | "completed" | "cancelled" | "failed";
  file_index?: number;
  file_count?: number;
  source_path?: string;
  results?: ReviewExportFileResult[];
  operation_id?: string;
  job_id?: string;
  phase?: string;
  completed?: string;
  total?: string;
  current_output?: string;
  final_path?: string;
  retained_partial_path?: string;
  images_exported?: string;
  images_failed?: string;
  metrics_written?: boolean;
  /** Reanalysis, terminal only (decimal strings): the records written and, of
   * those, the valid ones (`valid_objects`, #763 F18). */
  objects?: string;
  valid_objects?: string;
  /** Reanalysis, completed only (#867 REVIEW-064): the px→µm factor the objects were validated at and
   * where it came from (the source file's own, the live factor when it records none, or the
   * operator's), and the E-LUT E was computed with. */
  pixel_to_micron?: number;
  pixel_to_micron_source?: "recorded" | "fallback" | "operator";
  emodulus_lut?: string;
  warnings?: string[];
  error?: string;
}

export interface ReviewExportFileResult {
  source_path: string;
  state: string;
  final_path: string;
  retained_partial_path: string;
  error: string;
  /** Present when the request named `conversion_factor_source` (#867). */
  conversion_factor?: number;
  conversion_factor_source?: "recorded" | "fallback" | "request";
  valid_count?: string;
  invalid_count?: string;
}

/** One line on what a file was exported with: its factor (and whether it is the file's
 * own), and for FCS the event counts and the event map beside it. "" for an old backend. */
export function describeExportFile(r: ReviewExportFileResult): string {
  if (r.conversion_factor === undefined || !r.conversion_factor_source) return "";
  const source = r.conversion_factor_source === "recorded" ? "recorded in the file"
    : r.conversion_factor_source === "fallback" ? "live factor: the file records none" : "requested";
  const parts = [`px→µm ${r.conversion_factor} (${source})`];
  if (/\.fcs$/i.test(r.final_path) && r.state === "completed" && r.valid_count !== undefined && r.invalid_count !== undefined) {
    const valid = Number(r.valid_count), invalid = Number(r.invalid_count);
    parts.push(`${valid + invalid} FCS 3.1 events (${valid} valid, ${invalid} invalid)`);
    parts.push(`event map ${r.final_path.replace(/\.fcs$/i, "")}_event_map.csv`);
  }
  return parts.join(" · ");
}

type DialogFilter = { name: string; extensions: string[] };
const HDF_FILES: DialogFilter[] = [{ name: "HDF5 Files", extensions: ["h5", "hdf5"] }, { name: "All Files", extensions: ["*"] }];

export interface ExportDialogPlan {
  /** Batch: the HDF files to export (Qt: getOpenFileNames). */
  files?: { title: string; filters: DialogFilter[]; defaultPath?: string };
  destination:
    | { kind: "file"; title: string; filters: DialogFilter[]; defaultPath: string; normalize: (picked: string) => string }
    | { kind: "folder"; title: string; defaultPath?: string };
}

/** The Qt tab's export dialogs (HdfReviewTab: titles, filters, default names) for MIB
 * Studio's Review stage. `lastDir` is the remembered export folder, else the open file's. */
export function exportDialogPlan(format: ReviewExportRequest["format"], batch: boolean, openFile: string, lastDir: string): ExportDialogPlan {
  const at = (name: string) => (lastDir ? joinPath(lastDir, name) : name);
  const folder = (title: string) => ({ kind: "folder" as const, title, ...(lastDir ? { defaultPath: lastDir } : {}) });
  const openDir = openFile.replace(/[/\\][^/\\]*$/, "");
  const files = (title: string) => ({ title, filters: HDF_FILES, ...(openFile && openDir !== openFile ? { defaultPath: openDir } : {}) });
  if (batch) {
    if (format === "metrics_csv") return { files: files("Select HDF Files for Metrics Export"), destination: folder("Select Directory for Metrics CSV Files") };
    if (format === "fcs") return { files: files("Select HDF Files for Batch FCS Export"), destination: folder("Select Directory for FCS Files") };
    return { files: files("Select HDF Files for Batch Export All"), destination: folder("Select Export Root Directory") };
  }
  const base = stem(openFile) || "hdf_export";
  if (format === "metrics_csv")
    return { destination: { kind: "file", title: "Export Metrics to CSV", filters: [{ name: "CSV Files", extensions: ["csv"] }, { name: "All Files", extensions: ["*"] }], defaultPath: at(`${base}_metrics.csv`), normalize: (p) => p } };
  if (format === "fcs")
    return { destination: { kind: "file", title: "Export FCS 3.1", filters: [{ name: "FCS 3.1 Files", extensions: ["fcs"] }, { name: "All Files", extensions: ["*"] }], defaultPath: at(`${base}.fcs`), normalize: withFcsExtension } };
  if (format === "charts") return { destination: folder("Select Directory to Export Charts") };
  return { destination: folder(format === "images" ? "Select Export Root Directory for Images" : "Select Export Root Directory") };
}
