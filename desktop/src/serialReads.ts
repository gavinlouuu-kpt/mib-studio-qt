// One poll's reads, one at a time, newest reply wins (#736 review). A poll that ticks while its
// previous read is still in flight skips the tick instead of stacking another request, so a
// stalled backend does not collect a pile of reads. A read that has not ended after `staleMs` no
// longer holds the poll: the next tick starts a new read. Every read carries a sequence number,
// and a reply is applied only when no later read's reply has been applied already, so a late,
// older reply never overwrites newer state.
//
// Pure module (no React): App keeps one instance per poll in a ref.

export class SerialReads {
  private started = 0;
  private shown = 0;
  private inFlight: { seq: number; since: number } | null = null;

  constructor(private readonly staleMs: number, private readonly now: () => number = () => Date.now()) {}

  /** Start a read: its sequence number, or null to skip this tick (a read is in flight and not
   *  yet stale). */
  begin(): number | null {
    if (this.inFlight && this.now() - this.inFlight.since < this.staleMs) return null;
    const seq = ++this.started;
    this.inFlight = { seq, since: this.now() };
    return seq;
  }

  /** Whether read `seq`'s reply may be applied: true unless a later read's reply was applied.
   *  Several parts of the same read may each be accepted. */
  accept(seq: number): boolean {
    if (seq < this.shown) return false;
    this.shown = seq;
    return true;
  }

  /** Read `seq` ended (replied or failed). A stale read that ends late frees nothing. */
  end(seq: number): void {
    if (this.inFlight?.seq === seq) this.inFlight = null;
  }

  /** True while a read holds the poll. */
  get busy(): boolean {
    return !!this.inFlight && this.now() - this.inFlight.since < this.staleMs;
  }
}
