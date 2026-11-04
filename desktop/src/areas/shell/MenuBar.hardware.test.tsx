// @vitest-environment jsdom
// Settings menu items of the hardware stream (epic #868: HW-020 Syringe Pump Settings…, HW-038 Boot Service Toggles…; SHELL-008 of #862).
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { MenuBar } from './MenuBar';
import { StudioContext } from '../../studio/StudioContext';
import type { Studio } from '../../studio/useStudioController';
import { OPEN_BOOT_SERVICES_EVENT, OPEN_PUMP_SETTINGS_EVENT } from '../../components/hardwareDialogEvents';
import { VIEWER_ONLY_REASON } from '../../components/ViewerGate';

vi.mock('../../bridge', () => ({ bridge: {} }));
let host: HTMLDivElement, root: Root;
async function render(studio: Partial<Studio>) {
  await act(async () => root.render(<StudioContext.Provider value={{ operatingMode: 'operator', enterMode: vi.fn(), append: vi.fn(), ...studio } as unknown as Studio}><MenuBar /></StudioContext.Provider>));
}
const openSettings = async () => act(async () => (Array.from(host.querySelectorAll('.menubar-item > button')).find(node => node.textContent === 'Settings') as HTMLElement).click());
const item = (label: string) => Array.from(host.querySelectorAll('[role="menuitem"]')).find(node => node.textContent === label) as HTMLButtonElement;
beforeEach(() => { Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true }); host = document.createElement('div'); document.body.append(host); root = createRoot(host); });
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe('Settings menu: hardware items', () => {
  it('lists Syringe Pump Settings… and Boot Service Toggles… and opens each dialog by its event', async () => {
    await render({ viewerOnly: false });
    await openSettings();
    const pump = vi.fn(), boot = vi.fn();
    window.addEventListener(OPEN_PUMP_SETTINGS_EVENT, pump); window.addEventListener(OPEN_BOOT_SERVICES_EVENT, boot);
    await act(async () => item('Syringe Pump Settings…').click());
    expect(pump).toHaveBeenCalledOnce(); expect(boot).not.toHaveBeenCalled();
    await openSettings();
    await act(async () => item('Boot Service Toggles…').click());
    window.removeEventListener(OPEN_PUMP_SETTINGS_EVENT, pump); window.removeEventListener(OPEN_BOOT_SERVICES_EVENT, boot);
    expect(boot).toHaveBeenCalledOnce();
  });
  it('is not offered to a viewer, who cannot change what the instrument keeps', async () => {
    await render({ viewerOnly: true });
    await openSettings();
    for (const label of ['Syringe Pump Settings…', 'Boot Service Toggles…']) {
      expect(item(label).disabled).toBe(true);
      expect(item(label).title).toBe(VIEWER_ONLY_REASON);
    }
  });
});
