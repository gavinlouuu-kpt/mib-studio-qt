// R1 Open · experiment library (#717 B1, #269; Penpot board "R1 · Open —
// experiment library", ids in docs/design/egrabber-workspace/PENPOT.md).
//
// Choose a folder, see its run files, open one in the review session, and
// read what the open run says about itself. Opening never modifies a file;
// listing a folder reads names only (`review_list_dir`). The grouping by
// experiment (run snapshot v3 identities through `review_probe_runs`, B1a)
// arrives once the workspace branch carries review ABI 2; until then the
// folder is a plain list of files.

import { useCallback, useEffect, useRef, useState } from "react";
import { Button, Card, DataTable, FactList, Notice, StateChip, Toolbar, type DataTableColumn } from "@mib/ui";
import { open } from "../../transport/dialogs";
import { baseName } from "../exports/exportHelpers";
import { reviewBridge, type ReviewInfo } from "../reviewBridge";
import type { OpenOutcome } from "../ReviewPanel";
import { completionChip, openRunFacts, runFilesIn, type FolderRun } from "./openModel";

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

/** A folder's run files, kept with the folder they were listed for. */
interface Listed {
  folder: string;
  runs: FolderRun[];
}

export function OpenView({ ready, info, onOpenPath, onOpenFile, onClose, onInspect, log }: OpenViewProps) {
  const [folder, setFolder] = useState(loadFolder);
  const [listed, setListed] = useState<Listed | null>(null);
  // The folder being listed ("" when idle).
  const [listing, setListing] = useState("");
  const [opening, setOpening] = useState("");
  const [failure, setFailure] = useState<{ name: string; message: string } | null>(null);
  // Each listing takes the next generation; an answer for an older one is
  // dropped (#747 review): listings can finish out of order.
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
      setListed({ folder: dir, runs });
      setListing("");
    },
    [log],
  );

  // The remembered folder is listed once the backend is up (names only).
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
  const columns: DataTableColumn<FolderRun>[] = [
    { key: "name", header: "File", emphasis: true },
    {
      key: "state",
      header: "Status",
      width: "160px",
      cell: (run) =>
        run.path === openPath ? (
          <StateChip tone="info" size="small">
            Open
          </StateChip>
        ) : null,
    },
    {
      key: "open",
      header: "",
      width: "96px",
      cell: (run) => (
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
    },
  ];

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
        <Card
          title="Runs in this folder"
          subtitle={folder ? `${folder} · names only; opening a run never modifies it` : "Choose a folder of runs (.h5 / .hdf5) with Open folder…"}
          status={
            busy ? (
              <StateChip tone="unknown" size="small">
                Listing…
              </StateChip>
            ) : shown ? (
              <StateChip tone="neutral" size="small">
                {`${shown.runs.length} run file${shown.runs.length === 1 ? "" : "s"}`}
              </StateChip>
            ) : null
          }
        >
          <DataTable
            columns={columns}
            rows={shown?.runs ?? []}
            rowKey={(run) => run.path}
            selectedKey={openPath || undefined}
            empty={!folder ? "No folder chosen yet." : busy ? "Listing…" : "No .h5 or .hdf5 files in this folder."}
            density="compact"
          />
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
