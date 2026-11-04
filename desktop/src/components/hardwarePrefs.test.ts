import { beforeEach, describe, expect, it, vi } from 'vitest';
import { bridge } from '../bridge';
import { loadHardwarePrefs, saveHardwarePrefs, withPump } from './hardwarePrefs';
vi.mock('../bridge', () => ({bridge: {getPreferences: vi.fn(), setPreferences: vi.fn()}}));
describe('hardware preferences (HW-016, HW-030)', () => {
  let stored: Record<string, unknown>;
  beforeEach(() => {
    stored = {sidebarCollapsed: true};
    vi.mocked(bridge.getPreferences).mockImplementation(async () => structuredClone(stored));
    vi.mocked(bridge.setPreferences).mockImplementation(async value => { await new Promise(r => setTimeout(r, 5)); stored = value; });
  });
  it('keeps other preferences and chains quick saves so none is lost', async () => {
    await Promise.all([
      saveHardwarePrefs(old => withPump(old, 0, {rate: '5'})),
      saveHardwarePrefs(old => withPump(old, 1, {direction: 1})),
      saveHardwarePrefs(old => ({...old, focusSetpoint: 30})),
    ]);
    expect(stored).toEqual({sidebarCollapsed: true, hardware: {pumps: [{rate: '5'}, {direction: 1}], focusSetpoint: 30}});
    expect(await loadHardwarePrefs()).toEqual(stored.hardware);
  });
  it('loads nothing when the preferences cannot be read', async () => {
    vi.mocked(bridge.getPreferences).mockRejectedValue(new Error('no file'));
    expect(await loadHardwarePrefs()).toEqual({});
  });
});
