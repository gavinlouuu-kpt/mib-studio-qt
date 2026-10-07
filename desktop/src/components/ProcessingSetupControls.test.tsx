// @vitest-environment jsdom
import {act, useState} from 'react';
import {createRoot, type Root} from 'react-dom/client';
import {beforeEach, afterEach, it, expect, vi} from 'vitest';
import {ProcessingSetupControls} from './ProcessingSetupControls';
import {bridge} from '../bridge';
vi.mock('../bridge', () => ({bridge: {setBackgroundFromCurrentFrame: vi.fn(), clearBackgroundImage: vi.fn(), setProcessingRoi: vi.fn(), fetchProcessingConfigJson: vi.fn()}}));
let host: HTMLDivElement, root: Root;
const append = vi.fn();
function Harness({active = false}: {active?: boolean}) {
  const [roi, setRoi] = useState(8), [background, setBackground] = useState(true);
  async function refresh() {
    const doc = await bridge.fetchProcessingConfigJson();
    const config = JSON.parse(doc.json);
    setRoi(config.roi.w); setBackground(config.background_set);
  }
  return <><output>{roi}:{String(background)}</output><ProcessingSetupControls ready running experimentActive={active}
    hostProcessing backgroundSet={background} autoBackgroundEnabled={false} append={append} refresh={refresh} onConfigure={() => {}} /></>;
}
beforeEach(() => {
  Object.assign(globalThis, {IS_REACT_ACT_ENVIRONMENT: true}); vi.resetAllMocks();
  const refused = {ok: false, message: 'Experiment must be idle', command: 2, operation_id: '0'};
  vi.mocked(bridge.setProcessingRoi).mockResolvedValue(refused);
  vi.mocked(bridge.clearBackgroundImage).mockResolvedValue(refused);
  vi.mocked(bridge.setBackgroundFromCurrentFrame).mockResolvedValue(refused);
  vi.mocked(bridge.fetchProcessingConfigJson).mockResolvedValue({valid: true, json: JSON.stringify({roi: {w: 8}, background_set: true})});
  host = document.createElement('div'); root = createRoot(host);
});
afterEach(async () => {await act(async () => root.unmount());});
it.each(['Starting', 'Active', 'Stopping'])('locks setup controls while %s', async () => {
  await act(async () => root.render(<Harness active />));
  const buttons = Array.from(host.querySelectorAll('button')).filter(button => button.textContent !== 'Auto background: off · configure');
  expect(buttons).toHaveLength(3);
  for (const button of buttons) {expect(button.disabled).toBe(true); expect(button.title).toContain('Stop the experiment'); button.click();}
  expect(bridge.setProcessingRoi).not.toHaveBeenCalled(); expect(bridge.clearBackgroundImage).not.toHaveBeenCalled();
});
it.each(['Clear ROI', 'Clear Background', 'Set Background'])('refreshes authoritative fields after %s refusal', async label => {
  await act(async () => root.render(<Harness />));
  await act(async () => Array.from(host.querySelectorAll('button')).find(button => button.textContent === label)!.click());
  expect(host.querySelector('output')!.textContent).toBe('8:true');
  expect(bridge.fetchProcessingConfigJson).toHaveBeenCalledOnce();
  expect(append).toHaveBeenCalledWith(expect.stringContaining('failed: Experiment must be idle'));
});
