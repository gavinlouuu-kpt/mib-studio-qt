// Virtualised thumbnail grid (plan 2026-10-01-standalone-review-app, PR 2).
// Only the rows in view (plus overscan) are in the DOM; tiles arrive as
// packed strips of THUMB_PAGE from the backend (pull kind 5) and are cached
// in a bounded LRU, so memory stays bounded for 10⁵-frame files (the Qt
// tab's "never load all images" rule).
import { useCallback, useEffect, useLayoutEffect, useRef, useState } from "react";
import type { FramePacket } from "../framePacket";
import { REVIEW_PIXEL_FORMATS } from "./reviewContract";
import { reviewBridge, type OverlayMode } from "./reviewBridge";
import { gridGeometry, imageBand, PageCache, pagesFor, scrollToReveal, THUMB_PAGE, THUMB_SIZE, tileRgba, visibleRange } from "./thumbnails";

const GAP = 8;
const CACHE_PAGES = 12;

interface Page {
  channels: 1 | 3;
  count: number;
  data: Uint8Array;
}

export interface ThumbnailGridProps {
  /** Valid (or recording "Frames") set when true, invalid set otherwise. */
  valid: boolean;
  total: number;
  overlay: OverlayMode;
  roiOverlay: boolean;
  selected: number;
  /** Changes on every open/close so cached pages never outlive their file. */
  fileKey: string;
  /** Frame geometry of the set (cells follow its aspect ratio). */
  imageWidth: number;
  imageHeight: number;
  onSelect: (index: number) => void;
  onOpen: (index: number) => void;
  log: (line: string) => void;
}

type Band = { x: number; y: number; w: number; h: number };

function Tile(props: { page: Page | undefined; tile: number; index: number; band: Band; selected: boolean; onSelect: () => void; onOpen: () => void }) {
  const ref = useRef<HTMLCanvasElement>(null);
  const { band } = props;
  useEffect(() => {
    const canvas = ref.current;
    if (!canvas || !props.page || props.tile >= props.page.count) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    // Only the image band of the letterboxed tile (no black bars).
    ctx.putImageData(new ImageData(tileRgba(props.page.data, THUMB_SIZE, props.page.channels, props.tile), THUMB_SIZE, THUMB_SIZE), -band.x, -band.y, band.x, band.y, band.w, band.h);
  }, [props.page, props.tile, band.x, band.y, band.w, band.h]);
  return (
    <div
      className={`thumb${props.selected ? " selected" : ""}`}
      role="option"
      aria-selected={props.selected}
      title={`#${props.index + 1}`}
      onClick={props.onSelect}
      onDoubleClick={props.onOpen}
    >
      {props.page ? <canvas ref={ref} width={band.w} height={band.h} /> : <div className="thumb-pending" />}
      <span className="thumb-label">{props.index + 1}</span>
    </div>
  );
}

export function ThumbnailGrid(props: ThumbnailGridProps) {
  const { valid, total, overlay, roiOverlay, selected, fileKey, imageWidth, imageHeight, onSelect, onOpen, log } = props;
  const band = imageBand(THUMB_SIZE, imageWidth, imageHeight);
  const cellW = band.w + GAP;
  const cellH = band.h + GAP + 12; // + label line
  const scroller = useRef<HTMLDivElement>(null);
  const cache = useRef(new PageCache<Page>(CACHE_PAGES));
  const inFlight = useRef(new Set<number>());
  const generation = useRef(0);
  const [, setTick] = useState(0);
  const [viewport, setViewport] = useState({ width: 600, height: 400, scrollTop: 0 });

  // A new file, set, overlay or ROI choice invalidates every cached page.
  useEffect(() => {
    generation.current++;
    cache.current.clear();
    inFlight.current.clear();
    setTick((t) => t + 1);
  }, [fileKey, valid, overlay, roiOverlay]);

  useLayoutEffect(() => {
    const el = scroller.current;
    if (!el) return;
    const measure = () => setViewport({ width: el.clientWidth, height: el.clientHeight, scrollTop: el.scrollTop });
    measure();
    const ro = new ResizeObserver(measure);
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  const geometry = gridGeometry(total, viewport.width, cellH, cellW);
  const range = visibleRange(viewport.scrollTop, viewport.height, geometry, total);

  const fetchPage = useCallback(
    async (offset: number) => {
      if (cache.current.has(offset) || inFlight.current.has(offset)) return;
      const gen = generation.current;
      inFlight.current.add(offset);
      try {
        const count = Math.min(THUMB_PAGE, total - offset);
        const p: FramePacket = await reviewBridge.thumbnails(valid, offset, count, THUMB_SIZE, overlay, roiOverlay);
        if (gen !== generation.current || !p.valid) return;
        cache.current.set(offset, {
          channels: p.pixel_format === REVIEW_PIXEL_FORMATS.Rgb8 ? 3 : 1,
          count: Math.floor(p.height / THUMB_SIZE),
          data: p.data,
        });
        setTick((t) => t + 1);
      } catch (e) {
        if (gen === generation.current) log(`thumbnail page ${offset} error: ${e}`);
      } finally {
        inFlight.current.delete(offset);
      }
    },
    [log, overlay, roiOverlay, total, valid],
  );

  useEffect(() => {
    for (const offset of pagesFor(range.start, range.end)) void fetchPage(offset);
  }, [range.start, range.end, fetchPage]);

  // Keep the selected item visible (table clicks, prev/next in the viewer).
  useEffect(() => {
    const el = scroller.current;
    if (!el || selected < 0 || selected >= total) return;
    const top = scrollToReveal(selected, el.scrollTop, el.clientHeight, geometry);
    if (top !== null) el.scrollTop = top;
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [selected, geometry.columns]);

  const onKey = (e: React.KeyboardEvent) => {
    if (total === 0) return;
    const step: Record<string, number> = {
      ArrowRight: 1,
      ArrowLeft: -1,
      ArrowDown: geometry.columns,
      ArrowUp: -geometry.columns,
    };
    if (e.key in step) {
      e.preventDefault();
      onSelect(Math.min(total - 1, Math.max(0, (selected < 0 ? 0 : selected) + step[e.key])));
    } else if (e.key === "Enter" && selected >= 0) {
      onOpen(selected);
    }
  };

  const tiles = [];
  for (let i = range.start; i < range.end; i++) {
    const offset = Math.floor(i / THUMB_PAGE) * THUMB_PAGE;
    const row = Math.floor(i / geometry.columns);
    const col = i % geometry.columns;
    tiles.push(
      <div key={i} className="thumb-cell" style={{ top: row * cellH, left: col * cellW, width: cellW, height: cellH }}>
        <Tile
          page={cache.current.get(offset)}
          tile={i - offset}
          index={i}
          band={band}
          selected={i === selected}
          onSelect={() => onSelect(i)}
          onOpen={() => onOpen(i)}
        />
      </div>,
    );
  }

  return (
    <div
      ref={scroller}
      className="thumb-grid"
      role="listbox"
      aria-label={valid ? "Frames" : "Invalid frames"}
      tabIndex={0}
      onKeyDown={onKey}
      onScroll={(e) => {
        const el = e.currentTarget;
        setViewport((v) => (v.scrollTop === el.scrollTop ? v : { ...v, scrollTop: el.scrollTop }));
      }}
    >
      <div className="thumb-canvas" style={{ height: geometry.height }}>
        {tiles}
      </div>
      {total === 0 && <div className="thumb-empty">No images in this set.</div>}
    </div>
  );
}
