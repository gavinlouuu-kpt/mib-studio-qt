// @vitest-environment jsdom
import {act} from 'react';
import {createRoot, type Root} from 'react-dom/client';
import {beforeEach, afterEach, it, expect, vi} from 'vitest';
import {BackgroundCalibrationControls} from './BackgroundCalibrationControls';
import {bridge} from '../bridge';
vi.mock('../bridge', () => ({bridge: {backgroundCalibrationStatus: vi.fn(), backgroundCalibrationCommand: vi.fn()}}));
let host: HTMLDivElement, root: Root;
const status = {valid: true, state: 'idle', operation_generation: '0', frozen_config_version: '0', attempted: 0, accepted: 0, rejected_non_empty: 0, rejected_processing_failed: 0, published_background_generation: '0', published_sha256: '', message: ''};
beforeEach(() => {Object.assign(globalThis, {IS_REACT_ACT_ENVIRONMENT: true}); vi.clearAllMocks(); vi.mocked(bridge.backgroundCalibrationStatus).mockResolvedValue(status); vi.mocked(bridge.backgroundCalibrationCommand).mockResolvedValue({ok: true, command: 2, message: 'Started', operation_id: '0'}); host = document.createElement('div'); document.body.append(host); root = createRoot(host);});
afterEach(async () => {await act(async () => root.unmount()); host.remove();});
async function render(active = false) {await act(async () => root.render(<BackgroundCalibrationControls ready experimentActive={active} />));}
it('only starts on explicit action with bounded request', async () => {await render(); expect(bridge.backgroundCalibrationCommand).not.toHaveBeenCalled(); await act(async () => host.querySelector('button')!.click()); expect(bridge.backgroundCalibrationCommand).toHaveBeenCalledWith({action: 'start', required_accepted: 10, max_attempts: 200, timeout_ms: 5000});});
it('can cancel running calibration while experiment becomes active', async () => {vi.mocked(bridge.backgroundCalibrationStatus).mockResolvedValue({...status, state: 'running'}); await render(true); const buttons = host.querySelectorAll('button'); expect(buttons[0].disabled).toBe(true); await act(async () => buttons[1].click()); expect(bridge.backgroundCalibrationCommand).toHaveBeenCalledWith({action: 'cancel'});});
it('retains explicit backend failure without claiming publication', async () => {vi.mocked(bridge.backgroundCalibrationCommand).mockResolvedValue({ok: false, command: 2, message: 'realtime processing is not running', operation_id: '0'}); await render(); await act(async () => host.querySelector('button')!.click()); expect(host.textContent).toContain('realtime processing is not running'); expect(host.textContent).not.toContain('Published generation');});
