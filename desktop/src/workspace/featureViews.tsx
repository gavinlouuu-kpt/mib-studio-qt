// Align's GenApi pieces (#806 E-3, goal E02): the draft and readback cells of an SFNC-bound quick
// control, and the read-only feature tree browser. They lay out what camera_feature_tree and
// camera_feature_write returned; every fact and outcome is the backend's.
import { useMemo } from "react";
import { Button, Card, DataTable, StateChip, type DataTableColumn } from "@mib/ui";
import type { CameraFeatureReply, CameraFeatureTreeReply } from "../bridge";
import {
  accessText,
  draftOf,
  encodeDraft,
  rangeText,
  treeLines,
  valueText,
  writeSummary,
  type Resolution,
  type TreeLine,
} from "./cameraFeatures";

export interface BindingDraftProps {
  id: string;
  label: string;
  resolution: Resolution;
  /** The operator's text; null while the draft follows the tree's value. */
  draft: string | null;
  pending: boolean;
  lockedReason: string;
  onDraft: (text: string | null) => void;
  onCommit: (text: string) => void;
}

/** The Requested cell: a draft and its Write, only for a node the tree reports writable now. */
export function BindingDraft(p: BindingDraftProps) {
  const r = p.resolution;
  if (r.state !== "writable") return <span className="ws-detail" title="Not writable here">—</span>;
  const d = r.descriptor;
  const text = p.draft ?? draftOf(d);
  const disabled = p.pending || !!p.lockedReason;
  const encoded = encodeDraft(d, text);
  const choices = d.kind === "enumeration" ? d.entries : d.kind === "boolean" ? ["true", "false"] : null;
  return (
    <span className="ws-roi" data-testid={`align-draft-${p.id}`}>
      {choices ? (
        <select aria-label={`${p.label} requested`} value={text} disabled={disabled} onChange={(e) => p.onDraft(e.target.value)}>
          {choices.includes(text) ? null : <option value={text}>{text || "—"}</option>}
          {choices.map((c) => <option key={c} value={c}>{c}</option>)}
        </select>
      ) : (
        <input
          type="text"
          inputMode={d.kind === "string" ? "text" : "decimal"}
          aria-label={`${p.label} requested`}
          value={text}
          disabled={disabled}
          onChange={(e) => p.onDraft(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === "Enter" && encoded.ok) p.onCommit(text);
            if (e.key === "Escape") p.onDraft(null);
          }}
        />
      )}
      {d.unit ? <span className="ws-detail">{d.unit}</span> : null}
      <Button size="small" disabled={disabled || !encoded.ok} title={encoded.ok ? p.lockedReason || undefined : encoded.reason}
        aria-label={`Write ${p.label}`} onClick={() => p.onCommit(text)}>
        {p.pending ? "Writing…" : "Write"}
      </Button>
    </span>
  );
}

/** The Readback cell: the tree's value and range, then the last write's reply as returned. */
export function BindingReadback({ id, resolution: r, reply, fallback }: {
  id: string; resolution: Resolution; reply: CameraFeatureReply | undefined;
  /** A camera report from outside the node map (the geometry session), shown only without one. */
  fallback: string | null;
}) {
  const d = r.state === "writable" || r.state === "readOnly" ? r.descriptor : null;
  const w = reply ? writeSummary(reply, d?.unit ?? "") : null;
  return (
    <>
      {d ? valueText(d.value, d.unit) : fallback ? fallback : "—"}
      {d && rangeText(d) ? <span className="ws-detail">{rangeText(d)}</span> : null}
      {!d && fallback ? <span className="ws-detail">camera report, not from the node map</span> : null}
      {w ? (
        <span className="ws-detail" data-testid={`align-write-${id}`} data-tone={w.tone} title={w.reason || undefined}>
          {`${w.label} · requested ${w.requested} · ${w.readback}${w.reason ? ` · ${w.reason}` : ""}`}
        </span>
      ) : null}
    </>
  );
}

/** The read-only feature tree of the camera, as camera_feature_tree enumerated it. */
export function FeatureTreeBrowser({ tree, loading, onRefresh }: { tree: CameraFeatureTreeReply | null; loading: boolean; onRefresh: () => void }) {
  const module = tree?.outcome === "read" ? tree.modules.find((m) => m.module === "remote") : undefined;
  const lines = useMemo(() => (module?.enumerated ? treeLines(module) : []), [module]);
  const columns: DataTableColumn<TreeLine>[] = [
    {
      key: "name", header: "Category / feature", width: "34%", cell: (l) => (
        <span style={{ paddingLeft: `${l.depth * 1.2}em` }} className={l.kind === "category" ? "ws-tree__category" : undefined}
          title={l.displayName && l.displayName !== l.name ? l.displayName : undefined}>
          {l.kind === "category" ? `▸ ${l.name}` : l.name}
        </span>
      ),
    },
    { key: "type", header: "Type", width: "12%", cell: (l) => (l.kind === "category" ? "" : l.descriptor?.kind ?? "—") },
    { key: "access", header: "Access", width: "14%", cell: (l) => (l.kind === "category" ? "" : accessText(l.descriptor)) },
    { key: "value", header: "Value", width: "16%", cell: (l) => (l.kind === "category" ? "" : l.descriptor ? valueText(l.descriptor.value, l.descriptor.unit) : "—") },
    {
      key: "range", header: "Range", width: "24%", cell: (l) =>
        l.error ? <span className="ws-detail">{l.error}</span> : l.kind === "category" || !l.descriptor ? "" : rangeText(l.descriptor),
    },
  ];
  const status = !tree ? "Not read yet"
    : tree.outcome !== "read" ? `${tree.outcome} · ${tree.reason || "no reason given"}`
      : !module ? "The remote module was not enumerated"
        : !module.enumerated ? `Not enumerated · ${module.reason}`
          : `${lines.filter((l) => l.kind === "feature").length} features${module.truncated ? " · truncated at the backend's limit" : ""}`;
  return (
    <Card
      className="ws-align__tree"
      title="Feature tree · camera (remote device)"
      subtitle="Read-only · categories and features as the camera's GenApi node map describes them"
      data-testid="align-feature-tree"
      status={<StateChip size="small" tone={tree?.outcome === "read" ? "neutral" : "unknown"} data-testid="align-feature-tree-status">
        {loading ? "Reading…" : status}
      </StateChip>}
      actions={<Button size="small" disabled={loading} onClick={onRefresh}>Refresh</Button>}
    >
      {tree?.outcome === "read" ? (
        <p className="ws-note">Evidence: {tree.evidence || "not stated"} · source {tree.session?.source ?? "—"} · generation {tree.session?.generation ?? "—"}</p>
      ) : null}
      {lines.length ? <DataTable columns={columns} rows={lines} rowKey={(l) => l.key} density="compact" /> : null}
    </Card>
  );
}
