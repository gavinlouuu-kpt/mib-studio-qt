// @vitest-environment jsdom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { HardwareControls } from './HardwareControls';
import { bridge } from '../bridge';
import type { AutofocusConfig, PumpStatus } from '../bridge';
vi.mock('../bridge', () => ({bridge: {
  fetchPumpStatus: vi.fn(), fetchAutofocusStatus: vi.fn(), fetchAutofocusConfig: vi.fn(),
  pumpStart: vi.fn(), pumpStop: vi.fn(), pumpPurge: vi.fn(), pumpSetFlowRate: vi.fn(), autofocusSetEnabled: vi.fn(),
}}));
const pump: PumpStatus = {valid: true, connected: true, run_status: 0, current_flow_rate: 1, accumulated_volume: 0, min_flow_rate: 0, max_flow_rate: 10, stalled: false, com_port: 1, baud_rate: 115200, modbus_address: 1, configured_flow_rate: 1, flow_rate_unit: 100, direction: 0};
const config: AutofocusConfig = {valid: true, focus_setpoint: 1, focus_range: .1, voltage_step: 1, fine_voltage_step: .1, min_voltage: 0, max_voltage: 100, initial_voltage: 50, manual_voltage_step: 1, ring_ratio_stale_ms: 500, min_samples_per_step: 1, safe_shutdown_voltage: 0, require_new_sample_per_step: true, focus_direction: true};
let host: HTMLDivElement, root: Root;
const append = vi.fn(), onDisarm = vi.fn();
const button = (name: string) => Array.from(host.querySelectorAll('button')).find(node => node.textContent === name)!;
async function render(overrides = {}) {
  await act(async () => root.render(<HardwareControls ready experimentActive={false} append={append} mode="service" armed onDisarm={onDisarm} {...overrides} />));
}
beforeEach(() => {
  Object.assign(globalThis, {IS_REACT_ACT_ENVIRONMENT: true});
  vi.clearAllMocks();
  vi.mocked(bridge.fetchPumpStatus).mockResolvedValue(pump);
  vi.mocked(bridge.fetchAutofocusStatus).mockResolvedValue({valid: true, connected: true, enabled: false, current_voltage: 50, com_port: 3, average_ring_ratio: 1, median_ring_ratio: 1, last_ring_ratio_update_us: 0, ring_ratio_age_us: 0});
  vi.mocked(bridge.fetchAutofocusConfig).mockResolvedValue(config);
  vi.mocked(bridge.pumpStart).mockResolvedValue({ok: true, command: 10, message: 'Started', operation_id: '0'});
  vi.mocked(bridge.pumpStop).mockResolvedValue({ok: true, command: 10, message: 'Stopped', operation_id: '0'});
  host = document.createElement('div'); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });
describe('hardware operator interactions', () => {
  it('does not actuate while mounted and uses one-shot arming on an explicit click', async () => {
    await render();
    expect(bridge.pumpStart).not.toHaveBeenCalled();
    await act(async () => button('Run configured pump').click());
    expect(onDisarm).toHaveBeenCalledOnce();
    expect(bridge.pumpStart).toHaveBeenCalledWith(0);
    expect(append).toHaveBeenCalledWith('Start pump: Started');
  });
  it('locks mutations during an experiment without removing stop', async () => {
    await render({experimentActive: true, armed: false, mode: 'operator'});
    expect(button('Run configured pump').disabled).toBe(true);
    expect(button('Apply rate').disabled).toBe(true);
    expect(button('Enable autofocus').disabled).toBe(true);
    expect(button('Disable autofocus').disabled).toBe(false);
    await act(async () => button('Stop pump').click());
    expect(bridge.pumpStop).toHaveBeenCalledWith(0);
  });
  it('shows rejected commands as failures, never optimistic success', async () => {
    vi.mocked(bridge.pumpStart).mockResolvedValue({ok: false, command: 10, message: 'Device stalled', operation_id: '0'});
    await render();
    await act(async () => button('Run configured pump').click());
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Device stalled');
    expect(append).not.toHaveBeenCalledWith('Start pump: Started');
  });
  it('blocks motion and configuration when status cannot be read', async () => {
    vi.mocked(bridge.fetchPumpStatus).mockRejectedValue(new Error('Transport lost'));
    await render();
    expect(button('Run configured pump').disabled).toBe(true);
    expect(button('Apply rate').disabled).toBe(true);
    expect(button('Apply autofocus configuration').disabled).toBe(true);
    expect(button('Stop pump').disabled).toBe(false);
    expect(host.textContent).toContain('Transport lost');
  });
  it('validates blank flow rates before invoking the bridge', async () => {
    await render();
    await act(async () => button('Apply rate').click());
    expect(bridge.pumpSetFlowRate).not.toHaveBeenCalled();
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Flow rate');
  });
});
