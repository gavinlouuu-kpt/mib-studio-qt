// @vitest-environment jsdom
import { act } from 'react';
import { createRoot } from 'react-dom/client';
import { describe, expect, it, vi } from 'vitest';
import { bridge } from '../../bridge';
import { useServiceMode } from './useHardware';
vi.mock('../../bridge', () => ({bridge: {setServiceMode: vi.fn()}}));
vi.mock('../../transport/dialogs', () => ({confirm: vi.fn(async () => true)}));
describe('Service mode switch (#868)', () => {
  it('logs a backend refusal instead of swallowing it', async () => {
    Object.assign(globalThis, {IS_REACT_ACT_ENVIRONMENT: true});
    vi.mocked(bridge.setServiceMode).mockResolvedValue({ok: false, command: 0, message: 'refused by instrument', operation_id: '0'});
    const append = vi.fn();
    let enter: (mode: 'service' | 'operator') => Promise<void> = async () => {};
    function Probe() {
      enter = useServiceMode({append, instrumentModes: true, operatingMode: 'operator', setOperatingMode: vi.fn(), triggerArmed: false,
        setTriggerArmed: vi.fn(), trigStatus: null, expActive: false} as never).enterMode;
      return null;
    }
    const root = createRoot(document.createElement('div'));
    await act(async () => root.render(<Probe />));
    await act(async () => { await enter('service'); });
    expect(bridge.setServiceMode).toHaveBeenCalledWith(true);
    expect(append).toHaveBeenCalledWith('backend did not switch Service mode: refused by instrument');
    await act(async () => root.unmount());
  });
});
