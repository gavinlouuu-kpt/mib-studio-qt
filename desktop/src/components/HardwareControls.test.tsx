// @vitest-environment jsdom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { HardwareControls } from './HardwareControls';
import { bridge } from '../bridge';
import type { AutofocusConfig, AutofocusStatus, CmdResult, PumpStatus } from '../bridge';
vi.mock('../bridge', () => ({bridge: {
  fetchPumpStatus: vi.fn(), pumpPollStatus: vi.fn(), fetchAutofocusStatus: vi.fn(), fetchAutofocusConfig: vi.fn(),
  pumpStart: vi.fn(), pumpStop: vi.fn(), pumpStopPurge: vi.fn(), pumpPurge: vi.fn(), pumpSetFlowRate: vi.fn(),
  autofocusSetEnabled: vi.fn(), autofocusJog: vi.fn(), autofocusSetConfig: vi.fn(),
  pulseGeneratorStatus: vi.fn(),
}}));
const ok = (message = 'ok'): CmdResult => ({ok: true, command: 10, message, operation_id: '0'});
const pump: PumpStatus = {valid: true, connected: true, run_status: 0, current_flow_rate: 1, accumulated_volume: 0, min_flow_rate: 0, max_flow_rate: 10, stalled: false, com_port: 1, baud_rate: 115200, modbus_address: 1, configured_flow_rate: 1, flow_rate_unit: 100, direction: 0};
const focus: AutofocusStatus = {valid: true, connected: true, enabled: false, current_voltage: 50, com_port: 3, average_ring_ratio: 1, median_ring_ratio: 1, last_ring_ratio_update_us: 0, ring_ratio_age_us: 0};
const config: AutofocusConfig = {valid: true, focus_setpoint: 1, focus_range: .1, voltage_step: 1, fine_voltage_step: .1, min_voltage: 0, max_voltage: 100, initial_voltage: 50, manual_voltage_step: 1, ring_ratio_stale_ms: 500, min_samples_per_step: 1, safe_shutdown_voltage: 0, require_new_sample_per_step: true, focus_direction: true};
let host: HTMLDivElement, root: Root;
const append = vi.fn(), onDisarm = vi.fn();
const button = (name: string) => Array.from(host.querySelectorAll('button')).find(node => node.textContent === name)!;
const click = async (node: HTMLElement) => act(async () => node.click());
/** Qt's 'Enable Autofocus' check box. */
const enableBox = () => Array.from(host.querySelectorAll('label')).find(node => node.textContent === 'Enable autofocus')!.querySelector('input') as HTMLInputElement;
async function render(overrides = {}) {
  await act(async () => root.render(<HardwareControls ready experimentActive={false} append={append} mode="operator" armed={false} onDisarm={onDisarm} {...overrides} />));
}
beforeEach(() => {
  Object.assign(globalThis, {IS_REACT_ACT_ENVIRONMENT: true});
  vi.clearAllMocks();
  vi.mocked(bridge.fetchPumpStatus).mockResolvedValue(pump);
  vi.mocked(bridge.pumpPollStatus).mockResolvedValue(ok());
  vi.mocked(bridge.fetchAutofocusStatus).mockResolvedValue(focus);
  vi.mocked(bridge.fetchAutofocusConfig).mockResolvedValue(config);
  for (const name of ['pumpStart', 'pumpStop', 'pumpStopPurge', 'autofocusSetEnabled', 'autofocusJog'] as const) vi.mocked(bridge[name] as (...a: unknown[]) => Promise<CmdResult>).mockResolvedValue(ok(name));
  vi.mocked(bridge.pulseGeneratorStatus).mockRejectedValue(new Error('no generator'));
  host = document.createElement('div'); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe('device panel composition', () => {
  it('shows pumps, autofocus and the pulse generator on the desktop, and pumps only on the PZ7035', async () => {
    await render();
    expect(host.querySelector('h2')?.textContent).toBe('Pumps and autofocus');
    expect(Array.from(host.querySelectorAll('legend')).map(node => node.textContent)).toEqual(['Startup hardware selection', 'Sample pump', 'Sheath pump', 'Autofocus / nanopositioner', 'Acquisition pulse generator (RS485)']);
    const capabilities = {
      instrument: 'pz7035' as const, autofocus: false, trigger: false, host_background: false, frame_buffer: false,
      reanalysis: false, core_updates: false, egrabber_script: false, pl_identity: true, led_strobe: true, align_mode: false, run_mode: false,
      pump: {model: 'tushui_peristaltic', port: '/dev/ttyPS1', sample_address: 3, sheath_address: 4, microliters_per_rev: 25},
    };
    await render({capabilities});
    expect(host.querySelector('h2')?.textContent).toBe('Pumps');
    expect(Array.from(host.querySelectorAll('legend')).map(node => node.textContent)).toEqual(['Sample pump', 'Sheath pump']);
    expect(host.textContent).not.toContain('nanopositioner');
    expect(host.textContent).not.toContain('Pulse');
  });
  it('lets the operator run and stop a pump during an experiment without Service mode or the arm', async () => {
    await render({experimentActive: true});
    expect(button('Run configured pump').disabled).toBe(false);
    await click(button('Run configured pump'));
    expect(bridge.pumpStart).toHaveBeenCalledWith(0);
    expect(onDisarm).not.toHaveBeenCalled();
    await click(button('Stop pump'));
    expect(bridge.pumpStop).toHaveBeenCalledWith(0);
  });
});

describe('autofocus operator controls (D2, D3)', () => {
  it('enables and jogs without Service mode or the arm, also during an experiment', async () => {
    await render({experimentActive: true});
    expect(enableBox().disabled).toBe(false);
    await click(enableBox());
    expect(bridge.autofocusSetEnabled).toHaveBeenCalledWith(true);
    await click(button('Jog voltage +'));
    expect(bridge.autofocusJog).toHaveBeenCalledWith(true);
    await click(button('Jog voltage −'));
    expect(bridge.autofocusJog).toHaveBeenCalledWith(false);
    expect(onDisarm).not.toHaveBeenCalled();
  });
  it('jogs while autofocus is enabled, as Qt does', async () => {
    vi.mocked(bridge.fetchAutofocusStatus).mockResolvedValue({...focus, enabled: true});
    await render();
    expect(button('Jog voltage +').disabled).toBe(false);
    expect(enableBox().checked).toBe(true);
  });
  it('needs a connected nanopositioner to enable or jog, but not to disable', async () => {
    vi.mocked(bridge.fetchAutofocusStatus).mockResolvedValue({...focus, connected: false});
    await render();
    expect(enableBox().disabled).toBe(true);
    expect(button('Jog voltage +').disabled).toBe(true);
    expect(button('Disable autofocus').disabled).toBe(false);
  });
  it('disables autofocus even while another command is in flight, and only a not-ready backend refuses', async () => {
    let finish!: (value: CmdResult) => void;
    vi.mocked(bridge.autofocusJog).mockReturnValue(new Promise(resolve => { finish = resolve; }));
    await render();
    await click(button('Jog voltage +'));
    await act(async () => { await Promise.resolve(); });
    expect(button('Jog voltage −').disabled).toBe(true);        // a non-stop command is in flight
    expect(button('Disable autofocus').disabled).toBe(false);
    await click(button('Disable autofocus'));
    expect(bridge.autofocusSetEnabled).toHaveBeenCalledWith(false);
    await act(async () => finish(ok()));
    await render({ready: false});
    expect(button('Disable autofocus').disabled).toBe(true);
  });
  it('keeps connection and the tuning editor locked during an experiment, and blocks them when the status cannot be read', async () => {
    await render({experimentActive: true});
    expect(button('Disconnect autofocus').disabled).toBe(true);
    expect(button('Apply autofocus configuration').disabled).toBe(true);
    expect(button('Disable autofocus').disabled).toBe(false);
    vi.mocked(bridge.fetchAutofocusStatus).mockRejectedValue(new Error('Transport lost'));
    await render({experimentActive: false});
    await act(async () => { await new Promise(resolve => setTimeout(resolve, 1100)); });
    expect(button('Apply autofocus configuration').disabled).toBe(true);   // no configuration without a status
    expect(button('Jog voltage +').disabled).toBe(true);
    expect(button('Disable autofocus').disabled).toBe(false);
    expect(host.textContent).toContain('Transport lost');
  });
  it('still reports the autofocus status error when only the pumps are readable', async () => {
    vi.mocked(bridge.fetchAutofocusStatus).mockRejectedValue(new Error('Autofocus link down'));
    await render();
    expect(host.textContent).toContain('Autofocus link down');
    expect(button('Run configured pump').disabled).toBe(false);   // the pump panel does not depend on the autofocus read
  });
});
