// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { DiscoveredDevice } from '../bridge';
import { defaultStartupPreference, startupPreferenceKey } from '../startupPreference';
import {
  AutofocusCommandQueue, SELECTION_SAVED_EVENT, SKIPPED, candidateFromDevice, candidateFromPreference, connectTarget, listCandidates, parseAddress, parseJogStep,
  parseSetpoint, rememberSelection, rememberSetpoint, rememberedSelection, rememberedSetpoint, selectionPreference, setpointKey, stepFromConfig, usesCoremorSettings,
} from './autofocusControlModel';
import { publishAutofocusMessage, recentAutofocusMessages, resetAutofocusMessages, subscribeAutofocusMessages } from './autofocusMessages';

const device = (over: Partial<DiscoveredDevice>): DiscoveredDevice => ({
  kind: 2, provider_id: 'nanopositioner', display_name: 'USB Serial', system_path: '/dev/ttyUSB0', persistent_id: '/dev/serial/by-id/usb-1', sdk_index: 0,
  interface_index: 0, device_index: 0, stream_index: 0, bus_address: 0, stable_identity: '', identity_strength: 2, identification: 1, claimed_by: [],
  capabilities: ['vendor:OEABT'], synthetic: false, camera_type: -1, interface_id: '', device_id: '', stream_id: '', model_name: '', firmware_version: '', label: '', ...over,
});
const oeabt = candidateFromDevice(device({}));
const coremor = candidateFromDevice(device({display_name: 'COM3', system_path: 'COM3', persistent_id: 'COM3', capabilities: ['vendor:CoreMorrow / XMT'], bus_address: 1}));

describe('input rules of Qt\'s spin boxes', () => {
  it('target ring width: 1-100, two decimals, rounded as the spin box shows it', () => {
    expect(parseSetpoint('20')).toBe(20);
    expect(parseSetpoint(' 1 ')).toBe(1);
    expect(parseSetpoint('100')).toBe(100);
    expect(parseSetpoint('20.126')).toBe(20.13);
    for (const bad of ['', 'abc', '0', '0.99', '100.01', '-3', '1e1', '2,5']) expect(() => parseSetpoint(bad), bad).toThrow(/Target ring width/);
  });
  it('jog step: whole volts 1-100', () => {
    expect(parseJogStep('1')).toBe(1);
    expect(parseJogStep('100')).toBe(100);
    for (const bad of ['', '0', '101', '1.5', '-1', 'x']) expect(() => parseJogStep(bad), bad).toThrow(/Step/);
    expect(stepFromConfig(5.7)).toBe('5');
    expect(stepFromConfig(0.2)).toBe('1');
    expect(stepFromConfig(500)).toBe('100');
    expect(stepFromConfig(NaN)).toBe('1');
  });
  it('device address: whole number 0-255', () => {
    expect(parseAddress('0')).toBe(0);
    expect(parseAddress('255')).toBe(255);
    for (const bad of ['', '-1', '256', '1.5']) expect(() => parseAddress(bad), bad).toThrow(/address/);
  });
});

describe('device list from the discovery job (HW-025, HW-024)', () => {
  it('labels OEABT endpoints as candidates and takes the backend from the vendor capability', () => {
    expect(oeabt).toMatchObject({id: '/dev/serial/by-id/usb-1', backend: 'oeabt', comPort: -1, label: 'OEABT candidate — USB Serial', identification: 'unidentified', saved: false});
    expect(coremor).toMatchObject({id: 'COM3', backend: 'coremor', comPort: 3, label: 'COM3'});
    // The simulated OEABT bench names its port COM5: only the vendor says it is not a CoreMorrow DLL port.
    expect(candidateFromDevice(device({system_path: 'COM5', persistent_id: 'COM5', display_name: 'COM5'}))).toMatchObject({backend: 'oeabt', comPort: -1});
    expect(candidateFromDevice(device({capabilities: [], system_path: 'COM4', persistent_id: 'COM4'})).backend).toBe('coremor');
    expect(candidateFromDevice(device({capabilities: [], system_path: '/dev/ttyACM0', persistent_id: ''})).backend).toBe('oeabt');
    expect(candidateFromDevice(device({identification: 0})).identification).toBe('identified');
    expect(candidateFromDevice(device({identification: 2})).identification).toBe('ambiguous');
  });
  it('the backend choice filters the list; Auto keeps everything', () => {
    expect(listCandidates([oeabt, coremor], 'auto', null).map(c => c.id)).toEqual([oeabt.id, 'COM3']);
    expect(listCandidates([oeabt, coremor], 'oeabt', null).map(c => c.id)).toEqual([oeabt.id]);
    expect(listCandidates([oeabt, coremor], 'coremor', null).map(c => c.id)).toEqual(['COM3']);
  });
  it('the remembered selection joins the list only when the scan did not return it', () => {
    const saved = candidateFromPreference({backend: 'oeabt', endpoint: oeabt.id, com_port: -1, baud: 115200, address: 1})!;
    expect(saved).toMatchObject({saved: true, backend: 'oeabt'});
    expect(listCandidates([oeabt], 'auto', saved)).toHaveLength(1);
    expect(listCandidates([], 'auto', saved)).toEqual([saved]);
    expect(listCandidates([], 'coremor', saved)).toEqual([]);
    expect(candidateFromPreference(defaultStartupPreference)).toBeNull();
    expect(candidateFromPreference({backend: 'auto', endpoint: 'COM6', com_port: 6, baud: 115200, address: 1})).toMatchObject({backend: 'coremor', comPort: 6});
    expect(candidateFromPreference({backend: 'auto', endpoint: '/dev/ttyUSB1', com_port: -1, baud: 115200, address: 1})).toMatchObject({backend: 'oeabt', comPort: -1});
  });
  it('baud and address matter for CoreMOR only', () => {
    expect(usesCoremorSettings('coremor', null)).toBe(true);
    expect(usesCoremorSettings('oeabt', coremor)).toBe(false);
    expect(usesCoremorSettings('auto', coremor)).toBe(true);
    expect(usesCoremorSettings('auto', oeabt)).toBe(false);
    expect(usesCoremorSettings('auto', null)).toBe(false);
  });
});

describe('connect target (the backend refuses Auto, BackendFacade.cpp:2330)', () => {
  it('resolves Auto to the row\'s backend and passes the baud and address fields along for every backend (the :197 literals)', () => {
    expect(connectTarget({choice: 'auto', candidate: oeabt, baud: '57600', address: '7'})).toEqual({backend: 'oeabt', endpoint: oeabt.id, comPort: -1, baud: 57600, address: 7});
    expect(connectTarget({choice: 'auto', candidate: coremor, baud: '19200', address: '0'})).toEqual({backend: 'coremor', endpoint: 'COM3', comPort: 3, baud: 19200, address: 0});
    expect(connectTarget({choice: 'coremor', candidate: coremor, baud: '115200', address: '1'}).backend).toBe('coremor');
  });
  it('refuses without a device, a bad address, or a CoreMOR row that is no COM port', () => {
    expect(() => connectTarget({choice: 'auto', candidate: null, baud: '115200', address: '1'})).toThrow(/Select a serial device/);
    expect(() => connectTarget({choice: 'auto', candidate: oeabt, baud: '115200', address: '300'})).toThrow(/address/);
    expect(() => connectTarget({choice: 'coremor', candidate: oeabt, baud: '115200', address: '1'})).toThrow(/not a CoreMOR COM port/);
  });
});

describe('selection saved on connect, disconnect and exit (HW-036)', () => {
  const previous = {...defaultStartupPreference};
  it('saves what Qt saves: backend choice, endpoint, COM number for CoreMOR, baud, address', () => {
    expect(selectionPreference({choice: 'auto', candidate: oeabt, baud: '115200', address: '1', previous})).toEqual({backend: 'auto', endpoint: oeabt.id, com_port: -1, baud: 115200, address: 1});
    expect(selectionPreference({choice: 'coremor', candidate: coremor, baud: '57600', address: '9', previous})).toEqual({backend: 'coremor', endpoint: 'COM3', com_port: 3, baud: 57600, address: 9});
    expect(selectionPreference({choice: 'oeabt', candidate: oeabt, baud: '9600', address: '2', previous})).toMatchObject({backend: 'oeabt', com_port: -1});
  });
  it('keeps the earlier selection when the list is empty and never saves what the backend would refuse', () => {
    const earlier = {backend: 'oeabt' as const, endpoint: 'x', com_port: -1, baud: 115200, address: 1};
    expect(selectionPreference({choice: 'auto', candidate: null, baud: '9600', address: '3', previous: earlier})).toEqual({...earlier, baud: 9600, address: 3});
    expect(selectionPreference({choice: 'coremor', candidate: oeabt, baud: '115200', address: '1', previous})?.backend).toBe('auto');
    expect(selectionPreference({choice: 'auto', candidate: oeabt, baud: '', address: '1', previous})).toBeNull();
  });
});

describe('remembered values in browser storage', () => {
  let saved: Map<string, string>;
  const storage = () => ({getItem: (key: string) => saved.get(key) ?? null, setItem: (key: string, value: string) => { saved.set(key, value); }});
  beforeEach(() => { saved = new Map(); });
  it('round-trips the selection through the startup preference record and announces it', () => {
    const heard = vi.fn(); window.addEventListener(SELECTION_SAVED_EVENT, heard);
    const preference = {backend: 'oeabt' as const, endpoint: '/dev/ttyUSB3', com_port: -1, baud: 115200, address: 1};
    expect(rememberSelection(preference, storage())).toMatchObject({saved: true, preference});
    expect(JSON.parse(saved.get(startupPreferenceKey)!)).toEqual(preference);
    expect(rememberedSelection(storage())).toEqual(preference);
    expect(heard).toHaveBeenCalledTimes(1);
    window.removeEventListener(SELECTION_SAVED_EVENT, heard);
  });
  it('does not throw when storage fails or the record is invalid', () => {
    expect(rememberSelection(defaultStartupPreference, {setItem: () => { throw new Error('quota'); }})).toMatchObject({saved: false, error: expect.stringContaining('quota')});
    expect(rememberSelection({backend: 'coremor', endpoint: 'COM3', com_port: -1, baud: 115200, address: 1}, storage()).saved).toBe(false);
    saved.set(startupPreferenceKey, '{broken');
    expect(rememberedSelection(storage())).toEqual(defaultStartupPreference);
  });
  it('remembers the target ring width, 1-100, rounded to two decimals', () => {
    expect(rememberedSetpoint(storage())).toBeNull();
    expect(rememberSetpoint(22.456, storage())).toBe(true);
    expect(saved.get(setpointKey)).toBe('22.46');
    expect(rememberedSetpoint(storage())).toBe(22.46);
    saved.set(setpointKey, '150'); expect(rememberedSetpoint(storage())).toBeNull();
    saved.set(setpointKey, 'oops'); expect(rememberedSetpoint(storage())).toBeNull();
    expect(rememberSetpoint(5, {setItem: () => { throw new Error('blocked'); }})).toBe(false);
  });
});

describe('command queue (D3: nothing queued is sent after Disable or Disconnect)', () => {
  const deferred = <T,>() => { let resolve!: (value: T) => void; const promise = new Promise<T>(r => { resolve = r; }); return {promise, resolve}; };
  it('runs commands one at a time, in order', async () => {
    const queue = new AutofocusCommandQueue(), order: string[] = [], first = deferred<void>();
    const a = queue.run('config', async () => { order.push('a start'); await first.promise; order.push('a end'); return 'a'; });
    const b = queue.run('actuate', async () => { order.push('b'); return 'b'; });
    await Promise.resolve();
    expect(order).toEqual(['a start']);
    first.resolve();
    expect(await a).toBe('a'); expect(await b).toBe('b');
    expect(order).toEqual(['a start', 'a end', 'b']);
  });
  it('drops an actuating command that was queued before cancelActuation, but not a configuration write', async () => {
    const queue = new AutofocusCommandQueue(), first = deferred<void>(), sent: string[] = [];
    const blocker = queue.run('config', async () => { await first.promise; sent.push('blocker'); });
    const enable = queue.run('actuate', async () => { sent.push('enable'); });
    const jog = queue.run('actuate', async () => { sent.push('jog'); });
    const setpoint = queue.run('config', async () => { sent.push('setpoint'); });
    queue.cancelActuation();
    const later = queue.run('actuate', async () => { sent.push('later'); });
    first.resolve();
    await Promise.all([blocker, enable, jog, setpoint, later]);
    expect(await enable).toBe(SKIPPED); expect(await jog).toBe(SKIPPED);
    expect(sent).toEqual(['blocker', 'setpoint', 'later']);
  });
  it('lets a running actuating command see the cancellation between its steps, and survives a failing command', async () => {
    const queue = new AutofocusCommandQueue(), step = deferred<void>(), sent: string[] = [];
    const jog = queue.run('actuate', async context => { await step.promise; if (context.cancelled()) return 'cancelled'; sent.push('jog'); return 'sent'; });
    await Promise.resolve();
    queue.cancelActuation();
    step.resolve();
    expect(await jog).toBe('cancelled');
    await expect(queue.run('config', async () => { throw new Error('boom'); })).rejects.toThrow('boom');
    expect(await queue.run('config', async () => 'still works')).toBe('still works');
    expect(sent).toEqual([]);
  });
});

describe('autofocus messages (HW-033)', () => {
  beforeEach(() => resetAutofocusMessages());
  afterEach(() => resetAutofocusMessages());
  it('keeps the newest five for a panel that mounts late and tells subscribers', () => {
    const heard: string[] = [];
    const stop = subscribeAutofocusMessages(message => heard.push(message.text));
    for (let i = 1; i <= 7; i += 1) publishAutofocusMessage(`m${i}`, i);
    expect(recentAutofocusMessages().map(m => m.text)).toEqual(['m3', 'm4', 'm5', 'm6', 'm7']);
    expect(heard).toHaveLength(7);
    stop(); publishAutofocusMessage('after', 8);
    expect(heard).toHaveLength(7);
  });
});
