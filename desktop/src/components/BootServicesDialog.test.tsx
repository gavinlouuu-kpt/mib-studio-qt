// @vitest-environment jsdom
// Settings > Boot Service Toggles… (HW-038, Qt's MainWindow.cpp:246-302): the services to leave off at the next start.
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { BootServicesDialog } from './BootServicesDialog';
import { bridge } from '../bridge';
import type { CmdResult, HostSettingsDocument } from '../bridge';

vi.mock('../bridge', () => ({bridge: {fetchHostSettings: vi.fn(), hostSettingsCommand: vi.fn()}}));
const ok = (message = 'ok'): CmdResult => ({ok: true, command: 5, message, operation_id: '0'});
let host: HTMLDivElement, root: Root;
let saved: Record<string, unknown>, bootEnv: string;
const append = vi.fn(), onClose = vi.fn();
const box = (label: string) => Array.from(host.querySelectorAll('label.boot-service')).find(node => node.textContent === label)!.querySelector('input') as HTMLInputElement;
const button = (name: string) => Array.from(host.querySelectorAll('button')).find(node => node.textContent === name)! as HTMLButtonElement;
const click = async (node: HTMLElement) => act(async () => node.click());
const saves = () => vi.mocked(bridge.hostSettingsCommand).mock.calls.map(([request]) => request as {action: string; services: string[]});
async function render(ready = true) {
  await act(async () => root.render(<BootServicesDialog ready={ready} append={append} onClose={onClose} />));
  await act(async () => { await Promise.resolve(); });
}
beforeEach(() => {
  Object.assign(globalThis, {IS_REACT_ACT_ENVIRONMENT: true});
  vi.clearAllMocks();
  saved = {}; bootEnv = '';
  vi.mocked(bridge.fetchHostSettings).mockImplementation(async (): Promise<HostSettingsDocument> => ({valid: true, path: '', settings: structuredClone(saved), load_error: '', boot_env: bootEnv, pump_scan: {state: 'idle', operation_id: '0', port: '', found: [], error: ''}}));
  vi.mocked(bridge.hostSettingsCommand).mockImplementation(async request => { saved.boot_disabled_services = (request as {services: string[]}).services; return ok('Boot service toggles saved.'); });
  host = document.createElement('div'); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe('Boot Service Toggles', () => {
  it('lists Qt\'s services, with the saved ones ticked, and Qt\'s explanation', async () => {
    saved = {boot_disabled_services: ['trigger', 'sqlite']};
    await render();
    expect(Array.from(host.querySelectorAll('label.boot-service')).map(node => node.textContent)).toEqual(['SQLite service', 'HDF5 service', 'Processing service', 'Autofocus wiring', 'Trigger wiring', 'Capture service', 'Playback service']);
    expect(['SQLite service', 'Trigger wiring', 'HDF5 service'].map(label => box(label).checked)).toEqual([true, true, false]);
    expect(host.textContent).toContain('Disable selected services at next application startup. Changes are persisted and applied on next launch.');
  });
  it('ticks every service for a saved "all" and saves the services one by one', async () => {
    saved = {boot_disabled_services: ['all']};
    await render();
    expect(Array.from(host.querySelectorAll('label.boot-service input')).every(input => (input as HTMLInputElement).checked)).toBe(true);
    await click(box('Autofocus wiring'));
    await click(button('OK'));
    expect(saves()).toEqual([{action: 'set_boot_services', services: ['sqlite', 'hdf5', 'processing', 'trigger', 'capture', 'playback']}]);
  });
  it('saves the ticked services, keeps tokens it does not list, and says to restart', async () => {
    saved = {boot_disabled_services: ['auto_update', 'dot_grid']};
    await render();
    expect(host.textContent).toContain('Also left off, kept as saved: auto_update, dot_grid.');
    await click(box('Trigger wiring')); await click(box('Autofocus wiring'));
    await click(button('OK'));
    expect(saves()).toEqual([{action: 'set_boot_services', services: ['autofocus', 'trigger', 'auto_update', 'dot_grid']}]);
    expect(host.querySelector('[role="status"]')!.textContent).toBe('Saved. Restart the application for changes to take effect.');
    expect(append).toHaveBeenCalledWith('Boot service toggles: Saved. Restart the application for changes to take effect.');
  });
  it('clears the list when everything is unticked, and writes nothing on Cancel', async () => {
    saved = {boot_disabled_services: ['trigger']};
    await render();
    await click(box('Trigger wiring')); await click(button('Cancel'));
    expect(saves()).toEqual([]); expect(onClose).toHaveBeenCalledOnce();
    await click(button('OK'));
    expect(saves()).toEqual([{action: 'set_boot_services', services: []}]);
  });
  it('warns that MIB_DISABLED_SERVICES in the environment wins', async () => {
    bootEnv = 'autofocus,trigger';
    await render();
    expect(host.textContent).toContain('MIB_DISABLED_SERVICES is set in this application\'s environment (autofocus,trigger); it overrides what is saved here.');
  });
  it('says when the host would not save', async () => {
    vi.mocked(bridge.hostSettingsCommand).mockResolvedValue({...ok(), ok: false, message: 'There is no data directory to keep the settings in'});
    await render();
    await click(button('OK'));
    expect(host.querySelector('[role="alert"]')!.textContent).toBe('Not saved: There is no data directory to keep the settings in');
  });
  it('waits for the backend before it offers anything to save', async () => {
    await render(false);
    expect(host.textContent).toContain('Waiting for the backend');
    expect(button('OK').disabled).toBe(true);
  });
});
