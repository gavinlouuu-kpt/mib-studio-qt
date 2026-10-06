// @vitest-environment jsdom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { StageControls } from './StageControls';
import { bridge } from '../bridge';
import type { CmdResult, StageStatus } from '../bridge';
import { homed } from './stageTestFixtures';
import { LIMITS_UNVERIFIED_REASON, NOT_HOMED_REASON } from './stageControlModel';

vi.mock('../bridge', () => ({ bridge: {
  fetchStageStatus: vi.fn(), stageConnect: vi.fn(), stageDisconnect: vi.fn(), stageMoveTo: vi.fn(), stageMoveBy: vi.fn(),
  stageHome: vi.fn(), stageStop: vi.fn(), stageApplyProfile: vi.fn(), startDeviceDiscovery: vi.fn(),
  fetchDeviceDiscovery: vi.fn(), cancelDeviceDiscovery: vi.fn(),
} }));
const ok = (message: string): CmdResult => ({ ok: true, command: 13, message, operation_id: '1' });
const refused = (message: string): CmdResult => ({ ok: false, command: 13, message, operation_id: '0' });
const unhomed: Partial<StageStatus> = { referenced: false, position_um: -6564.99, span_um: 0, soft_min_um: 0, soft_max_um: 0 };
const status = (change: Partial<StageStatus> = {}): StageStatus => ({ ...homed, ...change });

let host: HTMLDivElement, root: Root;
const append = vi.fn(), onDisarm = vi.fn();
const button = (name: string) => Array.from(host.querySelectorAll('button')).find(node => node.textContent === name) as HTMLButtonElement;
const field = (label: string) => Array.from(host.querySelectorAll('label')).find(node => node.textContent?.startsWith(label))!.querySelector('input, select') as HTMLInputElement;
function type(input: HTMLInputElement, value: string) {
  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, value);
  input.dispatchEvent(new Event('input', { bubbles: true }));
}
const stageCommands = () => [bridge.stageConnect, bridge.stageDisconnect, bridge.stageMoveTo, bridge.stageMoveBy, bridge.stageHome, bridge.stageApplyProfile];
async function render(overrides: Record<string, unknown> = {}) {
  await act(async () => root.render(<StageControls ready experimentActive={false} append={append} mode="service" armed onDisarm={onDisarm} {...overrides} />));
}
beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  vi.clearAllMocks();
  vi.mocked(bridge.fetchStageStatus).mockResolvedValue(status());
  for (const command of [bridge.stageConnect, bridge.stageDisconnect, bridge.stageMoveTo, bridge.stageMoveBy, bridge.stageHome, bridge.stageApplyProfile]) vi.mocked(command).mockResolvedValue(ok('Accepted'));
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
  it('connecting never homes or moves, and does not consume the arming', async () => {
    vi.mocked(bridge.fetchStageStatus).mockResolvedValue(status({ connected: false }));
    await render();
    await act(async () => button('Connect stage').click());
    expect(bridge.stageConnect).toHaveBeenCalledWith('', '', 1);
    for (const command of [bridge.stageHome, bridge.stageMoveTo, bridge.stageMoveBy]) expect(command).not.toHaveBeenCalled();
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

describe('stage panel: Home', () => {
  it('asks for confirmation and only homes after the travel is declared clear', async () => {
    await render();
    await act(async () => button('Home…').click());
    expect(host.textContent).toContain('current focus position will be lost');
    expect(bridge.stageHome).not.toHaveBeenCalled();
    expect(button('Start Home').disabled).toBe(true);
    await act(async () => button('Start Home').click());
    expect(bridge.stageHome).not.toHaveBeenCalled();
    await act(async () => (host.querySelector('input[type="checkbox"]') as HTMLInputElement).click());
    expect(button('Start Home').disabled).toBe(false);
    await act(async () => button('Start Home').click());
    expect(bridge.stageHome).toHaveBeenCalledOnce();
    expect(onDisarm).toHaveBeenCalledOnce();
    expect(host.textContent).not.toContain('current focus position will be lost');
  });
  it('lets the operator back out without sending anything', async () => {
    await render();
    await act(async () => button('Home…').click());
    await act(async () => button('Cancel').click());
    expect(host.textContent).not.toContain('current focus position will be lost');
    expect(bridge.stageHome).not.toHaveBeenCalled();
  });
  it('stays disabled until the limit switches are verified, and says how', async () => {
    vi.mocked(bridge.fetchStageStatus).mockResolvedValue(status({ ...unhomed, limits_verified: false }));
    await render();
    expect(button('Home…').disabled).toBe(true);
    expect(host.textContent).toContain(LIMITS_UNVERIFIED_REASON);
    expect(host.textContent).toContain('zc300ctl verify-limits --supervised');
  });
  it('needs Service mode and arming', async () => {
    await render({ mode: 'operator', armed: false });
    expect(button('Home…').disabled).toBe(true);
    await render({ mode: 'service', armed: false });
    expect(button('Home…').disabled).toBe(true);
    await render({ mode: 'service', armed: true });
    expect(button('Home…').disabled).toBe(false);
  });
  it('shows a refused Home as a failure, never as success', async () => {
    vi.mocked(bridge.stageHome).mockResolvedValue(refused('Home refused: limit switches not verified'));
    await render();
    await act(async () => button('Home…').click());
    await act(async () => (host.querySelector('input[type="checkbox"]') as HTMLInputElement).click());
    await act(async () => button('Start Home').click());
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('limit switches not verified');
    expect(append).not.toHaveBeenCalledWith(expect.stringMatching(/^Home stage: Accepted/));
  });
});

describe('stage panel: position and moves', () => {
  it('shows the counter as unknown, never as a position, before Home', async () => {
    vi.mocked(bridge.fetchStageStatus).mockResolvedValue(status(unhomed));
    await render();
    expect(host.querySelector('[aria-label="Stage position"]')?.textContent).toBe('unknown until Home (controller counter -6565.0 µm)');
    expect(host.textContent).toContain('Not homed');
    expect(button('Move +10 µm').disabled).toBe(true);
    expect(button('Go to position').disabled).toBe(true);
    expect(host.textContent).toContain(NOT_HOMED_REASON);
  });
  it('shows the homed position and soft limits', async () => {
    await render();
    expect(host.querySelector('[aria-label="Stage position"]')?.textContent).toBe('120.4 µm');
    expect(host.textContent).toContain('soft limits -2900 to 2900 µm');
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
    for (const [text, message] of [['12.5', 'whole number of micrometres'], ['', 'whole number of micrometres'], ['3000', 'outside the soft limits'], ['-2901', 'outside the soft limits']]) {
      await act(async () => type(field('Go to position'), text));
      await act(async () => button('Go to position').click());
      expect(host.querySelector('[role="alert"]')?.textContent).toContain(message);
    }
    expect(bridge.stageMoveTo).not.toHaveBeenCalled();
    expect(onDisarm).not.toHaveBeenCalled(); // a rejected input must not consume the arming
  });
  it('rejects a jog that would leave the soft limits', async () => {
    vi.mocked(bridge.fetchStageStatus).mockResolvedValue(status({ position_um: 2895 }));
    await render();
    await act(async () => button('Move +10 µm').click());
    expect(bridge.stageMoveBy).not.toHaveBeenCalled();
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('outside the soft limits');
  });
  it('shows a refused move as a failure', async () => {
    vi.mocked(bridge.stageMoveBy).mockResolvedValue(refused('Move refused: outside the soft limits'));
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
    expect(button('Home…').disabled).toBe(true);
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

describe('stage panel: Stop and the experiment lock', () => {
  it('locks everything but Stop during an experiment', async () => {
    await render({ experimentActive: true });
    for (const name of ['Connect stage', 'Disconnect stage', 'Home…', 'Move +10 µm', 'Go to position']) expect(button(name).disabled).toBe(true);
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
