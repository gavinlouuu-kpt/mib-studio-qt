// R1 Open · experiment library (#717 B1, #269; Penpot board "R1 · Open —
// experiment library", ids in docs/design/egrabber-workspace/PENPOT.md).
//
// Choose a folder, see its runs grouped by experiment, open one in the review
// session, and read what the open run says about itself. Opening never
// modifies a file. Listing a folder reads names (`review_list_dir`); the
// run identities come from the files' headers (`review_probe_runs`, review
// ABI 2), in calls of at most REVIEW_PROBE.max_paths files. The kit has no
// grouped table rows yet (#715): each group is its own DataTable with a
// caption.

import { useCallback, useEffect, useRef, useState } from "react";
import { Button, Card, DataTable, FactList, Notice, StateChip, Toolbar, type DataTableColumn } from "@mib/ui";
import type { ReactNode } from "react";
import { open } from "../../transport/dialogs";
import { baseName } from "../exports/exportHelpers";
import { reviewBridge, type ReviewInfo, type ReviewProbeResult, type ReviewRunProbe } from "../reviewBridge";
import type { OpenOutcome } from "../ReviewPanel";
import {
  conditionLabel,
  groupRuns,
  groupSummary,
  probeChip,
  probeChunks,
  recordsLabel,
  runLabel,
  sampleLabel,
  snapshotLabel,
  type LibraryRun,
  type RunGroup,
} from "./libraryModel";
import { completionChip, openRunFacts, recordedTime, runFilesIn, type FolderRun } from "./openModel";

/** Remembered library folder (per machine). */
export const LIBRARY_FOLDER_KEY = "yofo.review.libraryFolder";

function loadFolder(): string {
  try {
    return localStorage.getItem(LIBRARY_FOLDER_KEY) ?? "";
  } catch {
    return "";
  }
}

function saveFolder(folder: string) {
  try {
    localStorage.setItem(LIBRARY_FOLDER_KEY, folder);
  } catch {
    // Storage unavailable: the folder is remembered for this session only.
  }
}

export interface OpenViewProps {
  /** Backend initialized (opening is refused otherwise). */
  ready: boolean;
  /** The open file's info (null when none). */
  info: ReviewInfo | null;
  /** Open a run in the review session (the panel's openPath). */
  onOpenPath: (path: string) => Promise<OpenOutcome>;
  /** Choose one file with the OS dialog and open it. */
  onOpenFile: () => Promise<void>;
  /** Close the open file. */
  onClose: () => Promise<void>;
  /** Go to Inspect (frames and charts of the open run). */
  onInspect: () => void;
  log: (line: string) => void;
}

/** A folder's run files and what their headers say, kept with the folder
 * they were listed for. */
interface Listed {
  folder: string;
  runs: FolderRun[];
  /** Probe results by path; a file without one is not read (yet). */
  probes: ReadonlyMap<string, ReviewRunProbe>;
  /** Files whose probe call has answered. */
  probed: number;
  /** Probe calls still to come. */
  probing: boolean;
  /** The probe was refused or failed: why. The files not read stay listed. */
  probeError: string | null;
}

const NOT_RECORDED = "Not recorded";

function GroupCaption({ group, probing }: { group: RunGroup; probing: boolean }) {
  const s = groupSummary(group);
  let title: ReactNode;
  let detail: ReactNode;
  switch (group.kind) {
    case "experiment":
      title = `Experiment ${group.experimentId}`;
      detail = [
        s.runs,
        s.dates ?? "start not recorded",
        `conditions: ${s.conditions ?? NOT_RECORDED.toLowerCase()}`,
        `core: ${s.cores ?? NOT_RECORDED.toLowerCase()}`,
        `calibration: ${s.calibrations ?? NOT_RECORDED.toLowerCase()}`,
      ].join(" · ");
      break;
    case "no-experiment":
      title = "Runs without an experiment";
      detail = "A run snapshot that declares no experiment (snapshot v1, a v3 snapshot without an experiment id, or one that cannot be read).";
      break;
    case "legacy":
      title = "Legacy files · no run snapshot";
      detail = "Recorded before run identity: experiment, run, sample, condition and calibration are not recorded. Each file is a single run.";
      break;
    case "unreadable":
      title = "Cannot be read";
      detail = "The file's header could not be read; each row says why.";
      break;
    case "unprobed":
      title = probing ? "Reading run identities…" : "Not read";
      detail = probing ? "These files are listed; their headers are being read." : "These files are listed by name; their headers were not read.";
      break;
  }
  const chip = group.kind === "experiment" || group.kind === "no-experiment" || group.kind === "legacy" ? s.status : null;
  return (
    <span className="rv-group__caption">
      <span className="rv-group__head">
        <span className="rv-group__title">{title}</span>
        {chip ? (
          <StateChip tone={chip.tone} size="small">
            {chip.label}
          </StateChip>
        ) : null}
      </span>
      <span className="rv-group__detail">{detail}</span>
    </span>
  );
}

export function OpenView({ ready, info, onOpenPath, onOpenFile, onClose, onInspect, log }: OpenViewProps) {
  const [folder, setFolder] = useState(loadFolder);
  const [listed, setListed] = useState<Listed | null>(null);
  // The folder whose names are being listed ("" when idle).
  const [listing, setListing] = useState("");
  const [opening, setOpening] = useState("");
  const [failure, setFailure] = useState<{ name: string; message: string } | null>(null);
  // Each listing takes the next generation; a listing's or a probe's answer
  // for an older one is dropped (#747 review): answers can arrive out of
  // order, and a stale probe must never fill a newer folder.
  const generation = useRef(0);
  // The current folder and listing for openRun, which may run from a render
  // that is already stale; set where the state changes.
  const current = useRef({ folder, listing: "" });

  const list = useCallback(
    async (dir: string) => {
      if (!dir) return;
      const mine = ++generation.current;
      current.current = { folder: dir, listing: dir };
      setListing(dir);
      // Another folder's rows go at once, so none stays selectable under
      // this folder's heading; a refresh keeps them, disabled, until it answers.
      setListed((prev) => (prev && prev.folder === dir ? prev : null));
      let runs: FolderRun[] = [];
      try {
        runs = runFilesIn(dir, await reviewBridge.listDir(dir));
      } catch (e) {
        if (mine === generation.current) log(`folder listing error: ${e}`);
      }
      if (mine !== generation.current) return; // superseded by a later listing
      current.current = { folder: dir, listing: "" };
      let state: Listed = { folder: dir, runs, probes: new Map(), probed: 0, probing: runs.length > 0, probeError: null };
      setListed(state);
      setListing("");
      // The headers, in calls of at most REVIEW_PROBE.max_paths files.
      for (const chunk of probeChunks(runs.map((r) => r.path))) {
        let result: ReviewProbeResult;
        try {
          result = await reviewBridge.probeRuns(chunk);
        } catch (e) {
          result = { ok: false, message: String(e), runs: [] };
        }
        if (mine !== generation.current) return; // a newer listing owns the view
        if (!result.ok) {
          log(`run identity probe refused: ${result.message}`);
          state = { ...state, probing: false, probeError: result.message || "The files' headers could not be read." };
          setListed(state);
          return;
        }
        const probes = new Map(state.probes);
        const asked = new Set(chunk);
        for (const p of result.runs) if (asked.has(p.path)) probes.set(p.path, p);
        state = { ...state, probes, probed: state.probed + chunk.length };
        setListed(state);
      }
      state = { ...state, probing: false };
      setListed(state);
    },
    [log],
  );

  // The remembered folder is listed once the backend is up.
  useEffect(() => {
    if (ready && folder) void list(folder);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ready]);

  const chooseFolder = useCallback(async () => {
    const picked = await open({ title: "Open folder of runs", directory: true, multiple: false, defaultPath: folder || undefined });
    if (typeof picked !== "string" || !picked) return;
    setFolder(picked);
    saveFolder(picked);
    setFailure(null);
    await list(picked);
  }, [folder, list]);

  const openRun = useCallback(
    async (run: FolderRun, from: string) => {
      // A row from another folder, or one shown while its folder is listed
      // again, opens nothing (the buttons are disabled then as well).
      if (from !== current.current.folder || current.current.listing !== "") return;
      setOpening(run.path);
      setFailure(null);
      try {
        const outcome = await onOpenPath(run.path);
        if (!outcome.ok) setFailure({ name: run.name, message: outcome.message });
      } finally {
        setOpening("");
      }
    },
    [onOpenPath],
  );

  const openPath = info?.file_open ? info.file_path : "";
  const shown = listed && listed.folder === folder ? listed : null;
  const busy = listing !== "";
  const groups = shown ? groupRuns(shown.runs, shown.probes) : [];

  const openColumn: DataTableColumn<LibraryRun> = {
    key: "open",
    header: "",
    width: "96px",
    cell: ({ run }) => (
      <Button
        size="small"
        variant={run.path === openPath ? "selected" : "secondary"}
        disabled={!ready || busy || opening !== "" || run.path === openPath}
        aria-label={`Open ${run.name}`}
        onClick={() => {
          if (shown) void openRun(run, shown.folder);
        }}
      >
        {opening === run.path ? "Opening…" : run.path === openPath ? "Opened" : "Open"}
      </Button>
    ),
  };
  const fileColumn: DataTableColumn<LibraryRun> = { key: "file", header: "File", emphasis: true, cell: ({ run }) => run.name };
  const probedColumns: DataTableColumn<LibraryRun>[] = [
    { key: "sample", header: "Sample", cell: ({ probe }) => sampleLabel(probe?.identity) },
    { key: "condition", header: "Condition", cell: ({ probe }) => conditionLabel(probe?.identity) },
    { key: "started", header: "Started", cell: ({ probe }) => recordedTime(probe?.start_time_ns) },
    { key: "records", header: "Records", cell: ({ probe }) => (probe ? recordsLabel(probe) : null) },
    { key: "core", header: "Core", cell: ({ probe }) => probe?.core_version ?? null },
    { key: "calibration", header: "Calibration", cell: ({ probe }) => probe?.identity.calibration_record_id ?? null },
    {
      key: "status",
      header: "Status",
      cell: ({ probe }) => {
        if (!probe) return null;
        const chip = probeChip(probe);
        return (
          <StateChip tone={chip.tone} size="small">
            {chip.label}
          </StateChip>
        );
      },
    },
  ];
  const columnsFor = (group: RunGroup): DataTableColumn<LibraryRun>[] => {
    switch (group.kind) {
      case "experiment":
        return [{ key: "run", header: "Run", width: "72px", cell: ({ probe }) => runLabel(probe?.identity) }, fileColumn, ...probedColumns, openColumn];
      case "no-experiment":
        return [
          {
            key: "snapshot",
            header: "Snapshot",
            cell: ({ probe }) => {
              const s = snapshotLabel(probe?.identity);
              return s.tone ? (
                <StateChip tone={s.tone} size="small">
                  {s.label}
                </StateChip>
              ) : (
                s.label
              );
            },
          },
          fileColumn,
          ...probedColumns,
          openColumn,
        ];
      case "legacy":
        return [fileColumn, ...probedColumns, openColumn];
      case "unreadable":
        return [fileColumn, { key: "problem", header: "Why it cannot be read", cell: ({ probe }) => probe?.message || null }, openColumn];
      case "unprobed":
        return [
          fileColumn,
          {
            key: "state",
            header: "Status",
            cell: () => (
              <StateChip tone="unknown" size="small">
                {shown?.probing ? "Reading…" : "Not read"}
              </StateChip>
            ),
          },
          openColumn,
        ];
    }
  };

  const experiments = groups.filter((g) => g.kind === "experiment").length;
  const files = shown?.runs.length ?? 0;
  const chip = info?.file_open ? completionChip(info.has_accounting, info.accounting_completion) : null;

  return (
    <div className="rv-view" data-testid="review-open">
      <Toolbar title="Open">
        <Button onClick={() => void chooseFolder()} disabled={!ready}>
          Open folder…
        </Button>
        <Button onClick={() => void onOpenFile()} disabled={!ready}>
          Open file…
        </Button>
        <Button onClick={() => void list(folder)} disabled={!ready || !folder || busy}>
          Refresh
        </Button>
        <Button onClick={() => void onClose()} disabled={!info?.file_open}>
          Close file
        </Button>
      </Toolbar>
      <div className="rv-body">
        {!ready && (
          <Notice tone="unknown" title="Backend · not initialized">
            Files can be listed and opened once the review backend is up.
          </Notice>
        )}
        {failure && (
          <Notice tone="blocked" title={`Not opened · ${failure.name}`}>
            {failure.message || "The file could not be opened."}
          </Notice>
        )}
        {shown?.probeError && (
          <Notice tone="warn" title="Run identities · not read">
            {shown.probeError} The files are still listed: open one to read it, or Refresh to try again.
          </Notice>
        )}
        <Card
          title="Experiments in this folder"
          subtitle={
            folder
              ? `${folder} · grouped by the experiment each file declares (run snapshot v3); opening a run never modifies it`
              : "Choose a folder of runs (.h5 / .hdf5) with Open folder…"
          }
          status={
            busy ? (
              <StateChip tone="unknown" size="small">
                Listing…
              </StateChip>
            ) : shown?.probing ? (
              <StateChip tone="unknown" size="small">
                {`Reading ${shown.probed} of ${files} files`}
              </StateChip>
            ) : shown ? (
              <StateChip tone="neutral" size="small">
                {`${experiments} experiment${experiments === 1 ? "" : "s"} · ${files} run file${files === 1 ? "" : "s"}`}
              </StateChip>
            ) : null
          }
        >
          {groups.length === 0 ? (
            <DataTable
              columns={[fileColumn, openColumn]}
              rows={[]}
              rowKey={({ run }) => run.path}
              empty={!folder ? "No folder chosen yet." : busy ? "Listing…" : "No .h5 or .hdf5 files in this folder."}
              density="compact"
            />
          ) : (
            <div className="rv-groups">
              {groups.map((group) => (
                <DataTable
                  key={group.key}
                  className="rv-group"
                  data-group={group.kind}
                  caption={<GroupCaption group={group} probing={!!shown?.probing} />}
                  columns={columnsFor(group)}
                  rows={group.runs}
                  rowKey={({ run }) => run.path}
                  selectedKey={openPath || undefined}
                  missing={NOT_RECORDED}
                  density="compact"
                />
              ))}
              <p className="rv-note">
                Experiment names, methods and analysis plans live in the workspace store, which YOFO Review does not read: an experiment shows the id its
                files declare.
              </p>
            </div>
          )}
        </Card>
        <Card
          title="Open run"
          subtitle={info?.file_open ? "What the file records about itself; nothing is computed here" : "No run is open. Open one from the folder, or with Open file…"}
          status={chip ? <StateChip tone={chip.tone}>{chip.label}</StateChip> : null}
          actions={
            info?.file_open ? (
              <Button variant="primary" onClick={onInspect}>
                Inspect {baseName(info.file_path)}
              </Button>
            ) : null
          }
        >
          {info?.file_open ? <FactList facts={openRunFacts(info)} /> : null}
        </Card>
      </div>
    </div>
  );
}
