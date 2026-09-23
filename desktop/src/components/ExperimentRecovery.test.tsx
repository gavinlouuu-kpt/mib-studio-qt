// @vitest-environment jsdom
import {act} from 'react';
import {createRoot, type Root} from 'react-dom/client';
import {afterEach, beforeEach, expect, it, vi} from 'vitest';
import {ExperimentRecovery} from './ExperimentRecovery';
import {bridge, type ExperimentStatus} from '../bridge';
vi.mock('../bridge', () => ({bridge:{experimentAcknowledgeFault:vi.fn(),fetchExperimentStatus:vi.fn()}}));
let host: HTMLDivElement, root: Root;
const failed = {valid:true,state:4,start_generation:'9007199254740993',terminal:true,flushing:false,finalization_ok:false,output_path:'/runs/failed.h5',fault_revision:'11',fault_code:'experiment.saveFailed',fault_message:'Disk full',completion_reason:'Disk full',persistence_committed:'4',persistence_admitted:'6',persistence_failed:'2'} as ExperimentStatus;
beforeEach(() => {Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});vi.clearAllMocks();host=document.createElement('div');document.body.append(host);root=createRoot(host);vi.mocked(bridge.experimentAcknowledgeFault).mockResolvedValue({ok:true,message:'Acknowledged',command:0} as never);vi.mocked(bridge.fetchExperimentStatus).mockResolvedValue({...failed,state:0,fault_code:''});});
afterEach(async () => {await act(async () => root.unmount());host.remove();});
async function render(status=failed) {await act(async () => root.render(<ExperimentRecovery ready status={status} onStatus={() => {}}/>));}
it('requires explicit review and sends exact retained identity',async()=>{await render();expect(host.textContent).toContain('/runs/failed.h5');expect(host.textContent).toContain('Disk full');expect(host.querySelector('button')!.disabled).toBe(true);await act(async()=>host.querySelector('input')!.click());await act(async()=>host.querySelector('button')!.click());expect(bridge.experimentAcknowledgeFault).toHaveBeenCalledWith('9007199254740993','11','experiment.saveFailed','Disk full',true);});
it('never enables acknowledgment during finalization',async()=>{await render({...failed,terminal:false,flushing:true});expect(host.querySelector('input')!.disabled).toBe(true);expect(host.querySelector('button')!.disabled).toBe(true);});
it('changed fault invalidates prior operator confirmation',async()=>{await render();await act(async()=>host.querySelector('input')!.click());await render({...failed,fault_message:'New writer error'});expect(host.querySelector('input')!.checked).toBe(false);expect(host.querySelector('button')!.disabled).toBe(true);});
it('retains failed outcome and file after acknowledgment',async()=>{await render({...failed,state:0,fault_code:'',fault_message:''});expect(host.textContent).toContain('/runs/failed.h5');expect(host.textContent).toContain('Disk full');expect(host.querySelector('button')).toBeNull();});

it("repeated identical fault clears previous confirmation",async()=>{await render();await act(async()=>host.querySelector("input")!.click());await render({...failed,fault_revision:"12"});expect(host.querySelector("input")!.checked).toBe(false);});
