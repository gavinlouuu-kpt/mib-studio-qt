import assert from 'node:assert/strict';
// Requires the standard 14-observation synthetic Review fixture and an idle mock QA app.
const [url,path]=process.argv.slice(2);
if(!url||!path)throw Error('Usage: node polygon-app-parity.mjs wss://host/ws /absolute/synthetic.h5');
const ws=new WebSocket(url,{headers:{Origin:new URL(url.replace(/^ws/,'http')).origin}});
const pending=new Map();let id=0;
ws.onmessage=e=>{if(typeof e.data!=='string')return;const r=JSON.parse(e.data);const p=pending.get(r.request_id);if(p){pending.delete(r.request_id);r.error?p.reject(Error(JSON.stringify(r.error))):p.resolve(r.ok);}};
await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
function call(cmd,args={}){return new Promise((resolve,reject)=>{const n=++id;pending.set(n,{resolve,reject});ws.send(JSON.stringify({request_id:n,cmd,args}));});}
const timer=setTimeout(()=>{console.error('QA timeout');process.exit(2);},90000);
try {
 await call('take_control');assert.equal(await call('init',{dataDir:''}),true);
 assert.equal((await call('load_recording',{filePath:path})).ok,true);
 const source={kind:'stored',path};
 const snapshot=await call('analysis_read',{request:{action:'snapshot',source}});assert.equal(snapshot.status,'ok',JSON.stringify(snapshot));
 const sourceBinding=Object.fromEntries(['fingerprint','sourceKey','parameters','scope'].map(k=>[k,snapshot.source[k]]));
 const planId='polygon-e2e-'+Date.now();
 const vertices=[[100,0],[110,0],[110,1],[100,1]];
 const graph={id:planId,revision:'draft1',output:'polygon',nodes:[{id:'polygon',parent:'',kind:'polygon',x:'area',y:'deformability',vertices}]};
 const save={action:'save',planId,expectedRevision:null,commandId:crypto.randomUUID(),graph,feature:'area',source,sourceBinding};
 const saved=await call('analysis_write',{request:save});assert.equal(saved.revision,'1',JSON.stringify(saved));
 const q={action:'evaluate',planId,revision:'1',source};
 const expected=snapshot.source.rows.filter(r=>{const a=r.measurements.area,d=r.measurements.deformability;return a!=null&&d!=null&&a>=100&&a<=110&&d>=0&&d<=1;}).length;assert.equal(expected,10);

 const evaluated=await call('analysis_read',{request:q});assert.equal(evaluated.evaluation.selected,String(expected));assert.equal(evaluated.evaluation.unit,'pixel2');
 const gallery=await call('analysis_read',{request:{...q,action:'gallery',offset:0}});assert.equal(gallery.status,'ok',JSON.stringify(gallery));
 assert.deepEqual(gallery.images.map(i=>i.key),evaluated.evaluation.rows.filter(r=>r.membership==='selected').map(r=>r.key));
 assert(gallery.images.every(i=>i.pixels.length===4096));
 const submission={commandId:crypto.randomUUID(),planId,revision:'1',source,sourceFingerprint:snapshot.source.fingerprint,x:'area',y:'deformability',vertices};
 const started=await call('analysis_job',{request:{action:'submit',submission}});assert.equal(started.status,'pending',JSON.stringify(started));
 let terminal;
 for(let tries=0;tries<600;tries++){terminal=await call('analysis_job',{request:{action:'query',jobId:started.jobId}});if(terminal.status!=='pending')break;await new Promise(r=>setTimeout(r,100));}
 assert.equal(terminal.status,'terminal',JSON.stringify(terminal));assert.equal(terminal.terminal.failure,null,JSON.stringify(terminal));
 assert.equal(terminal.terminal.result.rows.filter(r=>r.value==='true').length,expected);
 assert.equal(terminal.terminal.result.revision.sourceFingerprint,snapshot.source.fingerprint);assert.equal(JSON.parse(terminal.terminal.result.revision.parameters).identity,evaluated.evaluation.parameterFingerprint);
 assert.deepEqual(await call('analysis_write',{request:save}),saved);
 const report=await call('analysis_read',{request:{action:'report',planId,revision:'1',source}});assert.equal(report.status,'ok');assert.equal(report.report.table[0].selected,String(expected));
 const read=await call('analysis_read',{request:{action:'read',planId,revision:'1'}});assert.deepEqual(read.document.graph,graph);
 const invalid=structuredClone(save);invalid.expectedRevision='1';invalid.commandId=crypto.randomUUID();invalid.graph.nodes[0].vertices=[[0,0],[2,2],[0,2],[2,0]];assert.notEqual((await call('analysis_write',{request:invalid})).status,'ok');
 assert.deepEqual((await call('analysis_read',{request:{action:'read',planId,revision:'1'}})).document.graph,graph);
 assert.deepEqual(terminal.terminal.result.rows.map(r=>({key:r.key,membership:r.value==='true'?'selected':r.value==='false'?'excluded':'unknown'})),evaluated.evaluation.rows);
 const proof={url,planId,revision:'1',sourceFingerprint:snapshot.source.fingerprint,selected:evaluated.evaluation.selected,mean:evaluated.evaluation.mean,partition:report.report.table.map(r=>({population:r.population,selected:r.selected})),galleryImages:gallery.images.length,jobId:started.jobId,jobSelected:expected,commandId:submission.commandId,source};
 console.log(JSON.stringify(proof,null,2));
} finally {clearTimeout(timer);ws.close();}
