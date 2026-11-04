// @vitest-environment jsdom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { HardwareControls } from './HardwareControls';
import { bridge } from '../bridge';
import type { AutofocusConfig, PumpStatus } from '../bridge';
vi.mock('../bridge', () => ({bridge: {
  fetchPumpStatus: vi.fn(), fetchAutofocusStatus: vi.fn(), fetchAutofocusConfig: vi.fn(),
  pumpStart: vi.fn(), pumpStop: vi.fn(), pumpPurge: vi.fn(), pumpStopPurge: vi.fn(), pumpSetFlowRate: vi.fn(), pumpSetDirection: vi.fn(),
  pumpSetSyringeVolume: vi.fn(), pumpPollStatus: vi.fn(), autofocusSetEnabled: vi.fn(), autofocusSetConfig: vi.fn(), autofocusJog: vi.fn(),
  pumpConnectModel: vi.fn(), getPreferences: vi.fn(), setPreferences: vi.fn(),
}}));
const pump: PumpStatus = {valid: true, connected: true, run_status: 0, current_flow_rate: 1, accumulated_volume: 0, min_flow_rate: 0, max_flow_rate: 10, stalled: false, com_port: 1, baud_rate: 115200, modbus_address: 1, configured_flow_rate: 1, flow_rate_unit: 100, direction: 0};
const config: AutofocusConfig = {valid: true, focus_setpoint: 1, focus_range: .1, voltage_step: 1, fine_voltage_step: .1, min_voltage: 0, max_voltage: 100, initial_voltage: 50, manual_voltage_step: 1, ring_ratio_stale_ms: 500, min_samples_per_step: 1, safe_shutdown_voltage: 0, require_new_sample_per_step: true, focus_direction: true};
let host: HTMLDivElement, root: Root;
const append = vi.fn(), onDisarm = vi.fn();
const labelled = (text: string) => Array.from(host.querySelectorAll('label')).filter(node => node.textContent?.startsWith(text)).map(node => node.querySelector('select, input') as HTMLSelectElement & HTMLInputElement);
function choose(select: HTMLSelectElement, value: string) {
  Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value')!.set!.call(select, value);
  select.dispatchEvent(new Event('change', {bubbles: true}));
}
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
const accepted = (message = 'OK') => ({ok: true, command: 10, message, operation_id: '0'});
const typeInto = async (input: HTMLInputElement, value: string) => act(async () => {
  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, value);
  input.dispatchEvent(new Event('input', {bubbles: true}));
});
const settle = (ms = 0) => act(async () => { await new Promise(resolve => setTimeout(resolve, ms)); });
describe('hardware operator interactions', () => {
  beforeEach(() => {
    for (const fn of [bridge.pumpSetFlowRate, bridge.pumpSetDirection, bridge.pumpPurge, bridge.pumpStopPurge, bridge.pumpPollStatus,
      bridge.pumpSetSyringeVolume, bridge.autofocusSetEnabled, bridge.autofocusSetConfig, bridge.autofocusJog]) vi.mocked(fn).mockResolvedValue(accepted());
    vi.mocked(bridge.getPreferences).mockResolvedValue({});
    vi.mocked(bridge.setPreferences).mockResolvedValue(undefined);
  });
  it('starts a connected pump in Operator mode without arming (Qt parity)', async () => {
    await render({mode: 'operator', armed: false});
    expect(bridge.pumpStart).not.toHaveBeenCalled();
    await act(async () => button('Start pump').click());
    expect(onDisarm).not.toHaveBeenCalled();
    expect(bridge.pumpStart).toHaveBeenCalledWith(0);
    expect(append).toHaveBeenCalledWith('Start pump: Started');
  });
  it('keeps pump and autofocus controls live during an experiment, but locks connecting', async () => {
    await render({experimentActive: true, armed: false, mode: 'operator'});
    expect(button('Start pump').disabled).toBe(false);
    expect(button('Jog voltage +').disabled).toBe(false);
    expect(labelled('Enable Autofocus')[0].disabled).toBe(false);
    expect(button('Disconnect').disabled).toBe(true);
    expect(host.textContent).toContain('Connecting or disconnecting a device is locked while an experiment is active.');
    await act(async () => button('Stop pump').click());
    expect(bridge.pumpStop).toHaveBeenCalledWith(0);
  });
  it('polls each connected pump on the device every 500 ms (#855)', async () => {
    await render();
    await settle(1100);
    expect(vi.mocked(bridge.pumpPollStatus).mock.calls.length).toBeGreaterThanOrEqual(4);
    expect(bridge.pumpPollStatus).toHaveBeenCalledWith(0);
    expect(bridge.pumpPollStatus).toHaveBeenCalledWith(1);
  });
  it('keeps Stop pump and Disable autofocus usable while another command is in flight (#855)', async () => {
    let finish: (value: ReturnType<typeof accepted>) => void = () => {};
    vi.mocked(bridge.pumpStart).mockReturnValue(new Promise(resolve => { finish = resolve; }));
    await render();
    await act(async () => button('Start pump').click());
    expect(button('Start pump').disabled).toBe(true);
    expect(button('Stop pump').disabled).toBe(false);
    expect(button('Disable autofocus').disabled).toBe(false);
    await act(async () => button('Stop pump').click());
    await act(async () => button('Disable autofocus').click());
    expect(bridge.pumpStop).toHaveBeenCalledWith(0);
    expect(bridge.autofocusSetEnabled).toHaveBeenCalledWith(false);
    await act(async () => finish(accepted('Started')));
  });
  it('shows rejected commands as failures, never optimistic success', async () => {
    vi.mocked(bridge.pumpStart).mockResolvedValue({ok: false, command: 10, message: 'Device stalled', operation_id: '0'});
    await render();
    await act(async () => button('Start pump').click());
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Device stalled');
    expect(append).not.toHaveBeenCalledWith('Start pump: Started');
  });
  it('blocks motion and configuration when status cannot be read', async () => {
    vi.mocked(bridge.fetchPumpStatus).mockRejectedValue(new Error('Transport lost'));
    await render();
    expect(button('Start pump').disabled).toBe(true);
    expect(button('Apply autofocus configuration').disabled).toBe(true);
    expect(button('Jog voltage +').disabled).toBe(true);
    expect(button('Stop pump').disabled).toBe(false);
    expect(host.textContent).toContain('Transport lost');
  });
  it('applies rate and direction 300 ms after an edit, also while running, and saves them (HW-008, HW-016)', async () => {
    vi.mocked(bridge.fetchPumpStatus).mockResolvedValue({...pump, run_status: 1});
    await render();
    const rate = labelled('Flow rate')[0];
    expect(rate.disabled).toBe(false);
    expect(rate.max).toBe('100000');
    await typeInto(rate, '12.345678');
    await act(async () => rate.dispatchEvent(new FocusEvent('focusout', {bubbles: true})));
    expect(bridge.pumpSetFlowRate).not.toHaveBeenCalled();
    await settle(350);
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledWith(0, 12.3457, 100);
    expect(bridge.pumpSetDirection).toHaveBeenCalledWith(0, 0);
    await act(async () => choose(labelled('Direction')[0], '1'));
    await settle(350);
    expect(bridge.pumpSetDirection).toHaveBeenLastCalledWith(0, 1);
    expect(bridge.setPreferences).toHaveBeenLastCalledWith({hardware: {pumps: [{rate: '12.345678', unit: 100, direction: 1}, {}]}});
  });
  it('restores the saved rate, unit and direction at start (HW-016)', async () => {
    vi.mocked(bridge.getPreferences).mockResolvedValue({hardware: {pumps: [{rate: '7.5', unit: 103, direction: 1}, {}]}});
    await render();
    await settle();
    expect(labelled('Flow rate')[0].value).toBe('7.5');
    expect((labelled('Rate unit')[0] as HTMLSelectElement).value).toBe('103');
    expect((labelled('Direction')[0] as HTMLSelectElement).value).toBe('1');
    expect(labelled('Flow rate')[1].value).toBe('1'); // no saved value: the pump's configured rate
  });
  it('purges only while the button is held (HW-012)', async () => {
    await render({mode: 'operator', armed: false});
    const purge = button('Purge infuse (hold)');
    expect(purge.title).toBe('Run pump at full speed (hold to purge)');
    await act(async () => purge.dispatchEvent(new Event('pointerdown', {bubbles: true})));
    expect(bridge.pumpPurge).toHaveBeenCalledWith(0, 0);
    expect(bridge.pumpStopPurge).not.toHaveBeenCalled();
    await act(async () => purge.dispatchEvent(new Event('pointerup', {bubbles: true})));
    await settle();
    expect(bridge.pumpStopPurge).toHaveBeenCalledWith(0);
  });
  it('rejects a blank flow rate before invoking the bridge', async () => {
    await render();
    const rate = labelled('Flow rate')[0];
    await settle();
    await typeInto(rate, '');
    await act(async () => rate.dispatchEvent(new FocusEvent('focusout', {bubbles: true})));
    await settle(350);
    expect(bridge.pumpSetFlowRate).not.toHaveBeenCalled();
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Flow rate');
  });
  it('sends a fractional mL syringe volume as whole µL (HW-019)', async () => {
    await render();
    await act(async () => choose(labelled('Volume unit')[0], '103'));
    const volume = labelled('Syringe volume')[0];
    await typeInto(volume, '2.5');
    await act(async () => volume.dispatchEvent(new FocusEvent('focusout', {bubbles: true})));
    await settle();
    expect(bridge.pumpSetSyringeVolume).toHaveBeenCalledWith(0, 2500, 100);
  });
  it('applies the target ring width live and saves it; jogs with the typed step (HW-030, HW-032)', async () => {
    vi.mocked(bridge.fetchAutofocusStatus).mockResolvedValue({valid: true, connected: true, enabled: true, current_voltage: 50, com_port: 3, average_ring_ratio: 1, median_ring_ratio: 1, last_ring_ratio_update_us: 0, ring_ratio_age_us: 0});
    await render({mode: 'operator', armed: false});
    const ring = labelled('Target ring width')[0];
    expect(ring.disabled).toBe(false);
    await typeInto(ring, '25.5');
    await settle(350);
    expect(bridge.autofocusSetConfig).toHaveBeenCalledWith({...config, focus_setpoint: 25.5});
    expect(bridge.setPreferences).toHaveBeenCalledWith({hardware: {focusSetpoint: 25.5}});
    await typeInto(labelled('Jog step')[0], '5');
    expect(button('Jog voltage +').disabled).toBe(false);
    await act(async () => button('Jog voltage +').click());
    await settle();
    expect(bridge.autofocusSetConfig).toHaveBeenLastCalledWith({...config, manual_voltage_step: 5});
    expect(bridge.autofocusJog).toHaveBeenCalledWith(true);
  });
  it('enables autofocus from the check box in Operator mode (HW-029)', async () => {
    await render({mode: 'operator', armed: false});
    const box = Array.from(host.querySelectorAll('label')).find(l => l.textContent === 'Enable Autofocus')!.querySelector('input')!;
    expect(box.disabled).toBe(false);
    await act(async () => box.click());
    expect(bridge.autofocusSetEnabled).toHaveBeenCalledWith(true);
  });
  it('connects a slot to the peristaltic pump with its calibration and default endpoint', async () => {
    vi.mocked(bridge.fetchPumpStatus).mockResolvedValue({...pump, connected: false});
    vi.mocked(bridge.pumpConnectModel).mockResolvedValue({ok: true, command: 10, message: 'Pump connected', operation_id: '0'});
    await render();
    await act(async () => choose(labelled('Pump model')[0], '1'));
    expect(labelled('System serial port')[0].value).toBe('/dev/ttyPS1');
    expect(labelled('Device address')[0].value).toBe('3');
    expect(labelled('Calibration (µL per revolution)')[0].value).toBe('25');
    await act(async () => button('Connect').click());
    expect(bridge.pumpConnectModel).toHaveBeenCalledWith(0, 1, '/dev/ttyPS1', 115200, 3, 25);
  });
  it('shows head speed and hides syringe volume for a connected peristaltic pump', async () => {
    vi.mocked(bridge.fetchPumpStatus).mockResolvedValue({...pump, model: 1, speed_rpm: 0.4, microliters_per_rev: 25, configured_flow_rate: 10});
    await render();
    expect(host.textContent).toContain('Head 0.40 rpm at 25 µL/rev');
    expect(labelled('Syringe volume')[0]).toBeUndefined();
    expect(labelled('Pump model')[0].disabled).toBe(true);
  });
  it('takes a peristaltic rate in head rpm and sends the equivalent flow in µL/min', async () => {
    vi.mocked(bridge.fetchPumpStatus).mockResolvedValue({...pump, model: 1, speed_rpm: 0.4, microliters_per_rev: 25, configured_flow_rate: 10, flow_rate_unit: 100});
    vi.mocked(bridge.pumpSetFlowRate).mockResolvedValue({ok: true, command: 10, message: 'Rate set', operation_id: '0'});
    await render();
    expect(host.textContent).toContain('configured ≈ 0.40 rpm');
    const unit = labelled('Rate unit')[0] as HTMLSelectElement;
    expect(Array.from(unit.options).map(o => o.textContent)).toContain('rpm (head)');
    await act(async () => choose(unit, '200'));
    const rate = labelled('Flow rate')[0] as HTMLInputElement;
    await act(async () => { Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(rate, '0.8'); rate.dispatchEvent(new Event('input', {bubbles: true})); });
    await act(async () => rate.dispatchEvent(new FocusEvent('focusout', {bubbles: true})));
    await settle(350);
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledWith(0, 20, 100); // 0.8 rpm x 25 µL/rev
    expect(host.textContent).toContain('Infuse (CCW)');
    expect(host.textContent).toContain('Withdraw (CW)');
  });
  it('offers no rpm unit for a syringe pump', async () => {
    await render();
    const unit = labelled('Rate unit')[0] as HTMLSelectElement;
    expect(Array.from(unit.options).map(o => o.textContent)).not.toContain('rpm (head)');
    expect(host.textContent).not.toContain('(CCW)');
  });
});
describe('PZ7035 pumps (#501)', () => {
  it('defaults both slots to the peristaltic pumps on ttyPS1 (Sample 3, Sheath 4) and hides the nanopositioner', async () => {
    vi.mocked(bridge.fetchPumpStatus).mockResolvedValue({...pump, connected: false});
    const capabilities = {
      instrument: 'pz7035' as const, autofocus: false, trigger: false, host_background: false, frame_buffer: false,
      reanalysis: false, core_updates: false, egrabber_script: false, pl_identity: true, led_strobe: true,
      align_mode: false, run_mode: false,
      pump: {model: 'tushui_peristaltic', port: '/dev/ttyPS1', sample_address: 3, sheath_address: 4, microliters_per_rev: 25},
    };
    await render({capabilities});
    expect(labelled('System serial port').map(input => input.value)).toEqual(['/dev/ttyPS1', '/dev/ttyPS1']);
    expect(labelled('Device address').map(input => input.value)).toEqual(['3', '4']);
    expect(labelled('Pump model').map(select => select.value)).toEqual(['1', '1']);
    expect(host.textContent).not.toContain('nanopositioner');
    expect(host.textContent).not.toContain('Pulse');
  });
});
