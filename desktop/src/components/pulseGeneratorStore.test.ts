// @vitest-environment jsdom
// Where the pulse generator settings live (HW-056): the instrument's shell preferences, a localStorage copy as the fallback.
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { PULSE_SETTINGS_KEY, PULSE_SETTINGS_STORAGE_KEY } from './pulseGeneratorModel';
import { loadPulseSettings, savePulseSettings, type PreferenceApi } from './pulseGeneratorStore';

let prefs: Record<string, unknown>;
// The jsdom here has no usable localStorage (the other tests stub it the same way).
const memory = new Map<string, string>();
const storage = { getItem: (key: string) => memory.get(key) ?? null, setItem: (key: string, value: string) => { memory.set(key, value); } };
const api = (): PreferenceApi => ({ get: vi.fn(async () => structuredClone(prefs)), set: vi.fn(async (next: Record<string, unknown>) => { prefs = structuredClone(next); }) });
beforeEach(() => { prefs = {}; memory.clear(); });

describe('load', () => {
  it('reads the record from the shell preferences', async () => {
    prefs = { sidebarCollapsed: true, [PULSE_SETTINGS_KEY]: { portName: 'COM4', baud: 19200, address: 3 } };
    expect(await loadPulseSettings(api(), storage)).toEqual({ portName: 'COM4', baud: 19200, address: 3 });
  });
  it('falls back to the local copy when the preferences are unreadable or hold none', async () => {
    memory.set(PULSE_SETTINGS_STORAGE_KEY, JSON.stringify({ portName: 'ttyUSB0', frequencyHz: 1000 }));
    expect(await loadPulseSettings(api(), storage)).toEqual({ portName: 'ttyUSB0', frequencyHz: 1000 });
    const broken: PreferenceApi = { get: async () => { throw new Error('no shell'); }, set: async () => {} };
    expect(await loadPulseSettings(broken, storage)).toEqual({ portName: 'ttyUSB0', frequencyHz: 1000 });
  });
  it('is empty when nothing usable is stored (a corrupt local copy included)', async () => {
    expect(await loadPulseSettings(api(), storage)).toEqual({});
    memory.set(PULSE_SETTINGS_STORAGE_KEY, '{not json');
    expect(await loadPulseSettings(api(), storage)).toEqual({});
    expect(await loadPulseSettings(api(), null)).toEqual({});
  });
});

describe('save', () => {
  it('merges into the saved record, keeps the other preferences and mirrors it locally', async () => {
    prefs = { sidebarCollapsed: true, [PULSE_SETTINGS_KEY]: { portName: 'COM4', baud: 9600, address: 1 } };
    expect(await savePulseSettings(api(), storage, { address: 5, frequencyHz: 1000 })).toBe('');
    expect(prefs).toEqual({ sidebarCollapsed: true, [PULSE_SETTINGS_KEY]: { portName: 'COM4', baud: 9600, address: 5, frequencyHz: 1000 } });
    expect(JSON.parse(memory.get(PULSE_SETTINGS_STORAGE_KEY)!)).toMatchObject({ address: 5, frequencyHz: 1000 });
  });
  it('two quick saves cannot lose one another', async () => {
    const both = await Promise.all([savePulseSettings(api(), storage, { channel: 2 }), savePulseSettings(api(), storage, { dutyPercent: 10 })]);
    expect(both).toEqual(['', '']);
    expect(prefs[PULSE_SETTINGS_KEY]).toEqual({ channel: 2, dutyPercent: 10 });
  });
  it('reports a preferences failure (and still keeps the local copy)', async () => {
    const failing: PreferenceApi = { get: async () => ({}), set: async () => { throw new Error('disk full'); } };
    expect(await savePulseSettings(failing, storage, { channel: 3 })).toContain('disk full');
    expect(JSON.parse(memory.get(PULSE_SETTINGS_STORAGE_KEY)!)).toEqual({ channel: 3 });
    const unreadable: PreferenceApi = { get: async () => { throw new Error('no shell'); }, set: vi.fn(async () => {}) };
    expect(await savePulseSettings(unreadable, storage, { channel: 4 })).toContain('no shell');
    expect(unreadable.set).not.toHaveBeenCalled();
  });
});
