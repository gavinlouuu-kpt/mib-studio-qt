export interface StartupPreference {backend: 'auto' | 'oeabt' | 'coremor'; endpoint: string; com_port: number; baud: number; address: number}
export const defaultStartupPreference: StartupPreference = {backend: 'auto', endpoint: '', com_port: -1, baud: 115200, address: 1};
export const startupPreferenceKey = 'mib.startup.nanopositioner.v1';
export function validateStartupPreference(value: unknown): StartupPreference {
  if (!value || typeof value !== 'object') throw new Error('Invalid remembered startup preference');
  const p = value as StartupPreference;
  if (!['auto','oeabt','coremor'].includes(p.backend) || typeof p.endpoint !== 'string' || p.endpoint.length > 512 || p.endpoint.includes('\0')) throw new Error('Invalid remembered endpoint/backend');
  if (![p.com_port,p.baud,p.address].every(Number.isSafeInteger) || p.com_port < -1 || p.com_port > 65535 || p.baud < 1 || p.baud > 4000000 || p.address < 0 || p.address > 255 || (p.backend === 'coremor' && p.com_port < 1)) throw new Error('Invalid remembered serial settings');
  return {backend: p.backend, endpoint: p.endpoint.trim() || (p.com_port > 0 ? `COM${p.com_port}` : ''), com_port: p.com_port, baud: p.baud, address: p.address};
}
export function loadStartupPreference(storage: Pick<Storage,'getItem'>): {preference: StartupPreference; error?: string} {
  try {const text = storage.getItem(startupPreferenceKey); return {preference: text ? validateStartupPreference(JSON.parse(text)) : {...defaultStartupPreference}};}
  catch (error) {return {preference: {...defaultStartupPreference}, error: String(error)};}
}
export function saveStartupPreference(storage: Pick<Storage,'setItem'>, value: StartupPreference): StartupPreference {
  const preference = validateStartupPreference(value);
  storage.setItem(startupPreferenceKey, JSON.stringify(preference));
  return preference;
}
