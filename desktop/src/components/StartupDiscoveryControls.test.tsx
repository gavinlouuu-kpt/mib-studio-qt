// @vitest-environment jsdom
import {act} from 'react';
import {createRoot, type Root} from 'react-dom/client';
import {beforeEach, afterEach, it, expect, vi} from 'vitest';
import {StartupDiscoveryControls} from './StartupDiscoveryControls';
import {bridge} from '../bridge';
vi.mock('../bridge', () => ({bridge: {startupDiscoveryStatus: vi.fn(), startupDiscoveryRun: vi.fn()}}));
let host: HTMLDivElement, root: Root;
const status = {valid: true, camera_running: false, nanopositioner_running: false, camera_configured: false, nanopositioner_connected: false, camera: {job_id: '0', errors: []}, nanopositioner: {job_id: '0', errors: []}};
beforeEach(() => {Object.assign(globalThis, {IS_REACT_ACT_ENVIRONMENT: true}); const saved = new Map<string, string>(); vi.stubGlobal('localStorage', {getItem: (key: string) => saved.get(key) ?? null, setItem: (key: string, value: string) => saved.set(key, value)}); vi.clearAllMocks(); vi.mocked(bridge.startupDiscoveryStatus).mockResolvedValue(status); vi.mocked(bridge.startupDiscoveryRun).mockResolvedValue({accepted: true, message: 'Scheduled'}); host = document.createElement('div'); document.body.append(host); root = createRoot(host);});
afterEach(async () => {await act(async () => root.unmount()); host.remove();});
async function render(active = false) {await act(async () => root.render(<StartupDiscoveryControls ready experimentActive={active} append={() => {}} />));}
it('does not auto-select on first mount without saved opt-in', async () => {await render(); expect(bridge.startupDiscoveryRun).not.toHaveBeenCalled(); await act(async () => host.querySelector('button')!.click()); expect(bridge.startupDiscoveryRun).toHaveBeenCalledWith('start');});
it('runs saved startup preference only once per ready session', async () => {localStorage.setItem('mib.startup.auto-select-hardware', 'true'); await render(); await render(); expect(bridge.startupDiscoveryRun).toHaveBeenCalledOnce();});
it('blocks startup changes while experiment active', async () => {localStorage.setItem('mib.startup.auto-select-hardware', 'true'); await render(true); expect(bridge.startupDiscoveryRun).not.toHaveBeenCalled(); expect([...host.querySelectorAll('button')].every(button => button.disabled)).toBe(true);});
it('keeps scheduling distinct from actual connection', async () => {await render(); await act(async () => host.querySelector('button')!.click()); expect(host.textContent).toContain('Scheduled'); expect(host.textContent).toContain('Nanopositioner: manual selection needed');});
