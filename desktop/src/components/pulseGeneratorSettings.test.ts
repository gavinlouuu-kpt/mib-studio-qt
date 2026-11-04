// @vitest-environment jsdom
import {beforeEach, describe, expect, it, vi} from 'vitest';
import {bridge} from '../bridge';
import {DEFAULT_PULSE_FORM, type PulseForm} from './pulseGeneratorModel';
import {loadPulseGeneratorSettings, PULSE_PREFERENCES_KEY, sanitizeSettings, savePulseGeneratorSettings, serializeSettings} from './pulseGeneratorSettings';
vi.mock('../bridge', () => ({bridge: {getPreferences: vi.fn(), setPreferences: vi.fn()}}));

const form: PulseForm = {...DEFAULT_PULSE_FORM, port: ' /dev/ttyUSB1 ', baud: 19200, dataBits: 7, parity: 'E', stopBits: 2, address: '6', channel: 2, frequency: '1200.5', duty: '12'};
const stored = {port: '/dev/ttyUSB1', baud: 19200, dataBits: 7, parity: 'E', stopBits: 2, address: 6, channel: 2, frequencyHz: 1200.5, dutyPercent: 12};
beforeEach(() => {
  vi.clearAllMocks();
  const saved = new Map<string, string>(); // this Node has no usable localStorage under jsdom; same stub as StartupDiscoveryControls.test.tsx
  vi.stubGlobal('localStorage', {getItem: (key: string) => saved.get(key) ?? null, setItem: (key: string, value: string) => saved.set(key, value)});
});

describe('serialising the form (HW-056)', () => {
  it('stores port, serial parameters, address, channel, frequency and duty as typed values', () => {
    expect(serializeSettings(form)).toEqual(stored);
  });
  it('leaves out text that is not a number', () => {
    expect(serializeSettings({...form, address: '', frequency: 'abc', duty: ' '})).toEqual({port: '/dev/ttyUSB1', baud: 19200, dataBits: 7, parity: 'E', stopBits: 2, channel: 2});
  });
});

describe('reading stored settings', () => {
  it('turns a stored record back into form fields', () => {
    expect(sanitizeSettings(stored)).toEqual({...form, port: '/dev/ttyUSB1'});
  });
  it('drops every field that is not valid today, keeps the rest', () => {
    expect(sanitizeSettings({port: 5, baud: 1234, dataBits: 9, parity: 'X', stopBits: 3, address: 300, channel: 4, frequencyHz: 10, dutyPercent: 101})).toEqual({});
    expect(sanitizeSettings({baud: 4800, address: 1.5, frequencyHz: 5000})).toEqual({baud: 4800, frequency: '5000'});
    expect(sanitizeSettings(null)).toEqual({});
    expect(sanitizeSettings('x')).toEqual({});
  });
});

describe('the preferences file', () => {
  it('loads from the pulseGenerator key', async () => {
    vi.mocked(bridge.getPreferences).mockResolvedValue({sidebarCollapsed: true, [PULSE_PREFERENCES_KEY]: stored});
    expect(await loadPulseGeneratorSettings()).toMatchObject({port: '/dev/ttyUSB1', baud: 19200, address: '6', frequency: '1200.5'});
  });
  it('starts from the defaults when nothing is stored', async () => {
    vi.mocked(bridge.getPreferences).mockResolvedValue({});
    expect(await loadPulseGeneratorSettings()).toEqual({});
  });
  it('saves by read-modify-write, keeping the other preferences', async () => {
    vi.mocked(bridge.getPreferences).mockResolvedValue({sidebarCollapsed: true, other: {a: 1}});
    vi.mocked(bridge.setPreferences).mockResolvedValue(undefined);
    await savePulseGeneratorSettings(form);
    expect(bridge.setPreferences).toHaveBeenCalledWith({sidebarCollapsed: true, other: {a: 1}, [PULSE_PREFERENCES_KEY]: stored});
  });
  it('writes one save at a time', async () => {
    const order: string[] = [];
    vi.mocked(bridge.getPreferences).mockImplementation(async () => { order.push('read'); await Promise.resolve(); return {}; });
    vi.mocked(bridge.setPreferences).mockImplementation(async () => { order.push('write'); });
    await Promise.all([savePulseGeneratorSettings(form), savePulseGeneratorSettings({...form, channel: 1})]);
    expect(order).toEqual(['read', 'write', 'read', 'write']);
  });
  it('round-trips through the file', async () => {
    let file: Record<string, unknown> = {};
    vi.mocked(bridge.getPreferences).mockImplementation(async () => structuredClone(file));
    vi.mocked(bridge.setPreferences).mockImplementation(async (next: Record<string, unknown>) => { file = structuredClone(next); });
    await savePulseGeneratorSettings(form);
    expect({...DEFAULT_PULSE_FORM, ...await loadPulseGeneratorSettings()}).toEqual({...form, port: '/dev/ttyUSB1'});
  });
});

describe('without the preferences file (browser)', () => {
  it('falls back to localStorage for both directions and never throws', async () => {
    vi.mocked(bridge.getPreferences).mockRejectedValue(new Error('desktop only'));
    await expect(savePulseGeneratorSettings(form)).resolves.toBeUndefined();
    expect(bridge.setPreferences).not.toHaveBeenCalled();
    expect(await loadPulseGeneratorSettings()).toMatchObject({port: '/dev/ttyUSB1', channel: 2});
  });
});
