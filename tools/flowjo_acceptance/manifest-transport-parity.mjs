import assert from 'node:assert/strict';
const [url,path]=process.argv.slice(2);
if(!url||!path)throw Error('Usage: node manifest-transport-parity.mjs wss://host/ws recording.h5');
const ws=new WebSocket(url),pending=new Map();let id=0;
ws.onmessage=e=>{if(typeof e.data!=='string')return;const r=JSON.parse(e.data),p=pending.get(r.request_id);if(p){pending.delete(r.request_id);r.error?p.reject(Error(JSON.stringify(r.error))):p.resolve(r.ok);}};
await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
function call(cmd,args={},nativeFloat=false){return new Promise((resolve,reject)=>{const request_id=++id;pending.set(request_id,{resolve,reject});let text=JSON.stringify({request_id,cmd,args});if(nativeFloat)text=text.replace('"minimum":5.5','"minimum":5.0');ws.send(text);});}
const timeout=setTimeout(()=>process.exit(2),60000);
try{
 await call('take_control');assert.equal(await call('init',{dataDir:''}),true);assert.equal((await call('load_recording',{filePath:path})).ok,true);
 const source={kind:'stored',path},snap=await call('analysis_read',{request:{action:'snapshot',source}});assert.equal(snap.status,'ok');
 const planId='lossless-'+Date.now(),sourceBinding=Object.fromEntries(['fingerprint','sourceKey','parameters','scope'].map(k=>[k,snap.source[k]]));
 const graph={id:planId,revision:'draft',output:'gate',nodes:[{id:'gate',parent:'',kind:'interval',feature:'area',minimum:5.5,maximum:null}]};
 const saved=await call('analysis_write',{request:{action:'save',planId,expectedRevision:null,commandId:crypto.randomUUID(),source,sourceBinding,graph,feature:'area'}},true);assert.equal(saved.revision,'1',JSON.stringify(saved));
 const manifest=await call('analysis_read',{request:{action:'manifest',planId,revision:'1'}});assert(manifest.manifest_json.includes('"minimum":5.0'));
 const common={action:'import',source,expectedRevision:'1',contentHash:manifest.contentHash};
 const lossy=await call('analysis_write',{request:{...common,manifest:manifest.manifest,commandId:crypto.randomUUID()}});assert.equal(lossy.status,'invalid');
 const envelope=JSON.parse(JSON.stringify({manifest_json:manifest.manifest_json,contentHash:manifest.contentHash}));
 const imported=await call('analysis_write',{request:{...common,...envelope,commandId:crypto.randomUUID()}});assert.equal(imported.revision,'2',JSON.stringify(imported));
 const again=await call('analysis_read',{request:{action:'manifest',planId,revision:'2'}});assert(again.manifest_json.includes('"minimum":5.0'));
 console.log(JSON.stringify({url,planId,revision:'2',opaqueRoundTrip:true,lossyObjectRefused:true,originalHash:manifest.contentHash},null,2));
}finally{clearTimeout(timeout);ws.close();}
