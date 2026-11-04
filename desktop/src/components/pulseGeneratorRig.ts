// "Use XGC + R5D preset for Live View" (epic #868, HW-057). Qt (ConfigTabs.cpp:571-633) rewrites the MindVision
// camera JSON in its editor with the rig preset and a `live_view` block naming this pulse generator, then saves it for
// the next Live View. The desktop edits the file the selected MindVision camera was applied from, through the same
// revision-checked `camera_document` command as the camera document editor (cameraDocument.tsx), then applies it with
// the same `select_mindvision_camera` call the editor's Apply uses. Nothing here starts capture or drives the generator.
import {bridge} from '../bridge';
import {CAMERA_SELECTION_MODES} from '../bridgeContract';
import {invoke} from '../transport';
import {confirm} from '../transport/dialogs';
import {numericInput} from './hardwareControlModel';
import {PULSE_LIMITS, type PulseForm} from './pulseGeneratorModel';

/** The XGC + R5D rig's camera settings (ConfigTabs.cpp:597-612): 512x96, hardware-triggered, one frame per rising edge. */
export const RIG_CAMERA_PRESET = {
  width: 512, height: 96, offset_x: 0, offset_y: 0, exposure_time_us: 100, auto_exposure_enabled: false,
  trigger_mode: 2, ext_trig_signal_type: 0, trigger_count: 1, ext_trig_jitter_us: 0, acq_trigger_delay_us: 0,
  strobe_mode: 1, strobe_delay_us: 0, strobe_pulse_width_us: 100, strobe_polarity: 0,
} as const;
/** 1000 Hz with 2 % duty is the 20 us trigger pulse the rig measured; the document's channel is 1-based. */
export const RIG_LIVE_VIEW_RATE = {frequency_hz: 1000, duty_percent: 2} as const;
/** After saving, Qt resets the generator's own Frequency / Duty fields to the bench values. */
export const RIG_BENCH_FIELDS = {frequency: '5000', duty: '10'} as const;
export const RIG_PRESET_TOOLTIP = 'Saves the selected generator connection and channel with the rig preset (1000 Hz rising-edge trigger, 20 us pulse). Does not start hardware.';

/** The camera JSON with the rig preset and the generator link merged in; every other key is kept. */
export function rigDocument(text: string, form: PulseForm): string {
  let document: unknown;
  try { document = JSON.parse(text); } catch { throw new Error('Fix the camera JSON before saving the preset.'); }
  if (!document || typeof document !== 'object' || Array.isArray(document)) throw new Error('Fix the camera JSON before saving the preset.');
  const port = form.port.trim();
  if (!port) throw new Error('Select the generator\'s serial port first.');
  const {address} = PULSE_LIMITS;
  const liveView = {
    enabled: true, port, address: numericInput(form.address, 'Address', address.min, address.max, true), channel: form.channel + 1,
    ...RIG_LIVE_VIEW_RATE, baud: form.baud, data_bits: form.dataBits, parity: form.parity, stop_bits: form.stopBits,
  };
  return JSON.stringify({...document, ...RIG_CAMERA_PRESET, live_view: liveView}, null, 2) + '\n';
}

interface CameraFile {path: string; text: string; revision: string}

/** Returns the message to log, or null when the operator declined. Throws with the reason when it cannot proceed. */
export async function saveRigPreset(form: PulseForm, experimentActive: boolean): Promise<string | null> {
  if (!form.port.trim()) throw new Error('Select the generator\'s serial port first.');
  if (experimentActive) throw new Error('Finalize the experiment before changing camera settings.');
  const selected = await bridge.fetchCameraSelection();
  if (!selected.valid || !selected.configured || selected.mode !== CAMERA_SELECTION_MODES.MindVision) throw new Error('Select a MindVision camera on the Connect stage first.');
  if (selected.running) throw new Error('Stop Live View before saving setup.');
  if (!selected.mindvision_config_path) throw new Error('The selected MindVision camera has no configuration file yet: load one in the MindVision JSON editor and Apply it first.');
  const path = selected.mindvision_config_path;
  if (!await confirm(`Replace the rig settings in ${path} with the XGC + R5D preset and this generator's connection? Other settings in the file are kept.`)) return null;
  const file = await invoke<CameraFile>('camera_document', {action: 'read', path, kind: 'json', baseline: '', text: ''});
  const saved = await invoke<CameraFile>('camera_document', {action: 'save', path: file.path, kind: 'json', baseline: file.revision, text: rigDocument(file.text, form)});
  const applied = await bridge.selectMindVisionCamera(selected.mindvision_index, selected.label, saved.path);
  if (!applied.ok) throw new Error(`Saved ${saved.path}, but the camera did not accept it: ${applied.message}`);
  return `MindVision rig preset (XGC + R5D) saved to ${saved.path} with generator ${form.port.trim()} address ${form.address} channel ${form.channel + 1}; not started`;
}
