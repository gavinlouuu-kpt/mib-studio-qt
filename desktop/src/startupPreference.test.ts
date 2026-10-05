import {describe, expect, it} from 'vitest';
import {defaultStartupPreference, loadStartupPreference, saveStartupPreference, startupPreferenceKey, validateStartupPreference} from './startupPreference';
describe('remembered startup preference', () => {
  it('roundtrips endpoint, vendor, baud and address without losing identity', () => {
    const values = new Map<string,string>();
    const storage = {getItem: (key:string) => values.get(key) ?? null, setItem: (key:string,value:string) => {values.set(key,value);}};
    const preference = {...defaultStartupPreference, backend: 'oeabt' as const, endpoint:'/dev/serial/by-id/test', baud:57600, address:7};
    saveStartupPreference(storage, preference);
    expect(loadStartupPreference(storage)).toEqual({preference});
    expect(values.has(startupPreferenceKey)).toBe(true);
  });
  it('marks malformed storage rather than silently approving auto selection', () => {
    expect(loadStartupPreference({getItem: () => '{broken'}).error).toBeTruthy();
    expect(loadStartupPreference({getItem: () => null})).toEqual({preference:defaultStartupPreference});
  });
  it('rejects fractional settings and invalid vendor without writing', () => {
    expect(() => validateStartupPreference({...defaultStartupPreference, baud:1.5})).toThrow();
    expect(() => validateStartupPreference({...defaultStartupPreference, backend:'unknown'})).toThrow();
    expect(() => validateStartupPreference({...defaultStartupPreference, backend:'coremor'})).toThrow();
  });
  it('canonicalizes legacy COM preference and surfaces storage failure', () => {
    expect(validateStartupPreference({...defaultStartupPreference, backend:'coremor', com_port:3}).endpoint).toBe('COM3');
    expect(() => saveStartupPreference({setItem: () => {throw new Error('quota');}}, defaultStartupPreference)).toThrow('quota');
  });
});
