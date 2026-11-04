// Uses a real initialized QA app and an existing layout with retained sources.
// Creates only uniquely named report layouts/batches/runs; never starts acquisition.
import assert from 'node:assert/strict';
const [url,reportId,revision]=process.argv.slice(2);
if(!url||!reportId||!revision)throw Error('Usage: node report-batch-app-parity.mjs wss://host/ws reportId revision');
const ws=new WebSocket(url);const pending=new Map();let id=0;
ws.onmessage=e=>{if(typeof e.data!=='string')return;const r=JSON.parse(e.data),p=pending.get(r.request_id);if(p){pending.delete(r.request_id);r.error?p.reject(Error(JSON.stringify(r.error))):p.resolve(r.ok);}};
await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
const timer=setTimeout(()=>process.exit(2),90000);
const call=(cmd,args={})=>new Promise((resolve,reject)=>{const n=++id;pending.set(n,{resolve,reject});ws.send(JSON.stringify({request_id:n,cmd,args}));});
const read=request=>call('analysis_read',{request}),write=request=>call('analysis_write',{request});
try{
 await call('take_control');await call('init',{dataDir:''});
 const base=await read({action:'reportRead',reportId,revision});assert.equal(base.status,'ok',JSON.stringify(base));
 const suffix=Date.now(),second='batch-layout-'+suffix;
 const layout=await write({action:'reportSave',reportId:second,expectedRevision:null,commandId:crypto.randomUUID(),layout:base.document.layout});assert.equal(layout.status,'ok',JSON.stringify(layout));
 const reports=[{reportId,revision},{reportId:second,revision:layout.revision}];
 const batchId='report-batch-'+suffix;
 const create={action:'reportBatchCreate',batchId,commandId:crypto.randomUUID(),reports};
 const start=await write(create);assert.equal(start.document.state,'ready',JSON.stringify(start));assert.deepEqual(await write(create),start);
 const step=await write({action:'reportBatchAdvance',batchId,revision:start.revision});assert.equal(step.document.next,1,JSON.stringify(step));
 const cancel=await write({action:'reportBatchCancel',batchId,revision:step.revision});assert.equal(cancel.document.state,'cancelled');
 const refused=await write({action:'reportBatchAdvance',batchId,revision:cancel.revision});assert.deepEqual(refused,cancel);
 const absent=await read({action:'reportRunRead',reportId:cancel.document.runs[1].runId,revision:'1'});assert.equal(absent.status,'not_found');
 const completeId=batchId+'-complete';let complete=await write({...create,batchId:completeId,commandId:crypto.randomUUID()});
 for(let n=0;n<2;n++){complete=await write({action:'reportBatchAdvance',batchId:completeId,revision:complete.revision});assert.equal(complete.status,'ok',JSON.stringify(complete));}
 assert.equal(complete.document.state,'completed');assert.equal(complete.document.results.length,2);
 const reopened=await read({action:'reportBatchRead',batchId:completeId});assert.deepEqual(reopened,complete);
 for(const run of complete.document.results){const artifact=await read({action:'reportRunRead',reportId:run.runId,revision:'1'});assert.equal(artifact.document.contentHash,run.contentHash);}
 console.log(JSON.stringify({url,cancelledBatch:batchId,cancelledCompleted:cancel.document.next,pendingRunAbsent:true,completedBatch:completeId,completedReports:complete.document.results},null,2));
}finally{clearTimeout(timer);ws.close();}
