// Actual F retained-source/comparison/finding -> I planning -> SQLite recovery.
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
const [url,baseEvidence,otherFile,output,mode]=process.argv.slice(2);
const ws=new WebSocket(url);const deadline=setTimeout(()=>process.exit(99),60000);
let session;ws.addEventListener('message',e=>{if(typeof e.data==='string'){const r=JSON.parse(e.data);if(r.session)session={...session,...r.session};}});
await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
let sequence=0;const pending=new Map();
ws.onmessage=e=>{if(typeof e.data!=='string')return;const r=JSON.parse(e.data);const cb=pending.get(r.request_id);if(cb){pending.delete(r.request_id);cb(r);}};
async function call(cmd,args={}){const request_id=++sequence;const reply=new Promise(resolve=>pending.set(request_id,resolve));ws.send(JSON.stringify({request_id,cmd,args}));const r=await reply;assert.equal(r.error,undefined,JSON.stringify(r));return r.ok;}
const work=(q,write=false)=>call(write?'workspace_write':'workspace_read',{request:JSON.stringify(q)});
const analysis=(q,write=false)=>call(write?'analysis_write':'analysis_read',{request:q});
const ok=r=>{assert.equal(r.status,'ok',JSON.stringify(r));return r;};
try {
 await work({action:'list',limit:1});assert.equal(session.client_id,session.controller_id,'Runtime has another controller; do not take control');
 assert.equal(await call('init',{dataDir:''}),true);
 if(mode==='recover'){
  const e=JSON.parse(await fs.readFile(output,'utf8'));
  assert.deepEqual(await work(e.create,true),e.created);
  assert.equal(ok(await work(e.verify)).verified,true);
  assert.deepEqual(ok(await work({action:'read',workspace_id:e.create.workspace_id,revision:'1'})).document,e.created.document);
  assert.deepEqual(await work(e.exportRequest,true),e.exported);
  console.log('PASS restart: exact derived receipt, reopened evidence and export survive SQLite restart');
 }else if(mode==='import'){
  const e=JSON.parse(await fs.readFile(output,'utf8'));
  assert.equal((await work({action:'import',workspace_id:crypto.randomUUID(),command_id:crypto.randomUUID(),manifest:e.exported.manifest_json},true)).status,'invalid');
  console.log('PASS fresh database: workspace manifest cannot mint retained sources, strategies or finding authority');
 }else{
  const e=JSON.parse(await fs.readFile(baseEvidence,'utf8'));
  const origin=ok(await work({action:'read',workspace_id:e.workspace_id}));
  const source=e.save.source;
  assert.equal((await call('load_recording',{filePath:source.path})).ok,true);
  const retained=ok(await analysis({action:'retainSource',source,commandId:crypto.randomUUID()},true));
  const second={kind:'run-archive',path:path.resolve(otherFile)};
  assert.equal((await call('load_recording',{filePath:second.path})).ok,true);
  const native=ok(await analysis({action:'snapshot',source:second})).source;
  const planId=crypto.randomUUID();
  const graph={id:planId,revision:'draft',output:'all',nodes:[{id:'all',parent:'',kind:'interval',feature:'area',minimum:null,maximum:null}]};
  ok(await analysis({action:'save',planId,expectedRevision:null,commandId:crypto.randomUUID(),graph,feature:'area',source:second,sourceBinding:Object.fromEntries(['fingerprint','sourceKey','parameters','scope'].map(k=>[k,native[k]]))},true));
  const secondRetained=ok(await analysis({action:'retainSource',source:second,commandId:crypto.randomUUID()},true));
  const spec={conditions:['before','after'],design:'independent',weighting:'equal-experimental-unit',descriptiveOnly:true,endpoint:{feature:'area',statistic:'mean',unit:'pixel2'},samples:[{condition:'before',experimentalUnit:'unit-a',planId:e.save.planId,revision:'1',source:retained.descriptor},{condition:'after',experimentalUnit:'unit-b',planId,revision:'1',source:secondRetained.descriptor}]};
  const finding=ok(await analysis({action:'saveFinding',findingId:crypto.randomUUID(),commandId:crypto.randomUUID(),spec,note:'Synthetic pipeline QA; not biological inference'},true));
  const reference={findingId:finding.findingId,revision:'1',contentHash:finding.document.contentHash};
  const preview={action:'resolve_finding',workspace_id:e.workspace_id,revision:origin.revision,finding:reference,sample_index:0,profile_base:e.evaluate.profile_base};
  assert.equal(ok(await work(preview)).evidence.compatibility,'unknown');
  assert.equal((await work({...preview,finding:{...reference,contentHash:'sha256:'+'0'.repeat(64)}})).status,'invalid');
  assert.equal((await work({...preview,finding:{...reference,findingId:'missing'}})).status,'invalid');
  assert.equal((await work({...preview,sample_index:1})).status,'invalid');
  const create={action:'draft_from_finding',workspace_id:crypto.randomUUID(),command_id:crypto.randomUUID(),source_workspace_id:e.workspace_id,source_revision:origin.revision,finding:reference,sample_index:0,profile_base:e.evaluate.profile_base,experiment_id:crypto.randomUUID(),recipe_id:crypto.randomUUID()};
  const created=ok(await work(create,true));assert.equal(created.revision,'1');
  assert.equal(created.document.plan.experimentId,create.experiment_id);assert.equal(created.document.recipe.id,create.recipe_id);
  for(const k of ['run_observation','sources','findings','queue'])assert.equal(created.document[k],undefined);
  assert.equal(created.document.next_run_evidence.applied,false);
  const verify={action:'verify_next_run',workspace_id:create.workspace_id,revision:'1',profile_base:e.evaluate.profile_base};assert.equal(ok(await work(verify)).verified,true);
  const exportRequest={action:'export',workspace_id:create.workspace_id,revision:'1',job_id:crypto.randomUUID()};const exported=ok(await work(exportRequest,true));
  assert.equal(exported.manifest.payload.dependencies.length,5);
  for(const dep of exported.manifest.payload.dependencies){
    const native=ok(await work({action:'read',workspace_id:dep.workspace_id,revision:dep.revision}));
    assert.deepEqual(dep.document,native.document,dep.workspace_id);
  }
  ok(await work({action:'import',workspace_id:crypto.randomUUID(),command_id:crypto.randomUUID(),manifest:exported.manifest_json},true));
  const config=path.join(e.evaluate.profile_base,'method','config.json');const old=await fs.readFile(config,'utf8');
  try{await fs.writeFile(config,'{"changed":true}');assert.deepEqual(await work(create,true),created);assert.equal((await work(verify)).status,'invalid');}finally{await fs.writeFile(config,old);}
  ok(await work({action:'commit',workspace_id:e.workspace_id,expected_revision:origin.revision,command_id:crypto.randomUUID(),document:origin.document},true));
  assert.deepEqual(await work(create,true),created);
  assert.equal((await work({...create,workspace_id:crypto.randomUUID(),command_id:crypto.randomUUID()},true)).status,'conflict');
  await fs.writeFile(output,JSON.stringify({create,created,verify,exportRequest,exported,finding},null,2));
  console.log('PASS actual HDF -> native retained source/strategy/finding -> next-run draft; exact hashes, refusal, import, stale head and method-change retry');
 }
}finally{ws.close();clearTimeout(deadline);}
