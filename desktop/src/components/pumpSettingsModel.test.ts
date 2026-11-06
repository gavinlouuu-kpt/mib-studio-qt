import { describe, expect, it } from 'vitest';
import type { PlatformCapabilities, SerialPortEntry } from '../bridge';
import {
  BOOT_SERVICE_OPTIONS, NO_PORT_MESSAGE, bootChecks, bootServicesToSave, comNumber, connectionOf, draftOf, instrumentFallback, listedName, planApply, portChoices, portName,
  portOption, pumpKey, savedBootServices, savedPump, scanMessage, type PumpDraft,
} from './pumpSettingsModel';

const entry = (system_name: string, extra: Partial<SerialPortEntry> = {}): SerialPortEntry => ({system_name, system_location: `/dev/${system_name}`, description: '', manufacturer: '', serial_number: '', vendor_id: 0, product_id: 0, ...extra});
const draft = (change: Partial<PumpDraft> = {}): PumpDraft => ({port: 'COM3', baud: '115200', address: '1', volume: '', volumeUnit: 100, ...change});

describe('saved pump settings (HW-003)', () => {
  it('reads Qt\'s keys per pump, and the exact port text before the COM number', () => {
    const settings = {pump_sample_com_port: 3, pump_sample_baud_rate: 9600, pump_sample_address: 5, pump_sample_syringe_vol: 0.5, pump_sample_syringe_unit: 103,
      pump_sample_flow_rate: 12.5, pump_sample_flow_unit: 100, pump_sample_direction: 1, pump_sheath_port: '/dev/ttyUSB0', pump_sheath_com_port: 9};
    expect(savedPump(settings, 0)).toEqual({port: 'COM3', baud: 9600, address: 5, volume: 0.5, volumeUnit: 103, rate: 12.5, rateUnit: 100, direction: 1});
    expect(savedPump(settings, 1)).toMatchObject({port: '/dev/ttyUSB0', baud: null, address: null, volume: null, rate: null, direction: null});
  });
  it('is tolerant like Qt\'s loadConfig: a value that is not valid is null, never an exception', () => {
    expect(savedPump(undefined, 0)).toEqual({port: '', baud: null, address: null, volume: null, volumeUnit: null, rate: null, rateUnit: null, direction: null});
    expect(savedPump({pump_sample_port: '  ', pump_sample_com_port: 0, pump_sample_baud_rate: '9600', pump_sample_address: 248, pump_sample_syringe_vol: 0, pump_sample_flow_rate: -1,
      pump_sample_direction: 2, pump_sample_syringe_unit: 1.5}, 0)).toEqual({port: '', baud: null, address: null, volume: null, volumeUnit: null, rate: null, rateUnit: null, direction: null});
  });
  it('builds the connection from the saved values, the caller\'s fallback and Qt\'s defaults (115200 baud, Sample 1, Sheath 2)', () => {
    const none = savedPump({}, 0);
    expect(connectionOf(none, 0)).toEqual({port: '', baud: 115200, address: 1});
    expect(connectionOf(savedPump({}, 1), 1)).toEqual({port: '', baud: 115200, address: 2});
    expect(connectionOf(none, 0, {port: '/dev/ttyPS1', address: 3})).toEqual({port: '/dev/ttyPS1', baud: 115200, address: 3});
    expect(connectionOf(savedPump({pump_sample_port: 'COM4', pump_sample_address: 7}, 0), 0, {port: '/dev/ttyPS1', address: 3})).toEqual({port: 'COM4', baud: 115200, address: 7});
    expect(NO_PORT_MESSAGE).toBe('No COM port configured. Open Pump settings…');
  });
  it('knows COM numbers and keeps system paths as they are', () => {
    expect([comNumber('COM3'), comNumber('com12'), comNumber('3'), comNumber('/dev/ttyUSB0'), comNumber('COM0'), comNumber('COM70000'), comNumber('')]).toEqual([3, 12, 3, null, null, null, null]);
    expect([portName('3'), portName(' COM3 '), portName('/dev/ttyUSB0'), portName('ttyS1'), portName('')]).toEqual(['COM3', 'COM3', '/dev/ttyUSB0', 'ttyS1', '']);
    expect(pumpKey(0, 'com_port')).toBe('pump_sample_com_port'); expect(pumpKey(1, 'syringe_unit')).toBe('pump_sheath_syringe_unit');
  });
  it('fills the form from the saved values and the volume in the unit it was saved in', () => {
    expect(draftOf(savedPump({pump_sample_port: 'COM3', pump_sample_baud_rate: 9600, pump_sample_address: 4, pump_sample_syringe_vol: 2.5, pump_sample_syringe_unit: 103}, 0), 0))
      .toEqual({port: 'COM3', baud: '9600', address: '4', volume: '2.5', volumeUnit: 103});
    expect(draftOf(savedPump({}, 1), 1)).toEqual({port: '', baud: '115200', address: '2', volume: '', volumeUnit: 100});
    const capabilities = {pump: {model: 'tushui_peristaltic', port: '/dev/ttyPS1', sample_address: 3, sheath_address: 4, microliters_per_rev: 25}} as PlatformCapabilities;
    expect(instrumentFallback(capabilities, 1)).toEqual({port: '/dev/ttyPS1', address: 4});
    expect(instrumentFallback({pump: null} as PlatformCapabilities, 0)).toBeUndefined();
  });
});

describe('Apply is checked as a whole before anything is saved (HW-020)', () => {
  it('plans both pumps\' keys: Qt\'s COM number beside the exact port, baud, address and volume', () => {
    const planned = planApply([draft({volume: '0.5', volumeUnit: 103, baud: '9600', address: '5'}), draft({port: '/dev/ttyUSB0', volume: '100'})], true);
    expect(planned.patch).toEqual({
      pump_sample_port: 'COM3', pump_sample_com_port: 3, pump_sample_baud_rate: 9600, pump_sample_address: 5, pump_sample_syringe_vol: 0.5, pump_sample_syringe_unit: 103,
      pump_sheath_port: '/dev/ttyUSB0', pump_sheath_com_port: null, pump_sheath_baud_rate: 115200, pump_sheath_address: 1, pump_sheath_syringe_vol: 100, pump_sheath_syringe_unit: 100,
    });
    expect(planned.volumes).toEqual([{slot: 0, value: 500, unit: 100}, {slot: 1, value: 100, unit: 100}]);       // D9: 0.5 mL is sent as 500 µL
  });
  it('leaves the saved port alone when none is selected, as Qt writes it only for a selected row', () => {
    const planned = planApply([draft({port: ''}), draft({port: ''})], true);
    expect(Object.keys(planned.patch).filter(key => key.endsWith('port'))).toEqual([]);
  });
  it('names the pump and the problem, and refuses the whole Apply for one bad value', () => {
    expect(() => planApply([draft(), draft({baud: '1199'})], true)).toThrow(/^Sheath pump: Baud rate/);
    expect(() => planApply([draft({address: '248'}), draft()], true)).toThrow(/^Sample pump: Modbus address/);
    expect(() => planApply([draft({address: ''}), draft()], true)).toThrow(/^Sample pump: Modbus address/);
    expect(() => planApply([draft({volume: '1.25'}), draft()], true)).toThrow(/at most 1 decimal/);
    expect(() => planApply([draft({volume: '10000'}), draft()], true)).toThrow(/from 0.1 to 9999/);
    expect(() => planApply([draft(), draft({volume: '0.5', volumeUnit: 100})], true)).toThrow(/Sheath pump: .*cannot be sent exactly/);
    expect(() => planApply([draft({port: 'a\0b'}), draft()], true)).toThrow(/serial port name/);
  });
  it('ignores the volume for pumps without a syringe (the instrument\'s peristaltic pumps)', () => {
    const planned = planApply([draft({volume: 'junk'}), draft()], false);
    expect(Object.keys(planned.patch).some(key => key.includes('syringe'))).toBe(false);
    expect(planned.volumes).toEqual([null, null]);
  });
});

describe('serial ports (HW-004)', () => {
  const usb = entry('ttyUSB0', {description: 'FT232R USB UART', serial_number: 'A12345', vendor_id: 0x0403, product_id: 0x6001});
  it('labels a port as Qt does and offers a saved port that is not listed instead of replacing it', () => {
    expect(portOption(usb)).toBe('ttyUSB0 — FT232R USB UART, S/N A12345, 0403:6001');
    expect(portOption(entry('COM3'))).toBe('COM3');
    expect(portChoices([usb, entry('COM3')], 'COM3').map(c => [c.value, c.missing])).toEqual([['ttyUSB0', false], ['COM3', false]]);
    expect(portChoices([usb], 'COM9')).toEqual([{value: 'ttyUSB0', label: portOption(usb), missing: false}, {value: 'COM9', label: 'COM9 (saved, not found)', missing: true}]);
    expect(portChoices([usb], '')).toHaveLength(1);
  });
  it('finds the listed port a saved name stands for ("/dev/ttyUSB0" is "ttyUSB0", "com3" is "COM3")', () => {
    expect([listedName([usb, entry('COM3')], '/dev/ttyUSB0'), listedName([usb, entry('COM3')], 'com3'), listedName([usb], 'COM9')]).toEqual(['ttyUSB0', 'COM3', 'COM9']);
    expect(portChoices([usb], '/dev/ttyUSB0').some(c => c.missing)).toBe(false);
  });
});

describe('scan messages (HW-021, SyringePumpSettingsDialog::scanAddresses)', () => {
  it('uses Qt\'s wording', () => {
    expect(scanMessage({state: 'completed', found: [1, 2], error: ''}, 'COM3', 115200)).toEqual({text: 'Found 2 responsive address(es) on COM3.', problem: false});
    expect(scanMessage({state: 'completed', found: [], error: ''}, 'COM3', 9600)).toEqual({text: 'No pump address responded on COM3 at 9600 baud.', problem: false});
  });
  it('says why when the port could not be read instead of reporting an empty result', () => {
    expect(scanMessage({state: 'failed', found: [], error: 'could not open the port: port unavailable'}, 'COM9', 115200)).toEqual({text: 'Scan failed: could not open the port: port unavailable.', problem: true});
  });
});

describe('boot service toggles (HW-038)', () => {
  it('lists Qt\'s services except the update check, which this app never runs by itself', () => {
    expect(BOOT_SERVICE_OPTIONS.map(o => o.label)).toEqual(['SQLite service', 'HDF5 service', 'Processing service', 'Autofocus wiring', 'Trigger wiring', 'Capture service', 'Playback service']);
  });
  it('ticks what is saved, all of them for "all", and saves the ticked ones plus tokens it does not list', () => {
    expect(bootChecks(['trigger', 'sqlite'])).toMatchObject({trigger: true, sqlite: true, hdf5: false});
    expect(Object.values(bootChecks(['all'])).every(Boolean)).toBe(true);
    expect(savedBootServices({boot_disabled_services: ['trigger', 5, 'auto_update']})).toEqual(['trigger', 'auto_update']);
    expect(savedBootServices(undefined)).toEqual([]);
    const checks = {...bootChecks([]), autofocus: true, playback: true};
    expect(bootServicesToSave(checks, ['trigger', 'auto_update', 'dot_grid'])).toEqual(['autofocus', 'playback', 'auto_update', 'dot_grid']);
    expect(bootServicesToSave(bootChecks(['all']), ['all'])).toEqual(BOOT_SERVICE_OPTIONS.map(o => o.token));
    expect(bootServicesToSave(bootChecks([]), ['trigger'])).toEqual([]);
  });
});
