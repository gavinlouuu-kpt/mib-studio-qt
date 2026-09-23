// @vitest-environment jsdom
import {act} from 'react';
import {createRoot, type Root} from 'react-dom/client';
import {beforeEach,afterEach,it,expect,vi} from 'vitest';
import {CaptureRecovery} from './CaptureRecovery';
import {bridge} from '../bridge';
vi.mock('../bridge',()=>({bridge:{fetchCaptureLifecycle:vi.fn()}}));
let host:HTMLDivElement,root:Root;
const status={valid:true,state:'faulted',generation:'7',failure_generation:'7',camera_ready:false,failure:'cameraStartFailed',message:'Device unavailable'};
beforeEach(()=>{Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});vi.clearAllMocks();vi.mocked(bridge.fetchCaptureLifecycle).mockResolvedValue(status);host=document.createElement('div');document.body.append(host);root=createRoot(host);});
afterEach(async()=>{await act(async()=>root.unmount());host.remove();});
it('retains camera fault and offers explicit retry without claiming ready',async()=>{const retry=vi.fn().mockResolvedValue(undefined);await act(async()=>root.render(<CaptureRecovery ready blocked={false} onRetry={retry} onConfigure={()=>{}}/>));expect(host.textContent).toContain('Device unavailable');expect(retry).not.toHaveBeenCalled();await act(async()=>host.querySelector('button')!.click());expect(retry).toHaveBeenCalledOnce();expect(host.textContent).toContain('Device unavailable');});
it('blocks reconnect during experiment finalization',async()=>{await act(async()=>root.render(<CaptureRecovery ready blocked onRetry={vi.fn()} onConfigure={()=>{}}/>));expect([...host.querySelectorAll('button')].every(b=>b.disabled)).toBe(true);});
it('hides historical fault only when backend confirms camera ready',async()=>{vi.mocked(bridge.fetchCaptureLifecycle).mockResolvedValue({...status,state:'running',camera_ready:true});await act(async()=>root.render(<CaptureRecovery ready blocked={false} onRetry={vi.fn()} onConfigure={()=>{}}/>));expect(host.querySelector('button')).toBeNull();});
