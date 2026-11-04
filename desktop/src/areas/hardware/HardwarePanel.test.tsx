// @vitest-environment jsdom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { HardwarePanel } from './HardwarePanel';
import { useStudio } from '../../studio/StudioContext';
const mounts = vi.fn();
vi.mock('../../studio/StudioContext', () => ({useStudio: vi.fn()}));
vi.mock('../../components/ViewerGate', () => ({ViewerGate: ({children}: {children: unknown}) => children}));
vi.mock('../../components/StageControls', () => ({StageControls: () => null}));
vi.mock('../../components/HardwareControls', async () => {
  const {useEffect} = await import('react');
  return {HardwareControls: () => { useEffect(() => { mounts(); }, []); return <p>pump panel</p>; }};
});
let host: HTMLDivElement, root: Root;
const studio = (tab: string) => vi.mocked(useStudio).mockReturnValue({tab, ready: true, expActive: false, append: vi.fn(), caps: {autofocus: true}, operatingMode: 'operator', triggerArmed: false, setTriggerArmed: vi.fn(), refreshCameraState: vi.fn(), pz7035: false} as never);
beforeEach(() => { Object.assign(globalThis, {IS_REACT_ACT_ENVIRONMENT: true}); mounts.mockClear(); host = document.createElement('div'); document.body.append(host); root = createRoot(host); });
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });
describe('hardware panel on every tab (HW-018)', () => {
  it('shows the panel on Connect and docks it behind a toggle elsewhere without remounting', async () => {
    studio('connect');
    await act(async () => root.render(<HardwarePanel />));
    expect(host.querySelector('.hardware-dock-toggle')).toBeNull();
    expect((host.querySelector('p')!.parentElement as HTMLElement).hidden).toBe(false);
    studio('experiment');
    await act(async () => root.render(<HardwarePanel />));
    const toggle = host.querySelector<HTMLButtonElement>('.hardware-dock-toggle')!;
    expect(toggle.textContent).toContain('Hardware: pumps and autofocus');
    expect((host.querySelector('p')!.parentElement as HTMLElement).hidden).toBe(true);
    await act(async () => toggle.click());
    expect(toggle.getAttribute('aria-expanded')).toBe('true');
    expect((host.querySelector('p')!.parentElement as HTMLElement).hidden).toBe(false);
    expect(mounts).toHaveBeenCalledOnce();
  });
});
