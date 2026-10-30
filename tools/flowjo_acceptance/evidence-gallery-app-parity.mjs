import assert from 'node:assert/strict';
// Uses an actual E-run-events/1 experiment archive, never ordinal-as-image-index.
const [url,path,restorePath]=process.argv.slice(2);
if(!url||!path)throw Error('Usage: node evidence-gallery-app-parity.mjs wss://host/ws archive.h5 [restore.h5]');
const ws=new WebSocket(url);const pending=new Map();let id=0;
ws.onmessage=e=>{if(typeof e.data!=='string')return;const r=JSON.parse(e.data),p=pending.get(r.request_id);if(p){pending.delete(r.request_id);r.error?p.reject(Error(JSON.stringify(r.error))):p.resolve(r.ok);}};
await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
function call(cmd,args={}){return new Promise((resolve,reject)=>{const request_id=++id;pending.set(request_id,{resolve,reject});ws.send(JSON.stringify({request_id,cmd,args}));});}
const timer=setTimeout(()=>process.exit(2),60000);
try{
 await call('take_control');assert.equal(await call('init',{dataDir:''}),true);
 assert.equal((await call('load_recording',{filePath:path})).ok,true);
 const source={kind:'run-archive',path};
 const snapshot=await call('analysis_read',{request:{action:'snapshot',source}});assert.equal(snapshot.status,'ok',JSON.stringify(snapshot));
 const planId='evidence-images-'+Date.now(),graph={id:planId,revision:'draft',output:'all',nodes:[{id:'all',parent:'',kind:'interval',feature:'area',minimum:null,maximum:null}]};
 const sourceBinding=Object.fromEntries(['fingerprint','sourceKey','parameters','scope'].map(k=>[k,snapshot.source[k]]));
 const saved=await call('analysis_write',{request:{action:'save',planId,expectedRevision:null,commandId:crypto.randomUUID(),graph,feature:'area',source,sourceBinding}});assert.equal(saved.revision,'1',JSON.stringify(saved));
 const outcomes=[];
 for(const mask of [false,true]){
  const reply=await call('analysis_read',{request:{action:'gallery',planId,revision:'1',source,offset:0,mask}});assert.equal(reply.status,'ok',JSON.stringify(reply));
  assert.equal(reply.mask,mask);assert.equal(reply.fileFingerprint,snapshot.source.fileFingerprint);
  assert.deepEqual(reply.images.map(i=>i.key),snapshot.source.rows.slice(0,12).map(r=>r.key));
  assert(reply.images.length>0);assert(reply.images.every(i=>i.status==='available'&&i.pixels.length===4096),JSON.stringify(reply.images.map(({pixels,...rest})=>rest)));
  outcomes.push({mask,count:reply.images.length,keys:reply.images.map(i=>i.key)});
 }
 console.log(JSON.stringify({url,path,planId,revision:'1',fileFingerprint:snapshot.source.fileFingerprint,outcomes},null,2));
}finally{if(restorePath)await call('load_recording',{filePath:restorePath});clearTimeout(timer);ws.close();}
