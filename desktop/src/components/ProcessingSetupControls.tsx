import {bridge} from '../bridge';

export function ProcessingSetupControls({ready, running, experimentActive, hostProcessing, hostBackground = true, backgroundSet,
  autoBackgroundEnabled, append, refresh, onConfigure}: {
  ready: boolean; running: boolean; experimentActive: boolean; hostProcessing: boolean; hostBackground?: boolean;
  backgroundSet: boolean; autoBackgroundEnabled: boolean; append: (message: string) => void;
  refresh: () => Promise<void>; onConfigure: () => void;
}) {
  const reason = experimentActive ? 'Stop the experiment before changing background or ROI' : undefined;
  async function mutate(action: 'set' | 'clear' | 'roi') {
    try {
      const result = await (action === 'set' ? bridge.setBackgroundFromCurrentFrame()
        : action === 'clear' ? bridge.clearBackgroundImage() : bridge.setProcessingRoi(0, 0, 0, 0));
      append(result.ok ? (action === 'set' ? 'background captured from current frame' : action === 'clear' ? 'background cleared' : 'ROI cleared')
        : `${action === 'roi' ? 'clear ROI' : action === 'set' ? 'set background' : 'clear background'} failed: ${result.message}`);
    } catch (error) {
      append(`processing setup error: ${error}`);
    } finally {
      await refresh();
    }
  }
  return <>
    {hostProcessing && <button disabled={!running || experimentActive}
      title={reason ?? (running ? 'Capture the current frame as the processing background' : 'Camera is not running')}
      onClick={() => void mutate('set')}>Set Background</button>}
    {hostBackground && <button disabled={!backgroundSet || experimentActive} title={reason ?? (backgroundSet ? undefined : 'No background is set')}
      onClick={() => void mutate('clear')}>Clear Background</button>}
    {hostBackground && <button onClick={onConfigure} title="Edit image_processing.auto_background_* in the configuration below">
      Auto background: {autoBackgroundEnabled ? 'on' : 'off'} · configure
    </button>}
    {hostProcessing && <button disabled={!ready || experimentActive} title={reason} onClick={() => void mutate('roi')}>Clear ROI</button>}
  </>;
}
