// State that survives a page reload within one browser tab (#550 m14): the operator's stage
// confirmations and the event log. A reload during a run used to reset both. sessionStorage is per
// tab and cleared when it closes, so nothing leaks between sessions; every access is guarded
// because storage can throw or be absent (private windows, tests, blocked site data).
//
// Confirmations are safe to restore: the workflow compares them with the current device and core
// signature, so a confirmation for another device or core no longer counts.

import { useCallback, useState } from "react";

export function readPersisted<T>(key: string, fallback: T, valid: (v: unknown) => v is T): T {
  try {
    const raw = window.sessionStorage.getItem(key);
    if (raw === null) return fallback;
    const parsed: unknown = JSON.parse(raw);
    return valid(parsed) ? parsed : fallback;
  } catch {
    return fallback;
  }
}

export function writePersisted(key: string, value: unknown): void {
  try {
    window.sessionStorage.setItem(key, JSON.stringify(value));
  } catch {
    /* storage unavailable: the state just does not survive a reload */
  }
}

/** useState whose value is mirrored to sessionStorage under `key`. */
export function usePersistedState<T>(key: string, initial: T, valid: (v: unknown) => v is T): [T, (next: T | ((prev: T) => T)) => void] {
  const [value, setValue] = useState<T>(() => readPersisted(key, initial, valid));
  const set = useCallback(
    (next: T | ((prev: T) => T)) => {
      setValue((prev) => {
        const resolved = typeof next === "function" ? (next as (p: T) => T)(prev) : next;
        writePersisted(key, resolved);
        return resolved;
      });
    },
    [key],
  );
  return [value, set];
}

export const isString = (v: unknown): v is string => typeof v === "string";
export const isStringArray = (v: unknown): v is string[] => Array.isArray(v) && v.every((x) => typeof x === "string");
