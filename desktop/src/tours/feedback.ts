// Guided tours: per-browser storage (the "what's new" marker and the feedback checkpoint). Every access is wrapped:
// without localStorage, or with one that throws (private mode, a full quota, a sandboxed frame), the tours still work
// and only remember for the life of the page. Nothing here calls a backend.

export interface SafeStore {
  get(key: string): string | null;
  set(key: string, value: string): boolean;
}

/** localStorage behind try/catch, with an in-memory fallback for anything it cannot keep. */
export function safeStore(storage: () => Storage | undefined = () => globalThis.localStorage): SafeStore {
  const memory = new Map<string, string>();
  return {
    get(key) {
      try {
        const v = storage()?.getItem(key);
        if (v !== null && v !== undefined) return v;
      } catch { /* unavailable: use the page's copy */ }
      return memory.get(key) ?? null;
    },
    set(key, value) {
      memory.set(key, value);
      try {
        const s = storage();
        if (!s) return false;
        s.setItem(key, value);
        return true;
      } catch {
        return false;
      }
    },
  };
}

export type Verdict = "clear" | "unclear";

export interface FeedbackRecord {
  tour: string;
  verdict: Verdict;
  note: string;
  /** ISO time the checkpoint was answered. */
  at: string;
  /** The tour's `introduced.date`, so feedback on an older version of a tour stays distinguishable. */
  tourVersion: string;
}

const isRecord = (r: unknown): r is FeedbackRecord => {
  const x = r as FeedbackRecord;
  return !!x && typeof x.tour === "string" && (x.verdict === "clear" || x.verdict === "unclear") && typeof x.note === "string" && typeof x.at === "string";
};

export interface FeedbackStore {
  all(): FeedbackRecord[];
  add(r: Omit<FeedbackRecord, "at"> & { at?: string }): FeedbackRecord;
  /** The export the Tours list offers: `{ kind, exported, records }` as pretty JSON. */
  exportJson(meta?: Record<string, unknown>): string;
}

export function feedbackStore(store: SafeStore, key: string, now: () => Date = () => new Date()): FeedbackStore {
  const read = (): FeedbackRecord[] => {
    try {
      const v = JSON.parse(store.get(key) ?? "[]");
      return Array.isArray(v) ? v.filter(isRecord) : [];
    } catch {
      return [];
    }
  };
  return {
    all: read,
    add(r) {
      const record: FeedbackRecord = { ...r, note: r.note.slice(0, 2000), at: r.at ?? now().toISOString() };
      store.set(key, JSON.stringify([...read(), record].slice(-500)));
      return record;
    },
    exportJson(meta = {}) {
      return JSON.stringify({ kind: "tour-feedback", ...meta, exported: now().toISOString(), records: read() }, null, 2) + "\n";
    },
  };
}
