// @vitest-environment jsdom
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { HOST_SETTINGS_EVENT, reloadHostSettings, saveBootServices, savePumpSettings } from './hostSettingsStore';
import { bridge } from '../bridge';
import type { CmdResult, HostSettingsDocument } from '../bridge';

vi.mock('../bridge', () => ({bridge: {fetchHostSettings: vi.fn(), hostSettingsCommand: vi.fn()}}));
const ok = (message = 'ok'): CmdResult => ({ok: true, command: 10, message, operation_id: '0'});
const doc = (settings: Record<string, unknown> = {}, valid = true): HostSettingsDocument => ({valid, path: '', settings, load_error: '', boot_env: '', pump_scan: {state: 'idle', operation_id: '0', port: '', found: [], error: ''}});
const deferred = <T,>() => { let resolve!: (value: T) => void; const promise = new Promise<T>(r => { resolve = r; }); return {promise, resolve}; };
beforeEach(() => { vi.clearAllMocks(); vi.mocked(bridge.fetchHostSettings).mockResolvedValue(doc({a: 1})); vi.mocked(bridge.hostSettingsCommand).mockResolvedValue(ok('saved')); });

describe('host settings store', () => {
  it('announces the document to every reader after a save, once the host has the new values', async () => {
    const heard: HostSettingsDocument[] = [];
    const listener = (event: Event) => heard.push((event as CustomEvent<HostSettingsDocument>).detail);
    window.addEventListener(HOST_SETTINGS_EVENT, listener);
    const outcome = await savePumpSettings({pump_sample_address: 2});
    window.removeEventListener(HOST_SETTINGS_EVENT, listener);
    expect(outcome).toEqual({ok: true, message: 'saved'});
    expect(bridge.hostSettingsCommand).toHaveBeenCalledWith({action: 'set_pumps', settings: {pump_sample_address: 2}});
    expect(heard).toEqual([doc({a: 1})]);
  });
  it('sends boot toggles as their own action', async () => {
    await saveBootServices(['trigger']);
    expect(bridge.hostSettingsCommand).toHaveBeenCalledWith({action: 'set_boot_services', services: ['trigger']});
  });
  it('chains saves: the second goes out only after the first answered', async () => {
    const first = deferred<CmdResult>();
    vi.mocked(bridge.hostSettingsCommand).mockReturnValueOnce(first.promise);
    const a = savePumpSettings({pump_sample_address: 1}), b = savePumpSettings({pump_sample_address: 2});
    await Promise.resolve(); await Promise.resolve();
    expect(bridge.hostSettingsCommand).toHaveBeenCalledOnce();
    first.resolve(ok());
    await Promise.all([a, b]);
    expect(vi.mocked(bridge.hostSettingsCommand).mock.calls.map(([request]) => (request as {settings: {pump_sample_address: number}}).settings.pump_sample_address)).toEqual([1, 2]);
  });
  it('reports a refusal or a transport failure as an outcome, never as an exception, and keeps the chain going', async () => {
    vi.mocked(bridge.hostSettingsCommand).mockResolvedValueOnce({...ok(), ok: false, message: 'pump_sample_address is out of range'}).mockRejectedValueOnce(new Error('Transport lost'));
    expect(await savePumpSettings({pump_sample_address: 999})).toEqual({ok: false, message: 'pump_sample_address is out of range'});
    expect(await savePumpSettings({pump_sample_address: 1})).toEqual({ok: false, message: 'Transport lost'});
    expect(await savePumpSettings({pump_sample_address: 1})).toEqual({ok: true, message: 'saved'});
  });
  it('gives a refusal without a reason a message of its own', async () => {
    vi.mocked(bridge.hostSettingsCommand).mockResolvedValueOnce({...ok(), ok: false, message: ''});
    expect((await savePumpSettings({})).message).toBe('The host refused the settings.');
  });
  it('reload returns null for a host that cannot be read or has no valid document, and announces nothing then', async () => {
    const heard = vi.fn(); window.addEventListener(HOST_SETTINGS_EVENT, heard);
    vi.mocked(bridge.fetchHostSettings).mockResolvedValueOnce(doc({}, false)).mockRejectedValueOnce(new Error('down'));
    expect(await reloadHostSettings()).toBeNull(); expect(await reloadHostSettings()).toBeNull();
    window.removeEventListener(HOST_SETTINGS_EVENT, heard);
    expect(heard).not.toHaveBeenCalled();
  });
});
