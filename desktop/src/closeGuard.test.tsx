// @vitest-environment jsdom
import {act} from 'react';
import {createRoot, type Root} from 'react-dom/client';
import {beforeEach,afterEach,it,expect,vi} from 'vitest';
import {bridge} from './bridge';
import {invoke} from '@tauri-apps/api/core';
import {confirm} from '@tauri-apps/plugin-dialog';
import {useCloseGuard} from './closeGuard';
const native=vi.hoisted(()=>({close:vi.fn(),onCloseRequested:vi.fn()}));
vi.mock('@tauri-apps/api/window',()=>({getCurrentWindow:()=>native}));
vi.mock('@tauri-apps/api/core',()=>({invoke:vi.fn()}));
vi.mock('@tauri-apps/plugin-dialog',()=>({confirm:vi.fn()}));
vi.mock('./bridge',()=>({bridge:{isInitialized:vi.fn(),fetchExperimentStatus:vi.fn(),reviewExportStatus:vi.fn(),reviewReanalysisStatus:vi.fn(),backgroundCalibrationStatus:vi.fn(),stopCapture:vi.fn()}}));
Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});
let root:Root,host:HTMLDivElement,close:()=>Promise<void>,dirty=false,busy=false,ready=true;
const report=vi.fn();
function Harness(){close=useCloseGuard({ready,dirty,busy,report});return null;}
beforeEach(async()=>{
 vi.resetAllMocks();dirty=false;busy=false;ready=true;native.onCloseRequested.mockResolvedValue(()=>{});
 vi.mocked(bridge.fetchExperimentStatus).mockResolvedValue({valid:true,state:0} as never);
 vi.mocked(invoke).mockResolvedValue({capture_running:false,recording:false});
 vi.mocked(bridge.reviewExportStatus).mockResolvedValue({state:'idle'});
 vi.mocked(bridge.reviewReanalysisStatus).mockResolvedValue({state:'idle'});
 vi.mocked(bridge.backgroundCalibrationStatus).mockResolvedValue({state:'idle'} as never);
 host=document.createElement('div');root=createRoot(host);await act(async()=>root.render(<Harness/>));
});
afterEach(async()=>{await act(async()=>root.unmount());});
it('OS close is prevented while experiment finalizes, without issuing stop or closing',async()=>{
 vi.mocked(bridge.fetchExperimentStatus).mockResolvedValue({valid:true,state:3} as never);
 const event={preventDefault:vi.fn()};await act(async()=>native.onCloseRequested.mock.calls[0][0](event));
 expect(event.preventDefault).toHaveBeenCalled();expect(native.close).not.toHaveBeenCalled();expect(report).toHaveBeenCalledWith(expect.stringContaining('finalization'));
});
it('failed backend reconciliation cannot close and concurrent clicks are single flight',async()=>{
 vi.mocked(bridge.reviewExportStatus).mockRejectedValue(new Error('disconnected'));
 await act(async()=>Promise.all([close(),close()]));expect(bridge.fetchExperimentStatus).toHaveBeenCalledTimes(1);expect(native.close).not.toHaveBeenCalled();
});
it('dirty discard refusal preserves window, idle capture stops only after accepted discard',async()=>{
 dirty=true;await act(async()=>root.render(<Harness/>));vi.mocked(confirm).mockResolvedValue(false);
 await act(async()=>close());expect(native.close).not.toHaveBeenCalled();expect(bridge.fetchExperimentStatus).not.toHaveBeenCalled();
 vi.mocked(confirm).mockResolvedValue(true);vi.mocked(invoke).mockResolvedValue({capture_running:true,recording:false});vi.mocked(bridge.stopCapture).mockResolvedValue({ok:true} as never);
 await act(async()=>close());expect(bridge.stopCapture).toHaveBeenCalledTimes(1);expect(native.close).toHaveBeenCalledTimes(1);
});
it('in-flight saves and raw recording block closing',async()=>{
 busy=true;await act(async()=>root.render(<Harness/>));await act(async()=>close());expect(native.close).not.toHaveBeenCalled();
 busy=false;await act(async()=>root.render(<Harness/>));vi.mocked(invoke).mockResolvedValue({capture_running:true,recording:true});await act(async()=>close());expect(native.close).not.toHaveBeenCalled();expect(bridge.stopCapture).not.toHaveBeenCalled();
});

it('early reload cannot bypass native active-run checks before shell readiness',async()=>{
 ready=false;await act(async()=>root.render(<Harness/>));vi.mocked(bridge.isInitialized).mockResolvedValue(true);
 vi.mocked(bridge.fetchExperimentStatus).mockResolvedValue({valid:true,state:2} as never);
 await act(async()=>close());expect(native.close).not.toHaveBeenCalled();expect(report).toHaveBeenCalledWith(expect.stringContaining('finalization'));
});
