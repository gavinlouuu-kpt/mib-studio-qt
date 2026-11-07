// @vitest-environment jsdom
// HW-033 / HW-034 (epic #868): the shell routes the backend's autofocus failure events to the nanopositioner panel (besides the
// event log) and reads the configured staleness limit with the status.
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { COMMAND_TYPES } from '../../bridgeContract';
import { recentAutofocusMessages, resetAutofocusMessages } from '../../components/autofocusMessages';
import { bridge, type BridgeEvent } from '../../bridge';
import { invoke } from '../../transport';
import { useStatePolling } from './useShell';

vi.mock('@tauri-apps/api/app', () => ({getVersion: vi.fn()}));
vi.mock('../../transport', () => ({invoke: vi.fn(), isRemote: false, transport: {kind: 'tauri'}}));
vi.mock('../../transport/dialogs', () => ({setRemoteDefaultDir: vi.fn()}));
vi.mock('../../bridge', () => ({bridge: {pollEvents: vi.fn(), fetchProcessingStats: vi.fn(), fetchExperimentStatus: vi.fn(), fetchAutofocusStatus: vi.fn(), fetchAutofocusConfig: vi.fn()}}));

const error = (command: number, message: string): BridgeEvent => ({kind: 'BackendError', source: 7, command, message, textTruncated: false} as unknown as BridgeEvent);
let host: HTMLDivElement, root: Root;
const deps = () => ({append: vi.fn(), ready: true, tickBusy: {current: false}, setCamStatus: vi.fn(), setRunning: vi.fn(), setRange: vi.fn(), setRecording: vi.fn(), setStats: vi.fn(),
  setExpStatus: vi.fn(), setAfStatus: vi.fn(), setAfStaleMs: vi.fn()});
function Harness({value}: {value: ReturnType<typeof deps>}) { useStatePolling(value as never); return null; }
beforeEach(() => {
  Object.assign(globalThis, {IS_REACT_ACT_ENVIRONMENT: true});
  vi.useFakeTimers(); vi.clearAllMocks(); resetAutofocusMessages();
  vi.mocked(invoke).mockResolvedValue({capture_running: false, recording: false} as never);
  vi.mocked(bridge.pollEvents).mockResolvedValue([]);
  vi.mocked(bridge.fetchProcessingStats).mockResolvedValue({valid: false} as never);
  vi.mocked(bridge.fetchExperimentStatus).mockResolvedValue({valid: false} as never);
  vi.mocked(bridge.fetchAutofocusStatus).mockResolvedValue({valid: true} as never);
  vi.mocked(bridge.fetchAutofocusConfig).mockResolvedValue({valid: true, ring_ratio_stale_ms: 1500} as never);
  host = document.createElement('div'); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.useRealTimers(); });
const mount = async (value = deps()) => { await act(async () => root.render(<Harness value={value} />)); return value; };
const tick = async (ms: number) => act(async () => { await vi.advanceTimersByTimeAsync(ms); });

describe('autofocus failure events (HW-033)', () => {
  it('go to the event log as before and to the nanopositioner panel; other commands\' errors stay in the log only', async () => {
    vi.mocked(bridge.pollEvents).mockResolvedValueOnce([error(COMMAND_TYPES.Autofocus, 'Voltage change failed: write refused'), error(COMMAND_TYPES.Pump, 'pump went away')]);
    const value = await mount();
    expect(value.append).toHaveBeenCalledWith('backend error: Voltage change failed: write refused');
    expect(value.append).toHaveBeenCalledWith('backend error: pump went away');
    expect(recentAutofocusMessages().map(message => message.text)).toEqual(['Voltage change failed: write refused']);
  });
});

describe('staleness limit for the focus metric (HW-034)', () => {
  it('reads ring_ratio_stale_ms with the first status and then every tenth tick, and keeps the last value when the read fails', async () => {
    const value = await mount();
    expect(value.setAfStaleMs).toHaveBeenCalledTimes(1);
    expect(value.setAfStaleMs).toHaveBeenLastCalledWith(1500);
    vi.mocked(bridge.fetchAutofocusConfig).mockResolvedValue({valid: true, ring_ratio_stale_ms: 800} as never);
    await tick(500 * 9);
    expect(value.setAfStaleMs).toHaveBeenCalledTimes(1);
    await tick(500);
    expect(value.setAfStaleMs).toHaveBeenCalledTimes(2);
    expect(value.setAfStaleMs).toHaveBeenLastCalledWith(800);
    vi.mocked(bridge.fetchAutofocusConfig).mockRejectedValue(new Error('gone'));
    await tick(500 * 10);
    expect(value.setAfStaleMs).toHaveBeenCalledTimes(2);
    expect(value.append).not.toHaveBeenCalledWith(expect.stringContaining('tick error'));
  });
  it('ignores an invalid configuration', async () => {
    vi.mocked(bridge.fetchAutofocusConfig).mockResolvedValue({valid: false, ring_ratio_stale_ms: 0} as never);
    const value = await mount();
    expect(value.setAfStaleMs).not.toHaveBeenCalled();
  });
});
