// @vitest-environment jsdom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { StageControls } from './StageControls';
import { bridge } from '../bridge';
import type { CmdResult, StageStatus } from '../bridge';
import { zeroed } from './stageTestFixtures';
import { LIMITS_UNVERIFIED_NOTE, NO_ZERO_REASON } from './stageControlModel';

vi.mock('../bridge', () => ({ bridge: {
  fetchStageStatus: vi.fn(), stageConnect: vi.fn(), stageDisconnect: vi.fn(), stageMoveTo: vi.fn(), stageMoveBy: vi.fn(),
  stageSetZero: vi.fn(), stageStop: vi.fn(), stageApplyProfile: vi.fn(), startDeviceDiscovery: vi.fn(),
  fetchDeviceDiscovery: vi.fn(), cancelDeviceDiscovery: vi.fn(),
} }));
const ok = (message: string): CmdResult => ({ ok: true, command: 13, message, operation_id: '1' });
const refused = (message: string): CmdResult => ({ ok: false, command: 13, message, operation_id: '0' });
const noZero: Partial<StageStatus> = { zero_set: false, position_um: -6564.99, envelope_min_um: 0, envelope_max_um: 0 };
const status = (change: Partial<StageStatus> = {}): StageStatus => ({ ...zeroed, ...change });

let host: HTMLDivElement, root: Root;
const append = vi.fn(), onDisarm = vi.fn();
const button = (name: string) => Array.from(host.querySelectorAll('button')).find(node => node.textContent === name) as HTMLButtonElement;
const field = (label: string) => Array.from(host.querySelectorAll('label')).find(node => node.textContent?.startsWith(label))!.querySelector('input, select') as HTMLInputElement;
function type(input: HTMLInputElement, value: string) {
  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, value);
  input.dispatchEvent(new Event('input', { bubbles: true }));
}
const stageCommands = () => [bridge.stageConnect, bridge.stageDisconnect, bridge.stageMoveTo, bridge.stageMoveBy, bridge.stageSetZero, bridge.stageApplyProfile];
async function render(overrides: Record<string, unknown> = {}) {
  await act(async () => root.render(<StageControls ready experimentActive={false} append={append} mode="service" armed onDisarm={onDisarm} {...overrides} />));
}
beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  vi.clearAllMocks();
  vi.mocked(bridge.fetchStageStatus).mockResolvedValue(status());
  for (const command of [bridge.stageConnect, bridge.stageDisconnect, bridge.stageMoveTo, bridge.stageMoveBy, bridge.stageSetZero, bridge.stageApplyProfile]) vi.mocked(command).mockResolvedValue(ok('Accepted'));
  vi.mocked(bridge.stageStop).mockResolvedValue(ok('Stage stopped'));
  host = document.createElement('div'); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe('stage panel: nothing moves by itself', () => {
  it('reads status on mount and sends no command', async () => {
    await render();
    expect(bridge.fetchStageStatus).toHaveBeenCalled();
    for (const command of stageCommands()) expect(command).not.toHaveBeenCalled();
    expect(bridge.stageStop).not.toHaveBeenCalled();
  });
  it('connecting never moves or sets zero, and does not consume the arming', async () => {
    vi.mocked(bridge.fetchStageStatus).mockResolvedValue(status({ connected: false }));
    await render();
    await act(async () => button('Connect stage').click());
    expect(bridge.stageConnect).toHaveBeenCalledWith('', '', 1);
    for (const command of [bridge.stageSetZero, bridge.stageMoveTo, bridge.stageMoveBy]) expect(command).not.toHaveBeenCalled();
    expect(onDisarm).not.toHaveBeenCalled();
  });
  it('passes the entered endpoint to Connect', async () => {
    vi.mocked(bridge.fetchStageStatus).mockResolvedValue(status({ connected: false }));
    await render();
    await act(async () => { type(field('System serial port'), '/dev/ttyUSB1'); type(field('USB serial number'), 'A10RB8XC'); type(field('Modbus address'), '2'); });
    await act(async () => button('Connect stage').click());
    expect(bridge.stageConnect).toHaveBeenCalledWith('/dev/ttyUSB1', 'A10RB8XC', 2);
    expect(append).toHaveBeenCalledWith('Connect stage: Accepted');
  });
  it('cannot connect twice', async () => {
    await render();
    expect(button('Connect stage').disabled).toBe(true);
    expect(button('Disconnect stage').disabled).toBe(false);
  });
});

describe('stage panel: Set zero (there is no Home)', () => {
  it('offers no Home control anywhere', async () => {
    await render();
    expect(Array.from(host.querySelectorAll('button')).some(node => /home/i.test(node.textContent ?? ''))).toBe(false);
    expect(host.textContent).not.toMatch(/Home sensor/);
    expect(host.textContent).toContain('never homed');
  });
  it('asks for confirmation, warns that nothing checks the position, and sends nothing until confirmed', async () => {
    await render();
    await act(async () => button('Set zero here…').click());
    expect(host.textContent).toContain('does not move the stage');
    expect(host.textContent).toContain('hand move, a stall or a collision');
    expect(bridge.stageSetZero).not.toHaveBeenCalled();
    expect(onDisarm).not.toHaveBeenCalled();
    await act(async () => button('Set zero here').click());
    expect(bridge.stageSetZero).toHaveBeenCalledOnce();
    expect(bridge.stageSetZero).toHaveBeenCalledWith(false); // no mid-travel declaration unless ticked
    expect(onDisarm).toHaveBeenCalledOnce();
    expect(host.textContent).not.toContain('does not move the stage. Nothing checks');
  });
  it('declares mid-travel only when the operator ticks it, and not for the next time', async () => {
    await render();
    await act(async () => button('Set zero here…').click());
    await act(async () => (host.querySelector('input[type="checkbox"]') as HTMLInputElement).click());
    await act(async () => button('Set zero here').click());
    expect(bridge.stageSetZero).toHaveBeenLastCalledWith(true);
    await act(async () => button('Set zero here…').click());
    expect((host.querySelector('input[type="checkbox"]') as HTMLInputElement).checked).toBe(false);
    await act(async () => button('Set zero here').click());
    expect(bridge.stageSetZero).toHaveBeenLastCalledWith(false);
  });
  it('lets the operator back out without sending anything or using the arming', async () => {
    await render();
    await act(async () => button('Set zero here…').click());
    await act(async () => (host.querySelector('input[type="checkbox"]') as HTMLInputElement).click());
    await act(async () => button('Cancel').click());
    expect(host.textContent).not.toContain('hand move, a stall or a collision');
    expect(bridge.stageSetZero).not.toHaveBeenCalled();
    expect(onDisarm).not.toHaveBeenCalled();
    await act(async () => button('Set zero here…').click());
    expect((host.querySelector('input[type="checkbox"]') as HTMLInputElement).checked).toBe(false); // the declaration did not survive
  });
  it('works without verified limit switches (the wiring is a badge, not a gate)', async () => {
    vi.mocked(bridge.fetchStageStatus).mockResolvedValue(status({ ...noZero, limits_verified: false }));
    await render();
    expect(button('Set zero here…').disabled).toBe(false);
    expect(host.textContent).toContain(LIMITS_UNVERIFIED_NOTE);
  });
  it('needs Service mode and arming', async () => {
    await render({ mode: 'operator', armed: false });
    expect(button('Set zero here…').disabled).toBe(true);
    await render({ mode: 'service', armed: false });
    expect(button('Set zero here…').disabled).toBe(true);
    await render({ mode: 'service', armed: true });
    expect(button('Set zero here…').disabled).toBe(false);
  });
  it('shows a refused Set zero as a failure, never as success', async () => {
    vi.mocked(bridge.stageSetZero).mockResolvedValue(refused('Set zero refused: the stage is outside the window set by the first zero of this power-up'));
    await render();
    await act(async () => button('Set zero here…').click());
    await act(async () => button('Set zero here').click());
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('outside the window');
    expect(append).not.toHaveBeenCalledWith(expect.stringMatching(/^Set zero: Accepted/));
  });
});

describe('stage panel: position and moves', () => {
  it('shows the counter as unknown, never as a position, before zero is set', async () => {
    vi.mocked(bridge.fetchStageStatus).mockResolvedValue(status(noZero));
    await render();
    expect(host.querySelector('[aria-label="Stage position"]')?.textContent).toBe('unknown until zero is set (controller counter -6565.0 µm)');
    expect(host.textContent).toContain('Zero not set');
    expect(button('Move +10 µm').disabled).toBe(true);
    expect(button('Go to position').disabled).toBe(true);
    expect(host.textContent).toContain(NO_ZERO_REASON);
  });
  it('shows the zeroed position and the travel around it', async () => {
    await render();
    expect(host.querySelector('[aria-label="Stage position"]')?.textContent).toBe('120.4 µm');
    expect(host.textContent).toContain('travel -1000 to 1000 µm (mid-travel not declared)');
    expect(host.textContent).toContain(LIMITS_UNVERIFIED_NOTE);
  });
  it('shows the wider travel after a mid-travel declaration', async () => {
    vi.mocked(bridge.fetchStageStatus).mockResolvedValue(status({ mid_travel_declared: true, envelope_min_um: -2900, envelope_max_um: 2900 }));
    await render();
    expect(host.textContent).toContain('travel -2900 to 2900 µm (mid-travel declared)');
  });
  it('jogs by the chosen whole-micrometre step and consumes the arming each time', async () => {
    await render();
    await act(async () => button('Move +10 µm').click());
    expect(bridge.stageMoveBy).toHaveBeenLastCalledWith(10);
    await act(async () => button('Move −10 µm').click());
    expect(bridge.stageMoveBy).toHaveBeenLastCalledWith(-10);
    expect(onDisarm).toHaveBeenCalledTimes(2);
    await act(async () => { const select = field('Step') as unknown as HTMLSelectElement; Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value')!.set!.call(select, '100'); select.dispatchEvent(new Event('change', { bubbles: true })); });
    await act(async () => button('Move +100 µm').click());
    expect(bridge.stageMoveBy).toHaveBeenLastCalledWith(100);
  });
  it('goes to an absolute position', async () => {
    await render();
    await act(async () => type(field('Go to position'), '-500'));
    await act(async () => button('Go to position').click());
    expect(bridge.stageMoveTo).toHaveBeenCalledWith(-500);
  });
  it('rejects fractional, blank and out-of-range targets before calling the bridge', async () => {
    await render();
    for (const [text, message] of [['12.5', 'whole number of micrometres'], ['', 'whole number of micrometres'], ['1001', 'outside the allowed travel'], ['-1001', 'outside the allowed travel']]) {
      await act(async () => type(field('Go to position'), text));
      await act(async () => button('Go to position').click());
      expect(host.querySelector('[role="alert"]')?.textContent).toContain(message);
    }
    expect(bridge.stageMoveTo).not.toHaveBeenCalled();
    expect(onDisarm).not.toHaveBeenCalled(); // a rejected input must not consume the arming
  });
  it('rejects a jog that would leave the travel envelope', async () => {
    vi.mocked(bridge.fetchStageStatus).mockResolvedValue(status({ position_um: 995 }));
    await render();
    await act(async () => button('Move +10 µm').click());
    expect(bridge.stageMoveBy).not.toHaveBeenCalled();
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('outside the allowed travel');
    expect(onDisarm).not.toHaveBeenCalled();
  });
  it('shows a refused move as a failure', async () => {
    vi.mocked(bridge.stageMoveBy).mockResolvedValue(refused('Move refused: outside the travel envelope'));
    await render();
    await act(async () => button('Move +10 µm').click());
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Move refused');
  });
  it('refuses motion on an emergency stop or driver alarm and shows the indicators', async () => {
    vi.mocked(bridge.fetchStageStatus).mockResolvedValue(status({ emergency_stop: true, driver_alarm: true, limit_negative: true }));
    await render();
    expect(host.textContent).toContain('Emergency stop: ACTIVE');
    expect(host.textContent).toContain('Driver alarm: ACTIVE');
    expect(host.textContent).toContain('Limit −: ACTIVE');
    expect(button('Move +10 µm').disabled).toBe(true);
    expect(button('Set zero here…').disabled).toBe(true);
  });
  it('asks for the stage settings to be applied when the controller does not match', async () => {
    vi.mocked(bridge.fetchStageStatus).mockResolvedValue(status({ configured: false }));
    await render();
    expect(host.textContent).toContain('motion is refused until you apply the stage settings');
    expect(button('Move +10 µm').disabled).toBe(true);
    await act(async () => button('Apply stage settings').click());
    expect(bridge.stageApplyProfile).toHaveBeenCalledOnce();
    expect(onDisarm).not.toHaveBeenCalled();
  });
});

describe('stage panel: session-only zero warning', () => {
  it('is silent in normal operation', async () => {
    await render();
    expect(host.textContent).not.toContain('Hardware-acceptance mode');
  });
  it('warns, as an alert, when power cycles are not detected', async () => {
    vi.mocked(bridge.fetchStageStatus).mockResolvedValue(status({ session_only_zero: true }));
    await render();
    expect(host.querySelector('.stage-session-only')?.textContent).toContain('power cycle of the controller is NOT detected');
    expect(host.querySelector('.stage-session-only')?.getAttribute('role')).toBe('alert');
  });
});

describe('stage panel: Stop and the experiment lock', () => {
  it('locks everything but Stop during an experiment', async () => {
    await render({ experimentActive: true });
    for (const name of ['Connect stage', 'Disconnect stage', 'Set zero here…', 'Move +10 µm', 'Go to position']) expect(button(name).disabled).toBe(true);
    expect(button('Stop stage').disabled).toBe(false);
    await act(async () => button('Stop stage').click());
    expect(bridge.stageStop).toHaveBeenCalledOnce();
    expect(append).toHaveBeenCalledWith('Stop stage: Stage stopped');
  });
  it('can always stop, even with the status unreadable and in operator mode', async () => {
    vi.mocked(bridge.fetchStageStatus).mockRejectedValue(new Error('Transport lost'));
    await render({ mode: 'operator', armed: false });
    expect(host.textContent).toContain('Transport lost');
    expect(button('Stop stage').disabled).toBe(false);
    await act(async () => button('Stop stage').click());
    expect(bridge.stageStop).toHaveBeenCalledOnce();
  });
  it('stops while another command is still in flight', async () => {
    let finish!: (result: CmdResult) => void;
    vi.mocked(bridge.stageMoveBy).mockReturnValue(new Promise<CmdResult>(resolve => { finish = resolve; }));
    await render();
    await act(async () => button('Move +10 µm').click());
    expect(button('Move +10 µm').disabled).toBe(true); // the panel serializes its own commands...
    expect(button('Stop stage').disabled).toBe(false); // ...but never Stop
    await act(async () => button('Stop stage').click());
    expect(bridge.stageStop).toHaveBeenCalledOnce();
    await act(async () => finish(ok('Started')));
  });
  it('reports a Stop that the backend could not deliver', async () => {
    vi.mocked(bridge.stageStop).mockResolvedValue(refused('Stop: transport error'));
    await render();
    await act(async () => button('Stop stage').click());
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('transport error');
  });
  it('allows Disconnect while a move runs, but not Apply', async () => {
    vi.mocked(bridge.fetchStageStatus).mockResolvedValue(status({ busy: true, configured: false }));
    await render();
    expect(button('Disconnect stage').disabled).toBe(false);
    expect(button('Apply stage settings').disabled).toBe(true);
    expect(button('Move +10 µm').disabled).toBe(true);
    expect(button('Stop stage').disabled).toBe(false);
  });
});

describe('stage panel: endpoint discovery', () => {
  it('needs an explicit port to scan, and asks for the MotionStage kind with a serial scope', async () => {
    vi.mocked(bridge.fetchStageStatus).mockResolvedValue(status({ connected: false }));
    vi.mocked(bridge.startDeviceDiscovery).mockRejectedValue(new Error('scan stopped for the test'));
    await render();
    await act(async () => button('Find endpoints').click());
    expect(bridge.startDeviceDiscovery).not.toHaveBeenCalled();
    expect(host.textContent).toContain('Enter the serial port to scan first');
    await act(async () => type(field('System serial port'), '/dev/ttyUSB1'));
    await act(async () => button('Find endpoints').click());
    expect(bridge.startDeviceDiscovery).toHaveBeenCalledWith({ kinds: [4], hasSerialScope: true, serialPortName: '/dev/ttyUSB1', baudRate: 115200, addressFrom: 1, addressTo: 1, origin: 'tauri-stage-picker' });
    for (const command of stageCommands()) expect(command).not.toHaveBeenCalled(); // scanning never connects or moves
  });
});
