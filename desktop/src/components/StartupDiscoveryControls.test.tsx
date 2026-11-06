// @vitest-environment jsdom
import {act} from 'react';
import {createRoot, type Root} from 'react-dom/client';
import {beforeEach, afterEach, it, expect, vi} from 'vitest';
import {StartupDiscoveryControls} from './StartupDiscoveryControls';
import {bridge} from '../bridge';
vi.mock('../bridge', () => ({bridge: {startupDiscoverySetPreference: vi.fn(), startupDiscoveryStatus: vi.fn(), startupDiscoveryRun: vi.fn()}}));
let host: HTMLDivElement, root: Root;
const status = {valid: true, camera_running: false, nanopositioner_running: false, camera_configured: false, nanopositioner_connected: false, camera: {job_id: '0', errors: []}, nanopositioner: {job_id: '0', errors: []}};
beforeEach(() => {Object.assign(globalThis, {IS_REACT_ACT_ENVIRONMENT: true}); const saved = new Map<string, string>(); vi.stubGlobal('localStorage', {getItem: (key: string) => saved.get(key) ?? null, setItem: (key: string, value: string) => saved.set(key, value)}); vi.clearAllMocks(); vi.mocked(bridge.startupDiscoverySetPreference).mockImplementation(async preference => ({accepted: true, message: 'Applied', preference})); vi.mocked(bridge.startupDiscoveryStatus).mockResolvedValue(status); vi.mocked(bridge.startupDiscoveryRun).mockResolvedValue({accepted: true, message: 'Scheduled'}); host = document.createElement('div'); document.body.append(host); root = createRoot(host);});
afterEach(async () => {await act(async () => root.unmount()); host.remove();});
async function render(active = false) {await act(async () => root.render(<StartupDiscoveryControls ready experimentActive={active} append={() => {}} />));}
it('auto-selects on first mount of a fresh profile, as Qt starts discovery unconditionally (HW-037, D8)', async () => {
  await render();
  expect(bridge.startupDiscoveryRun).toHaveBeenCalledWith('start');
  expect(bridge.startupDiscoveryRun).toHaveBeenCalledOnce();
  expect((host.querySelector('input[type="checkbox"]') as HTMLInputElement).checked).toBe(true);
  await act(async () => [...host.querySelectorAll('button')].find(button => button.textContent === 'Auto-select hardware now')!.click());
  expect(bridge.startupDiscoveryRun).toHaveBeenCalledTimes(2);
});
it('keeps an operator opt-out, remembers it, and does not run discovery while opted out', async () => {
  await render();
  await act(async () => (host.querySelector('input[type="checkbox"]') as HTMLInputElement).click());
  expect(localStorage.getItem('mib.startup.auto-select-hardware')).toBe('false');
  await act(async () => root.unmount()); root = createRoot(host);
  vi.mocked(bridge.startupDiscoveryRun).mockClear();
  await render();
  expect((host.querySelector('input[type="checkbox"]') as HTMLInputElement).checked).toBe(false);
  expect(bridge.startupDiscoveryRun).not.toHaveBeenCalled();
  await act(async () => (host.querySelector('input[type="checkbox"]') as HTMLInputElement).click());
  expect(localStorage.getItem('mib.startup.auto-select-hardware')).toBe('true');
});
it('stays manual when the browser storage cannot be read: the remembered selection is unknown, so nothing is guessed', async () => {
  vi.stubGlobal('localStorage', {getItem: () => {throw new Error('blocked');}, setItem: () => {throw new Error('blocked');}});
  await render();
  expect(bridge.startupDiscoveryRun).not.toHaveBeenCalled();
  expect(host.textContent).toContain('automatic selection skipped');
});
it('runs saved startup preference only once per ready session', async () => {localStorage.setItem('mib.startup.auto-select-hardware', 'true'); await render(); await render(); expect(bridge.startupDiscoveryRun).toHaveBeenCalledOnce();});
it('blocks startup changes while experiment active', async () => {localStorage.setItem('mib.startup.auto-select-hardware', 'true'); await render(true); expect(bridge.startupDiscoveryRun).not.toHaveBeenCalled(); expect([...host.querySelectorAll('button')].every(button => button.disabled)).toBe(true);});
it('keeps scheduling distinct from actual connection', async () => {await render(); await act(async () => [...host.querySelectorAll('button')].find(button => button.textContent === 'Auto-select hardware now')!.click()); expect(host.textContent).toContain('Scheduled'); expect(host.textContent).toContain('Nanopositioner: manual selection needed');});
it('installs remembered identity before automatic selection', async () => {
  const preference = {backend:'oeabt', endpoint:'/dev/serial/by-id/test', com_port:-1, baud:57600, address:7};
  localStorage.setItem('mib.startup.nanopositioner.v1', JSON.stringify(preference));
  localStorage.setItem('mib.startup.auto-select-hardware', 'true');
  await render();
  expect(bridge.startupDiscoverySetPreference).toHaveBeenCalledWith(preference);
  expect(vi.mocked(bridge.startupDiscoverySetPreference).mock.invocationCallOrder[0]).toBeLessThan(vi.mocked(bridge.startupDiscoveryRun).mock.invocationCallOrder[0]);
});
it('does not auto-select with corrupt remembered identity', async () => {
  localStorage.setItem('mib.startup.nanopositioner.v1', '{broken');
  localStorage.setItem('mib.startup.auto-select-hardware', 'true');
  await render();
  expect(bridge.startupDiscoveryRun).not.toHaveBeenCalled();
  expect(bridge.startupDiscoverySetPreference).not.toHaveBeenCalled();
  expect(host.textContent).toContain('automatic selection skipped');
});
it('reports session-only application when persistence fails', async () => {
  await render();
  vi.stubGlobal('localStorage', {getItem: () => null, setItem: () => {throw new Error('quota');}});
  await act(async () => [...host.querySelectorAll('button')].find(button => button.textContent === 'Save startup preference')!.click());
  expect(host.textContent).toContain('applied for this session but could not be saved');
});
