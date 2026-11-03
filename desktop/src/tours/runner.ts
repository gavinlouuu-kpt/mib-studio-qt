// Guided tours: the Driver.js wrapper. Generic: the app gives a host (views, the flowchart panel, named conditions it can
// confirm from backend state, and when actions are not allowed). Driver.js is loaded only when a tour starts.
//
// Rules the runner keeps:
// - A tour never sends a command and never changes a setting. It shows views (host.navigate) and, in an action step,
//   lets the operator use the real control and waits for the host to confirm the result (host.condition).
// - Every step has Back, Next, Skip and Restart; Esc, the close button or a click outside ends the tour.
// - A step whose target is missing never points at nothing: it says why, centred, and offers Skip or Restart.
// - The last step is the feedback checkpoint (clear / unclear and a note), kept in this browser only.
import type { Config, DriveStep, PopoverDOM } from "driver.js";
import type { FeedbackStore, Verdict } from "./feedback";
import { EVIDENCE_TARGET, flowTarget, type CanvasRegion, type TourAction, type TourEntry, type TourStep, type TourView } from "./registry";

export type ConditionState = "pending" | "met" | { failed: string };

export interface RunnerHost {
  navigate(view: TourView): void;
  currentView(): TourView;
  /** Open the entry's flowchart panel (null: close it). */
  showFlow(entry: TourEntry<unknown> | null): void;
  /** A named condition the host confirms from backend state (e.g. "camera-running"). */
  condition(name: string): ConditionState;
  /** Why action steps are not allowed now (an experiment is running, something records), or null. Not asked for an action
   *  step marked `duringRun`. Given the step's action, so the host can let a `needsCamera` step through while the camera runs. */
  actionBlocked(action?: TourAction): string | null;
  /** A short detail the host formatted for a met condition (for example a revision or an instance id). It replaces
   *  `{detail}` in the action's `success` text. */
  detail?(name: string): string | undefined;
  /** Called with a step's element before it is checked for being shown: the app may open a collapsed section that holds it
   *  (view state only). */
  reveal?(el: Element): void;
  /** Who confirms an action, for "Waiting for … to confirm". Default: "the board". */
  confirmedBy?: string;
  feedback: FeedbackStore;
  /** Defaults: element connected, with a box, not inside [hidden]. */
  isShown?(el: Element): boolean;
  /** Wait for the host to render after a view change. Default: 80 ms. */
  settle?(): Promise<void>;
  onEnd?(how: "finished" | "closed"): void;
  /** Tests: a Driver.js factory. Default: import("driver.js") and its CSS. */
  loadDriver?: () => Promise<{ driver: (c?: Config) => DriverApi }>;
  pollMs?: number;
}

type DriverApi = { setConfig(c: Config): void; drive(i?: number): void; destroy(): void; isActive(): boolean; refresh(): void };

export interface TourRun {
  /** The step on screen: 0..steps-1, steps for the feedback checkpoint. */
  index(): number;
  /** What the step on screen is: "step", "missing", "recovery" or "feedback". */
  mode(): string;
  /** End the tour. `restoreView: false` (the host is going away): leave the views alone. */
  stop(opts?: { restoreView?: boolean }): void;
}

const fill = (success: string, detail: string | undefined) => success.replace("{detail}", detail?.trim() || "no detail reported");
const esc = (s: string) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
const selector = (value: string) => `[data-tour="${value.replace(/["\\]/g, "\\$&")}"]`;

function defaultShown(el: Element) {
  return el.isConnected && el.getClientRects().length > 0 && !el.closest("[hidden]");
}

/** Where a named region in image coordinates lands on screen, for a canvas whose intrinsic size is the image. */
export function regionRect(canvasBox: { left: number; top: number; width: number; height: number }, image: { width: number; height: number }, r: CanvasRegion) {
  const sx = canvasBox.width / image.width, sy = canvasBox.height / image.height;
  const w = (r.width ?? image.width - r.x), h = (r.height ?? image.height - r.y);
  return { left: canvasBox.left + r.x * sx, top: canvasBox.top + r.y * sy, width: w * sx, height: h * sy };
}

async function loadDriverJs() {
  const [mod] = await Promise.all([import("driver.js"), import("driver.js/dist/driver.css")]);
  return { driver: mod.driver as unknown as (c?: Config) => DriverApi };
}

export async function startTour(entry: TourEntry<unknown>, host: RunnerHost, from = 0): Promise<TourRun> {
  const { driver } = await (host.loadDriver ?? loadDriverJs)();
  const steps = entry.steps;
  const n = steps.length;
  const shown = host.isShown ?? defaultShown;
  const settle = host.settle ?? (() => new Promise<void>((r) => setTimeout(r, 80)));
  const startView = host.currentView();
  const reduced = typeof window.matchMedia === "function" && window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  let index = from, mode = "step", ended = false, token = 0;
  let poll: ReturnType<typeof setInterval> | null = null;
  let region: HTMLElement | null = null;
  let onResize: (() => void) | null = null;
  let flowOpen = false;
  let actionDone = true;
  let popover: PopoverDOM | null = null;
  let verdict: Verdict | null = null;

  const d = driver();
  const base: Config = {
    animate: !reduced, smoothScroll: !reduced, allowClose: true, allowKeyboardControl: true, overlayOpacity: 0.55,
    stagePadding: 6, stageRadius: 6, popoverClass: "tour-popover", showProgress: true,
    onDestroyStarted: () => stop("closed"),
  };

  const clearStep = () => {
    if (poll) clearInterval(poll);
    poll = null;
    if (onResize) window.removeEventListener("resize", onResize);
    onResize = null;
    region?.remove();
    region = null;
  };

  function stop(how: "finished" | "closed", restoreView = true) {
    if (ended) return;
    ended = true;
    token++;
    clearStep();
    d.destroy();
    if (!restoreView) return;
    if (flowOpen) host.showFlow(null);
    host.navigate(startView);
    host.onEnd?.(how);
  }

  function addControls(p: PopoverDOM, extra: { label: string; onClick: () => void; cls: string }[]) {
    popover = p;
    const holder = document.createElement("span");
    holder.className = "tour-extra-buttons";
    for (const b of [...extra, { label: "Skip", cls: "tour-skip", onClick: () => void show(index + 1) },
      ...(index > 0 || mode !== "step" ? [{ label: "Restart", cls: "tour-restart", onClick: () => void show(0) }] : [])]) {
      const el = document.createElement("button");
      el.type = "button";
      el.className = `tour-btn ${b.cls}`;
      el.textContent = b.label;
      el.addEventListener("click", (e) => { e.stopPropagation(); b.onClick(); });
      holder.append(el);
    }
    p.footer.insertBefore(holder, p.footerButtons);
  }

  function render(step: DriveStep, opts: { interactive?: boolean; nextDisabled?: boolean; isLast?: boolean; extra?: (p: PopoverDOM) => void } = {}) {
    const popoverCfg = step.popover ?? {};
    d.setConfig({
      ...base,
      disableActiveInteraction: !opts.interactive,
      steps: [{
        ...step,
        popover: {
          ...popoverCfg,
          showButtons: ["next", "previous", "close"],
          disableButtons: [...(index === 0 && (mode === "step" || mode === "missing") ? ["previous" as const] : []), ...(opts.nextDisabled ? ["next" as const] : [])],
          showProgress: true,
          progressText: index < n ? `${index + 1} of ${n}` : "Feedback",
          nextBtnText: opts.isLast ? "Finish" : "Next →",
          prevBtnText: "← Back",
          onNextClick: () => next(),
          onPrevClick: () => void show(prevIndex()),
          onCloseClick: () => stop("closed"),
          onPopoverRender: (p) => opts.extra?.(p),
        },
      }],
    });
    d.drive(0);
  }

  // Back: the step before; from a recovery, the action step again; from the checkpoint, the last step.
  const prevIndex = () => (mode === "recovery" ? index : mode === "feedback" ? n - 1 : Math.max(0, index - 1));

  function next() {
    if (mode === "feedback") return finish();
    if (mode === "step" && !actionDone) return;
    void show(index + 1);
  }

  function finish() {
    if (verdict) {
      const note = (document.querySelector(".tour-feedback-note") as HTMLTextAreaElement | null)?.value ?? "";
      host.feedback.add({ tour: entry.id, verdict, note, tourVersion: entry.introduced.date });
    }
    stop("finished");
  }

  async function resolve(step: TourStep): Promise<{ el: Element | null; missing?: string }> {
    const t = step.target;
    if ("flow" in t || "evidence" in t) {
      if (!flowOpen) { host.showFlow(entry); flowOpen = true; await settle(); }
      const el = document.querySelector(selector("flow" in t ? flowTarget(t.flow, t.node ?? t.edge) : EVIDENCE_TARGET));
      return el && shown(el) ? { el } : { el: null, missing: "This part of the flowchart is not on screen." };
    }
    if (flowOpen) { host.showFlow(null); flowOpen = false; await settle(); }
    if ("none" in t) return { el: null };
    if (t.view) { host.navigate(t.view); await settle(); }
    if ("ui" in t) {
      const el = document.querySelector(selector(t.ui));
      if (el) host.reveal?.(el);
      return el && shown(el) ? { el } : { el: null, missing: "It is not shown on this instrument, or not in its present state." };
    }
    const canvas = document.querySelector(selector(t.canvas));
    const area = entry.canvasRegions?.[t.region];
    if (!(canvas instanceof HTMLCanvasElement) || !shown(canvas) || !area) return { el: null, missing: "The image is not on screen." };
    if (t.requires && host.condition(t.requires) !== "met") return { el: null, missing: "There is no image yet: start the camera, then press Restart." };
    const place = () => {
      const r = regionRect(canvas.getBoundingClientRect(), { width: canvas.width, height: canvas.height }, area);
      Object.assign(region!.style, { left: `${r.left}px`, top: `${r.top}px`, width: `${r.width}px`, height: `${r.height}px` });
    };
    region = document.createElement("div");
    region.className = "tour-canvas-region";
    region.dataset.tourRegion = t.region;
    document.body.append(region);
    place();
    onResize = place;
    window.addEventListener("resize", onResize);
    return { el: region };
  }

  async function show(i: number) {
    if (ended) return;
    const my = ++token;
    clearStep();
    index = Math.max(0, Math.min(i, n));
    verdict = null;
    if (index === n) return showFeedback();
    const step = steps[index];
    mode = "step";
    actionDone = !step.action;
    const { el, missing } = await resolve(step);
    if (my !== token || ended) return;
    if (missing) {
      mode = "missing";
      actionDone = true;
      return render({ popover: { title: `Not on screen: ${esc(step.title)}`, description: `${esc(missing)} Skip this step, or restart the tour.`, side: "over", align: "center" } },
        { extra: (p) => addControls(p, []) });
    }
    const blocked = step.action && !step.action.duringRun ? host.actionBlocked(step.action) : null;
    const status = document.createElement("p");
    status.className = "tour-action-status";
    status.setAttribute("role", "status");
    const setNext = (enabled: boolean) => {
      actionDone = enabled;
      if (!popover) return;
      popover.nextButton.disabled = !enabled;
      popover.nextButton.classList.toggle("driver-popover-btn-disabled", !enabled);
    };
    const first = !step.action ? "met" : blocked ? "pending" : host.condition(step.action.await);
    // The status text is in place before Driver.js measures the popover, so it is placed around its final size.
    if (step.action) {
      status.textContent = blocked ? `Not now: ${blocked}. Skip this step to go on.`
        : first === "met" ? `Already done: ${fill(step.action.success, host.detail?.(step.action.await))}`
          : `${step.action.prompt} Waiting for ${host.confirmedBy ?? "the board"} to confirm…`;
    }
    render(
      { element: el ?? undefined, popover: { title: esc(step.title), description: esc(step.text), side: step.side ?? "bottom", align: "start" } },
      {
        interactive: (!!step.action && !blocked) || !!step.interactive,
        nextDisabled: !!step.action && first !== "met",
        extra: (p) => {
          addControls(p, []);
          if (step.action) p.description.append(status);
        },
      },
    );
    if (!step.action) return;
    const a = step.action;
    if (blocked) { actionDone = false; return; }
    if (first === "met") { actionDone = true; return; }
    const started = Date.now();
    poll = setInterval(() => {
      if (my !== token || ended) return;
      const why = a.duringRun ? null : host.actionBlocked(a);
      const c = host.condition(a.await);
      if (c === "met") {
        if (poll) clearInterval(poll);
        poll = null;
        status.textContent = `Done: ${fill(a.success, host.detail?.(a.await))}`;
        setNext(true);
        d.refresh();
      } else if (typeof c === "object") {
        void recovery(my, c.failed);
      } else if (why) {
        void recovery(my, `Stopped waiting: ${why}.`);
      } else if (Date.now() - started > a.timeoutMs) {
        void recovery(my, `No confirmation within ${Math.round(a.timeoutMs / 1000)} s.`);
      }
    }, host.pollMs ?? 250);
  }

  function recovery(my: number, detail: string) {
    if (my !== token || ended) return;
    clearStep();
    token++;
    mode = "recovery";
    actionDone = true;
    const step = steps[index];
    render(
      { popover: { title: `What happened: ${esc(step.title)}`, description: `${esc(detail)} ${esc(step.action?.recovery ?? "")}`, side: "over", align: "center" } },
      { extra: (p) => addControls(p, [{ label: "Try again", cls: "tour-retry", onClick: () => void show(index) }]) },
    );
  }

  function showFeedback() {
    mode = "feedback";
    actionDone = true;
    if (flowOpen) { host.showFlow(null); flowOpen = false; }
    render(
      { popover: { title: "Was this tour clear?", description: "Your answer stays in this browser. The Tours list can export it.", side: "over", align: "center" } },
      {
        isLast: true,
        extra: (p) => {
          popover = p;
          const box = document.createElement("div");
          box.className = "tour-feedback";
          for (const v of ["clear", "unclear"] as Verdict[]) {
            const b = document.createElement("button");
            b.type = "button";
            b.className = `tour-btn tour-verdict tour-verdict-${v}`;
            b.textContent = v === "clear" ? "Clear" : "Unclear";
            b.setAttribute("aria-pressed", "false");
            b.addEventListener("click", (e) => {
              e.stopPropagation();
              verdict = v;
              box.querySelectorAll(".tour-verdict").forEach((x) => x.setAttribute("aria-pressed", String(x === b)));
            });
            box.append(b);
          }
          const note = document.createElement("textarea");
          note.className = "tour-feedback-note";
          note.placeholder = "Optional note: what was unclear, or wrong?";
          note.rows = 3;
          // Arrow keys in the note move the caret, not the tour.
          note.addEventListener("keyup", (e) => { if (e.key !== "Escape") e.stopPropagation(); });
          box.append(note);
          p.description.append(box);
          const holder = document.createElement("span");
          holder.className = "tour-extra-buttons";
          const restart = document.createElement("button");
          restart.type = "button";
          restart.className = "tour-btn tour-restart";
          restart.textContent = "Restart";
          restart.addEventListener("click", (e) => { e.stopPropagation(); void show(0); });
          holder.append(restart);
          p.footer.insertBefore(holder, p.footerButtons);
        },
      },
    );
  }

  await show(from);
  return { index: () => index, mode: () => mode, stop: (opts) => stop("closed", opts?.restoreView ?? true) };
}
