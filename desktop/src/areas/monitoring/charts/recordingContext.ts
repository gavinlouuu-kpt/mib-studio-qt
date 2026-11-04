// Test support: a CanvasRenderingContext2D stand-in that records every call with the fill and stroke
// style in force (jsdom has no canvas). Used by the Monitoring chart tests.
export interface RecordedCall {
  fn: string;
  args: unknown[];
  fillStyle: unknown;
  strokeStyle: unknown;
}

export function recordingContext() {
  const calls: RecordedCall[] = [];
  const state: Record<string, unknown> = {};
  const g = new Proxy({} as Record<string, unknown>, {
    get(_t, key: string) {
      if (key in state) return state[key];
      return (...args: unknown[]) => {
        calls.push({ fn: key, args, fillStyle: state.fillStyle, strokeStyle: state.strokeStyle });
      };
    },
    set(_t, key: string, value) {
      state[key] = value;
      return true;
    },
  }) as unknown as CanvasRenderingContext2D;
  const of = (fn: string) => calls.filter((c) => c.fn === fn);
  const texts = () => of("fillText").map((c) => String(c.args[0]));
  return { g, calls, of, texts };
}
