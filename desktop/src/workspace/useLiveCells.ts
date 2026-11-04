// The Live view's live cells (O4, #716 A6b): the backend's bounded strip and pins (bridge ABI 41,
// monitoring_cells), read while the view is shown, and their crops pulled as frame packets.
//
// - Reads. `fetch_monitoring_cells` once a second while the view is shown; the backend refreshes its
//   strip on that read. Images come by identity (`fetch_monitoring_cell_packet`, pull kinds 5 and 6),
//   at most two at a time, only for the cells shown (the newest STRIP_SHOWN strip cells and the pins)
//   and only once per cell: a crop never changes. A cell that leaves the listing is forgotten.
// - Freeze display holds the strip shown (no new crops are pulled for it); the pins keep updating.
//   A listing of another capture or another latest run drops the held strip and its crops even while
//   frozen (the frozen state then says the capture changed): an earlier capture's cell is never shown
//   as live or pinned.
// - Pins are shared by every client of the instrument, so pin, unpin and clear are control commands
//   (VIEWER_ONLY on the WebSocket server): without control nothing is sent and the view says why.
//   A pin is sent only on a click, and only for an identity of the listing's capture and of the latest
//   run (or of none): any other is refused before anything is sent. The reply's outcome decides what
//   the view says.
// - "Clear ended pins" is one unpin per ended pin. A control loss cancels the batch: no further unpin
//   is sent, and it never resumes by itself (control coming back does not restart it).
// - A notice that describes a pin ("Pinned · F…") is withdrawn once that pin has ended or gone
//   (its capture ended, or it was unpinned); a notice of an action's outcome stays until the next.
// - A cell of an earlier capture is refused (`CAPTURE_MISMATCH` for an image pull, `CaptureMismatch`
//   for a pin): the view says so and reads the listing again, and never retries the same identity.
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { bridge } from "../bridge";
import type { FramePacket } from "../framePacket";
import {
  cellKeyString,
  monitoringCellCaptureMismatch,
  type MonitoringCell,
  type MonitoringCellKeyInput,
  type MonitoringCellsListing,
} from "../monitoringCells";
import { cellName, endedPins, frozenChangeText, listingScope, pinIdentityRefusal, pinNotice, pinNoticeStale, pinNoticeSubject, scopeChanged, stripShown,
  type CellScope, type LiveNotice, type PinNoticeSubject } from "./liveModel";

export type CellLayer = "image" | "mask";

/** A cell's crop as the view holds it. The image: loading until `src` (or `reason` when the backend
 *  no longer serves it, or it cannot be drawn). The mask likewise, with `maskSrc` / `maskReason`. */
export interface CellPicture {
  src?: string;
  reason?: string;
  maskSrc?: string;
  maskReason?: string;
}

/** Draws a crop or mask packet as an image URL; null when it cannot be drawn. */
export type PacketToUrl = (packet: FramePacket, layer: CellLayer) => string | null;

export interface LiveCellsOptions {
  /** The Live view is shown: read, else nothing is read. */
  active: boolean;
  controlHeld: boolean;
  /** Freeze display: the strip shown is held. */
  frozen: boolean;
  /** Pull the masks too (the Mask overlay is shown). */
  masks: boolean;
  toUrl: PacketToUrl;
  intervalMs?: number;
}

export interface LiveCells {
  /** The last listing read; null before the first, or while the store is unavailable. */
  listing: MonitoringCellsListing | null;
  /** Why the last read failed or the store is unavailable; "" when it answered. */
  readError: string;
  /** The strip as shown: newest first, held while frozen (empty once the capture changed). */
  strip: MonitoringCell[];
  /** Frozen, and the capture or run changed since: why the held strip was dropped; "" otherwise. */
  frozenChange: string;
  pictures: ReadonlyMap<string, CellPicture>;
  /** The last pin, unpin or clear, in plain words. */
  notice: LiveNotice | null;
  /** A pin command is on its way. */
  busy: boolean;
  /** The key of the last cell pinned (or found already pinned), to select it. */
  lastPinned: string | null;
  pin: (key: MonitoringCellKeyInput) => Promise<void>;
  unpin: (key: MonitoringCellKeyInput) => Promise<void>;
  clearEnded: () => Promise<void>;
  clearAll: () => Promise<void>;
  /** Says something that sent nothing (e.g. a point that cannot be pinned). */
  inform: (notice: LiveNotice) => void;
  dismiss: () => void;
}

export const VIEWER_PIN_REASON = "Pins are shared by every client of this instrument, so only the controlling client pins, unpins or clears them.";
const MAX_PULLS = 2;
const plain = (e: unknown) => String(e).replace(/^Error: /, "");

type Wanted = Map<string, { cell: MonitoringCell; mask: boolean }>;

export function useLiveCells({ active, controlHeld, frozen, masks, toUrl, intervalMs = 1000 }: LiveCellsOptions): LiveCells {
  const [listing, setListing] = useState<MonitoringCellsListing | null>(null);
  const [readError, setReadError] = useState("");
  /** The strip held by Freeze display, with the capture and run it was of (null before any listing). */
  const [held, setHeld] = useState<{ cells: MonitoringCell[]; scope: CellScope | null } | null>(null);
  const [pictures, setPictures] = useState<ReadonlyMap<string, CellPicture>>(() => new Map());
  // The last notice, and the pin it describes (null for an action's outcome): see pinNoticeStale.
  const [said, setSaid] = useState<{ notice: LiveNotice; about: PinNoticeSubject | null } | null>(null);
  const setNotice = useCallback((n: LiveNotice | null, about: PinNoticeSubject | null = null) => setSaid(n ? { notice: n, about } : null), []);
  const notice = said?.notice ?? null;
  const [busy, setBusy] = useState(false);
  const [lastPinned, setLastPinned] = useState<string | null>(null);
  const busyRef = useRef(false);
  const controlRef = useRef(controlHeld);
  controlRef.current = controlHeld;
  // Bumped on every control loss: a batch of commands started under an earlier value is cancelled
  // (it never resumes, even when control comes back before it would have sent the next command).
  const controlLosses = useRef(0);
  const controlSeen = useRef(controlHeld);
  if (controlSeen.current !== controlHeld) {
    controlSeen.current = controlHeld;
    if (!controlHeld) controlLosses.current += 1;
  }
  const toUrlRef = useRef(toUrl);
  toUrlRef.current = toUrl;
  const listingRef = useRef(listing);
  listingRef.current = listing;
  const mounted = useRef(true);
  useEffect(() => { mounted.current = true; return () => { mounted.current = false; }; }, []);

  // ---- the listing ----
  const reading = useRef(false);
  const read = useCallback(async () => {
    if (reading.current) return;
    reading.current = true;
    try {
      const reply = await bridge.fetchMonitoringCells();
      if (!mounted.current) return;
      if (!reply.valid) { setListing(null); setReadError("The live cell store is not available yet: the backend is not initialized."); return; }
      setReadError("");
      // The same revision of the same capture is the same listing: keep it, so nothing re-renders.
      setListing((prev) => (prev && prev.revision === reply.revision && prev.ring_capture_epoch === reply.ring_capture_epoch ? prev : reply));
    } catch (e) {
      if (mounted.current) setReadError(`The live cells could not be read: ${plain(e)}`);
    } finally {
      reading.current = false;
    }
  }, []);
  useEffect(() => {
    if (!active) return;
    void read();
    const id = window.setInterval(() => void read(), intervalMs);
    return () => window.clearInterval(id);
  }, [active, intervalMs, read]);

  // Freeze display holds the strip shown when it was pressed, for as long as the listing is of the
  // same capture and run: a change drops it (and so its crops), even while frozen.
  useEffect(() => {
    const l = listingRef.current;
    setHeld(frozen ? { cells: stripShown(l), scope: l ? listingScope(l) : null } : null);
  }, [frozen]);
  const heldChanged = !!(held?.scope && listing && scopeChanged(held.scope, listing));
  const strip = useMemo(() => (held ? (heldChanged ? [] : held.cells) : stripShown(listing)), [held, heldChanged, listing]);
  const frozenChange = held?.scope && listing && heldChanged ? frozenChangeText(held.scope, listing) : "";
  const pins = listing?.pins;
  // "Pinned · F…" goes once its pin has ended or gone (#763 F13).
  useEffect(() => {
    if (said?.about && pinNoticeStale(said.about, listing)) setSaid((cur) => (cur === said ? null : cur));
  }, [said, listing]);

  // ---- the crops: pulled once per cell, two at a time ----
  const inFlight = useRef(new Set<string>());
  const picturesRef = useRef(pictures);
  const wanted = useRef<Wanted>(new Map());
  const update = useCallback((id: string, change: (p: CellPicture) => CellPicture) => {
    if (!mounted.current || !wanted.current.has(id)) return; // forgotten meanwhile
    const next = new Map(picturesRef.current);
    next.set(id, change(next.get(id) ?? {}));
    picturesRef.current = next;
    setPictures(next);
  }, []);
  const pump = useCallback(() => {
    for (const [id, { cell, mask }] of wanted.current) {
      for (const layer of mask ? (["image", "mask"] as const) : (["image"] as const)) {
        const job = `${id}|${layer}`;
        const have = picturesRef.current.get(id) ?? {};
        const settled = layer === "image" ? have.src !== undefined || have.reason !== undefined : have.maskSrc !== undefined || have.maskReason !== undefined;
        if (settled || inFlight.current.has(job)) continue;
        if (inFlight.current.size >= MAX_PULLS) return;
        inFlight.current.add(job);
        const done = (p: CellPicture, src: string | undefined, reason: string | undefined): CellPicture =>
          layer === "image" ? { ...p, src, reason } : { ...p, maskSrc: src, maskReason: reason };
        void bridge.fetchMonitoringCellImage(cell, layer).then((packet) => {
          if (!packet.valid) return update(id, (p) => done(p, undefined, "The backend no longer holds this crop: the listing says why."));
          const url = toUrlRef.current(packet, layer);
          update(id, (p) => (url === null ? done(p, undefined, "This view cannot draw the crop (no 2D canvas).") : done(p, url, undefined)));
        }, (e) => {
          const mismatch = monitoringCellCaptureMismatch(e);
          update(id, (p) => done(p, undefined, mismatch !== null ? `Not served: ${mismatch}` : `The crop could not be read: ${plain(e)}`));
          if (mismatch !== null) void read();
        }).finally(() => {
          inFlight.current.delete(job);
          if (mounted.current) pump();
        });
      }
    }
  }, [read, update]);

  useEffect(() => {
    const next: Wanted = new Map();
    const want = (cell: MonitoringCell) => {
      if (cell.image !== null) next.set(cellKeyString(cell), { cell, mask: masks && cell.image.has_mask });
    };
    strip.forEach(want);
    pins?.forEach(want);
    wanted.current = next;
    const kept = new Map<string, CellPicture>();
    for (const id of next.keys()) {
      const p = picturesRef.current.get(id);
      if (p) kept.set(id, p);
    }
    picturesRef.current = kept;
    setPictures(kept);
    pump();
  }, [strip, pins, masks, pump]);

  // ---- the pins: control commands, on a click only ----
  const guard = useCallback((): boolean => {
    if (busyRef.current) return false;
    if (!controlRef.current) {
      setNotice({ tone: "blocked", title: "Not sent · this client does not control the instrument", text: VIEWER_PIN_REASON });
      return false;
    }
    busyRef.current = true;
    setBusy(true);
    return true;
  }, []);
  const release = useCallback(() => { busyRef.current = false; if (mounted.current) setBusy(false); }, []);

  const pin = useCallback(async (key: MonitoringCellKeyInput) => {
    if (!guard()) return;
    const name = cellName(key);
    const refusal = pinIdentityRefusal(key, listingRef.current);
    if (refusal) {
      setNotice({ tone: "warn", title: `Not sent · ${name} is not of the current capture`, text: refusal });
      release();
      return;
    }
    try {
      const reply = await bridge.monitoringCellPin(key);
      if (!mounted.current) return;
      setNotice(pinNotice(name, reply), pinNoticeSubject(reply));
      if (reply.cell) setLastPinned(cellKeyString(reply.cell));
    } catch (e) {
      if (mounted.current) setNotice({ tone: "blocked", title: `Not pinned · ${name}`, text: plain(e) });
    } finally {
      release();
      await read();
    }
  }, [guard, release, read, setNotice]);

  const unpin = useCallback(async (key: MonitoringCellKeyInput) => {
    if (!guard()) return;
    const name = cellName(key);
    try {
      const r = await bridge.monitoringCellUnpin(key);
      if (mounted.current) setNotice(r.ok ? { tone: "info", title: `Unpinned · ${name}`, text: "The backend released its copy." } : { tone: "warn", title: `Not unpinned · ${name}`, text: r.message });
    } catch (e) {
      if (mounted.current) setNotice({ tone: "blocked", title: `Not unpinned · ${name}`, text: plain(e) });
    } finally {
      release();
      await read();
    }
  }, [guard, release, read]);

  /** One unpin per ended pin (there is no backend command for "ended only"). Control is checked
   *  before every unpin; a control loss since the batch began cancels the rest. */
  const clearEnded = useCallback(async () => {
    const ended = endedPins(listingRef.current);
    if (!ended.length || !guard()) return;
    const losses = controlLosses.current;
    const ours = () => mounted.current && controlRef.current && controlLosses.current === losses;
    let cleared = 0, cancelled = false;
    const failures: string[] = [];
    try {
      for (const p of ended) {
        if (!ours()) { cancelled = true; break; }
        try {
          const r = await bridge.monitoringCellUnpin(p);
          if (r.ok) cleared += 1; else failures.push(`${cellName(p)}: ${r.message}`);
        } catch (e) {
          failures.push(`${cellName(p)}: ${plain(e)}`);
        }
      }
      if (mounted.current) setNotice(cancelled
        ? { tone: "warn", title: `Stopped · cleared ${cleared} of ${ended.length} ended pins`, text: `This client lost control, so no further unpin was sent, and the batch does not resume by itself. With control, press Clear ended pins again.${failures.length ? ` ${failures.join("; ")}` : ""}` }
        : failures.length
        ? { tone: "warn", title: `Cleared ${cleared} of ${ended.length} ended pins`, text: failures.join("; ") }
        : { tone: "info", title: `Cleared ${cleared} ended ${cleared === 1 ? "pin" : "pins"}`, text: "Pins of the current capture are kept." });
    } finally {
      release();
      await read();
    }
  }, [guard, release, read]);

  const clearAll = useCallback(async () => {
    if (!listingRef.current?.pins.length || !guard()) return;
    try {
      const r = await bridge.monitoringCellClearPins();
      if (mounted.current) setNotice(r.ok ? { tone: "info", title: "Pins cleared", text: r.message } : { tone: "warn", title: "Pins not cleared", text: r.message });
    } catch (e) {
      if (mounted.current) setNotice({ tone: "blocked", title: "Pins not cleared", text: plain(e) });
    } finally {
      release();
      await read();
    }
  }, [guard, release, read]);

  const inform = useCallback((n: LiveNotice) => setNotice(n), [setNotice]);
  const dismiss = useCallback(() => setNotice(null), [setNotice]);
  return { listing, readError, strip, frozenChange, pictures, notice, busy, lastPinned, pin, unpin, clearEnded, clearAll, inform, dismiss };
}
