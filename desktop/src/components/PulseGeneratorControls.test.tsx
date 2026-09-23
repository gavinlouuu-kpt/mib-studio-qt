// @vitest-environment jsdom
import {act} from 'react';
import {createRoot, type Root} from 'react-dom/client';
import {beforeEach, afterEach, it, expect, vi} from 'vitest';
import {PulseGeneratorControls} from './PulseGeneratorControls';
import {bridge} from '../bridge';
vi.mock('../bridge', () => ({bridge: {pulseGeneratorStatus: vi.fn(), pulseGeneratorCommand: vi.fn()}}));
let host: HTMLDivElement, root: Root;
const disarm = vi.fn();
const button = (name: string) => [...host.querySelectorAll('button')].find(x => x.textContent === name)!;
const status = {valid: true, connected: true, owned: false, error: 'None', port: '/dev/test', baud: 9600, address: 1, channels: [{frequency_hz: 5000, duty_percent: 50, output_enabled: false}]};
beforeEach(() => { Object.assign(globalThis, {IS_REACT_ACT_ENVIRONMENT: true}); vi.clearAllMocks(); vi.mocked(bridge.pulseGeneratorStatus).mockResolvedValue(status); vi.mocked(bridge.pulseGeneratorCommand).mockResolvedValue({ok: true, command: 12, operation_id: '0', message: 'Done'}); host = document.createElement('div'); document.body.append(host); root = createRoot(host); });
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });
async function render(props = {}) {await act(async () => root.render(<PulseGeneratorControls ready experimentActive={false} mode="service" armed onDisarm={disarm} append={() => {}} {...props} />));}
it('does not write on mount and consumes one arm on explicit output enable', async () => {await render(); expect(bridge.pulseGeneratorCommand).not.toHaveBeenCalled(); await act(async () => button('Enable pulse output').click()); expect(disarm).toHaveBeenCalledOnce(); expect(bridge.pulseGeneratorCommand).toHaveBeenCalledWith({action: 'enable', channel: 0, enabled: true});});
it('allows output-off during experiments but no output-on or frequency write', async () => {await render({experimentActive: true, armed: false}); expect(button('Enable pulse output').disabled).toBe(true); expect(button('Set frequency').disabled).toBe(true); await act(async () => button('Disable pulse output').click()); expect(bridge.pulseGeneratorCommand).toHaveBeenCalledWith({action: 'enable', channel: 0, enabled: false}); expect(disarm).not.toHaveBeenCalled();});
it('does not override coordinated live-view ownership', async () => {vi.mocked(bridge.pulseGeneratorStatus).mockResolvedValue({...status, owned: true}); await render(); expect(button('Enable pulse output').disabled).toBe(true); expect(button('Disable pulse output').disabled).toBe(true); expect(button('Disconnect pulse generator').disabled).toBe(true);});
it('reports backend failures without success and blocks duplicate requests', async () => {let resolve!: (x: Awaited<ReturnType<typeof bridge.pulseGeneratorCommand>>) => void; vi.mocked(bridge.pulseGeneratorCommand).mockReturnValue(new Promise(r => {resolve = r;})); await render(); await act(async () => {button('Enable pulse output').click(); button('Enable pulse output').click();}); expect(bridge.pulseGeneratorCommand).toHaveBeenCalledOnce(); await act(async () => resolve({ok: false, command: 12, operation_id: '0', message: 'Port lost'})); expect(host.textContent).toContain('Port lost');});
