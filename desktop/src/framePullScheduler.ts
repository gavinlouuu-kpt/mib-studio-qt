import type { FramePacket } from "./framePacket";
/** Presentation only: cancellation discards replies, never stops native work.
 * One aggregate in-flight pull; one replaceable pending request per view. */
export class FramePullScheduler {
  private views = new Map<string, { epoch: number; deliver: (p: FramePacket) => void; error: (e: unknown) => void }>();
  private pending = new Map<string, { epoch: number; pull: () => Promise<FramePacket> }>();
  private serial = 0;
  private active = false;
  private peakPending = 0;
  private replacements = 0;
  private stale = 0;
  static readonly MAX_VIEWS = 4;
  mount(id: string, deliver: (p: FramePacket) => void, error: (e: unknown) => void): () => void {
    if (this.views.has(id) || this.views.size >= FramePullScheduler.MAX_VIEWS)
      throw new Error("FRAME_VIEW_LIMIT_OR_DUPLICATE");
    const view = { epoch: ++this.serial, deliver, error };
    this.views.set(id, view);
    return () => {
      if (this.views.get(id) === view) { this.views.delete(id); this.pending.delete(id); }
    };
  }
  invalidate(id?: string): void {
    for (const [key, view] of this.views) if (id === undefined || key === id) {
      view.epoch = ++this.serial; this.pending.delete(key);
    }
  }
  request(id: string, pull: () => Promise<FramePacket>, supersede = true): void {
    const view = this.views.get(id); if (!view) return;
    if (supersede) view.epoch = ++this.serial;
    if (this.pending.has(id)) this.replacements++;
    this.pending.set(id, { epoch: view.epoch, pull });
    this.peakPending = Math.max(this.peakPending, this.pending.size);
    this.pump();
  }
  private pump(): void {
    if (this.active || this.pending.size === 0) return;
    const [id, job] = this.pending.entries().next().value!;
    this.pending.delete(id); this.active = true;
    void Promise.resolve().then(job.pull).then(packet => {
      const view = this.views.get(id);
      if (view?.epoch === job.epoch) view.deliver(packet); else this.stale++;
    }).catch(error => {
      const view = this.views.get(id);
      if (view?.epoch === job.epoch) view.error(error); else this.stale++;
    }).finally(() => { this.active = false; this.pump(); });
  }
  snapshot() {
    return { views: this.views.size, inFlight: Number(this.active), pending: this.pending.size,
      peakPending: this.peakPending, replacements: this.replacements, staleReplies: this.stale };
  }
}
