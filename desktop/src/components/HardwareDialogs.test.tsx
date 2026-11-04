// @vitest-environment jsdom
// The dialogs are opened by the Settings menu and the pump panel through window events; the shell mounts the host once.
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { HardwareDialogs } from './HardwareDialogs';
import { openBootServices, openPumpSettings } from './hardwareDialogEvents';
import { bridge } from '../bridge';

vi.mock('../bridge', () => ({bridge: {listSerialPorts: vi.fn(), fetchHostSettings: vi.fn(), hostSettingsCommand: vi.fn(), fetchPumpStatus: vi.fn(), pumpSetSyringeVolume: vi.fn()}}));
let host: HTMLDivElement, root: Root;
const dialog = (name: string) => host.querySelector(`[role="dialog"][aria-label="${name}"]`);
beforeEach(async () => {
  Object.assign(globalThis, {IS_REACT_ACT_ENVIRONMENT: true});
  vi.clearAllMocks();
  vi.mocked(bridge.listSerialPorts).mockResolvedValue({ports: []});
  vi.mocked(bridge.fetchHostSettings).mockResolvedValue({valid: true, path: '', settings: {}, load_error: '', boot_env: '', pump_scan: {state: 'idle', operation_id: '0', port: '', found: [], error: ''}});
  host = document.createElement('div'); document.body.append(host); root = createRoot(host);
  await act(async () => root.render(<HardwareDialogs ready append={() => undefined} />));
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe('HardwareDialogs', () => {
  it('shows nothing until a menu item or the pump panel asks', () => {
    expect(host.querySelector('[role="dialog"]')).toBeNull();
  });
  it('opens the pump settings and the boot toggles on their events, and closes them with Cancel', async () => {
    await act(async () => openPumpSettings());
    expect(dialog('Syringe Pump Settings')).not.toBeNull(); expect(dialog('Boot Service Toggles')).toBeNull();
    await act(async () => (Array.from(host.querySelectorAll('button')).find(node => node.textContent === 'Cancel') as HTMLElement).click());
    expect(dialog('Syringe Pump Settings')).toBeNull();
    await act(async () => openBootServices());
    expect(dialog('Boot Service Toggles')).not.toBeNull();
    await act(async () => (Array.from(host.querySelectorAll('button')).find(node => node.textContent === 'Cancel') as HTMLElement).click());
    expect(dialog('Boot Service Toggles')).toBeNull();
  });
  it('stops listening when it unmounts', async () => {
    await act(async () => root.unmount()); root = createRoot(host);
    await act(async () => openPumpSettings());
    expect(host.querySelector('[role="dialog"]')).toBeNull();
  });
});
