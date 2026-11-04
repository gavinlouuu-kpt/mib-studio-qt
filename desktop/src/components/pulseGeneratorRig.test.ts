import {beforeEach, describe, expect, it, vi} from 'vitest';
import {bridge} from '../bridge';
import {CAMERA_SELECTION_MODES} from '../bridgeContract';
import {invoke} from '../transport';
import {confirm} from '../transport/dialogs';
import {DEFAULT_PULSE_FORM, type PulseForm} from './pulseGeneratorModel';
import {RIG_CAMERA_PRESET, RIG_LIVE_VIEW_RATE, rigDocument, saveRigPreset} from './pulseGeneratorRig';
vi.mock('../bridge', () => ({bridge: {fetchCameraSelection: vi.fn(), selectMindVisionCamera: vi.fn()}}));
vi.mock('../transport', () => ({invoke: vi.fn()}));
vi.mock('../transport/dialogs', () => ({confirm: vi.fn()}));

const form: PulseForm = {...DEFAULT_PULSE_FORM, port: '/dev/ttyUSB0', address: '4', channel: 1, baud: 19200, dataBits: 7, parity: 'E', stopBits: 2};
const userDocument = {exposure_time_us: 2, analog_gain: 3, gamma: 100, trigger_mode: 0, strobe_polarity: 1, live_view: {enabled: false, port: 'auto', note: 'old'}};
const selection = {valid: true, configured: true, running: false, mode: CAMERA_SELECTION_MODES.MindVision, mindvision_index: 1, label: 'MV-1', mindvision_config_path: '/data/camera.json'};
beforeEach(() => {
  vi.clearAllMocks();
  vi.mocked(bridge.fetchCameraSelection).mockResolvedValue(selection as never);
  vi.mocked(bridge.selectMindVisionCamera).mockResolvedValue({ok: true, command: 0, operation_id: '0', message: 'applied'});
  vi.mocked(confirm).mockResolvedValue(true);
  vi.mocked(invoke).mockImplementation((async (command: string, args?: Record<string, unknown>) => {
    if (command !== 'camera_document') throw new Error(`unexpected ${command}`);
    if (args?.action === 'read') return {path: '/data/camera.json', text: JSON.stringify(userDocument), revision: 'r1'};
    return {path: args?.path, text: args?.text, revision: 'r2'};
  }) as never);
});

describe('rigDocument (HW-057)', () => {
  const merged = JSON.parse(rigDocument(JSON.stringify(userDocument), form));
  it('applies the rig preset over the camera settings and keeps the rest', () => {
    expect(merged).toMatchObject(RIG_CAMERA_PRESET);
    expect(merged).toMatchObject({width: 512, height: 96, trigger_mode: 2, ext_trig_signal_type: 0, strobe_polarity: 0, exposure_time_us: 100});
    expect(merged.analog_gain).toBe(3);
    expect(merged.gamma).toBe(100);
  });
  it('names this generator in live_view, with the 1-based channel and the 20 us trigger pulse', () => {
    expect(merged.live_view).toEqual({
      enabled: true, port: '/dev/ttyUSB0', address: 4, channel: 2, frequency_hz: RIG_LIVE_VIEW_RATE.frequency_hz, duty_percent: RIG_LIVE_VIEW_RATE.duty_percent,
      baud: 19200, data_bits: 7, parity: 'E', stop_bits: 2,
    });
    expect(1_000_000 / merged.live_view.frequency_hz * merged.live_view.duty_percent / 100).toBe(20);
  });
  it('refuses JSON that is not an object, a missing port and a bad address', () => {
    expect(() => rigDocument('{', form)).toThrow('Fix the camera JSON');
    expect(() => rigDocument('[]', form)).toThrow('Fix the camera JSON');
    expect(() => rigDocument('{}', {...form, port: ' '})).toThrow('serial port');
    expect(() => rigDocument('{}', {...form, address: '300'})).toThrow('Address');
  });
});

describe('saveRigPreset (HW-057)', () => {
  it('reads the selected camera file, saves it against its revision and applies it: one click', async () => {
    const message = await saveRigPreset(form, false);
    expect(confirm).toHaveBeenCalledOnce();
    expect(invoke).toHaveBeenNthCalledWith(1, 'camera_document', {action: 'read', path: '/data/camera.json', kind: 'json', baseline: '', text: ''});
    const save = vi.mocked(invoke).mock.calls[1][1] as {action: string; baseline: string; path: string; text: string};
    expect(save).toMatchObject({action: 'save', kind: 'json', path: '/data/camera.json', baseline: 'r1'});
    expect(JSON.parse(save.text).live_view.port).toBe('/dev/ttyUSB0');
    expect(bridge.selectMindVisionCamera).toHaveBeenCalledWith(1, 'MV-1', '/data/camera.json');
    expect(message).toContain('saved to /data/camera.json');
  });
  it.each([
    ['no port', {...form, port: ''}, false, selection, 'serial port'],
    ['an experiment', form, true, selection, 'Finalize the experiment'],
    ['no MindVision camera', form, false, {...selection, mode: CAMERA_SELECTION_MODES.Mock}, 'Select a MindVision camera'],
    ['a running camera', form, false, {...selection, running: true}, 'Stop Live View'],
    ['no config file', form, false, {...selection, mindvision_config_path: ''}, 'no configuration file'],
  ])('writes nothing with %s', async (_name, input, experimentActive, camera, reason) => {
    vi.mocked(bridge.fetchCameraSelection).mockResolvedValue(camera as never);
    await expect(saveRigPreset(input, experimentActive)).rejects.toThrow(reason);
    expect(invoke).not.toHaveBeenCalled();
    expect(bridge.selectMindVisionCamera).not.toHaveBeenCalled();
  });
  it('does nothing when the operator declines', async () => {
    vi.mocked(confirm).mockResolvedValue(false);
    expect(await saveRigPreset(form, false)).toBeNull();
    expect(invoke).not.toHaveBeenCalled();
  });
  it('does not overwrite a file that changed on disk', async () => {
    vi.mocked(invoke).mockImplementation((async (_command: string, args?: Record<string, unknown>) => {
      if (args?.action === 'read') return {path: '/data/camera.json', text: '{}', revision: 'r1'};
      throw new Error('Camera file changed on disk; reload and reconcile your draft');
    }) as never);
    await expect(saveRigPreset(form, false)).rejects.toThrow('changed on disk');
    expect(bridge.selectMindVisionCamera).not.toHaveBeenCalled();
  });
  it('says the file was saved when the camera then refuses it', async () => {
    vi.mocked(bridge.selectMindVisionCamera).mockResolvedValue({ok: false, command: 0, operation_id: '0', message: 'Requested FPS is too high'});
    await expect(saveRigPreset(form, false)).rejects.toThrow('Saved /data/camera.json, but the camera did not accept it: Requested FPS is too high');
  });
});
