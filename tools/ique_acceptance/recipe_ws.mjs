// Exact F provider → I recipe → durable finding/export qualification on an owned runtime.
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
const [url,file,evidencePath,mode]=process.argv.slice(2);
const sourceKind=process.env.MIB_QA_SOURCE_KIND??'stored';
const ws=new WebSocket(url);const deadline=setTimeout(()=>process.exit(99),60000);
await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
let sequence=0;const pending=new Map();
ws.onmessage=event=>{if(typeof event.data!=='string')return;const r=JSON.parse(event.data);const cb=pending.get(r.request_id);if(cb){pending.delete(r.request_id);cb(r);}};
async function call(cmd,args={}){const request_id=++sequence;const reply=new Promise(resolve=>pending.set(request_id,resolve));ws.send(JSON.stringify({request_id,cmd,args}));const r=await reply;assert.equal(r.error,undefined,JSON.stringify(r));return r.ok;}
const work=(q,write=false)=>call(write?'workspace_write':'workspace_read',{request:JSON.stringify(q)});
const analysis=(q,write=false)=>call(write?'analysis_write':'analysis_read',{request:q});
const ok=r=>{assert.equal(r.status,'ok',JSON.stringify(r));return r;};
try {
 assert.equal(await call('init',{dataDir:''}),true);
 assert.equal((await call('load_recording',{filePath:path.resolve(file)})).ok,true);
 if(mode==='import'){
  const e=JSON.parse(await fs.readFile(evidencePath,'utf8'));
  const imported=ok(await work({action:'import',workspace_id:'fresh-import',command_id:'fresh-command',manifest:e.exported.manifest},true));
  assert.equal(imported.revision,'1');
  assert.deepEqual(ok(await work({...e.evaluate,workspace_id:'fresh-import',revision:'1'})).evaluation,e.evaluated.evaluation);
  assert.equal((await analysis({action:'read',planId:e.save.planId,revision:'2'})).status,'not_found');
  console.log('PASS fresh real-app database import resolves only exported exact strategy and reproduces native recipe result');
 }else if(mode==='recover'){
  const e=JSON.parse(await fs.readFile(evidencePath,'utf8'));
  assert.deepEqual(await analysis(e.save,true),e.strategy);
  assert.deepEqual(await work(e.evaluate),e.evaluated);
  assert.deepEqual(await work(e.exportRequest,true),e.exported);
  const doc=ok(await work({action:'read',workspace_id:e.workspace_id})).document;
  assert.deepEqual(doc.findings[0].evaluation,e.evaluated.evaluation);
  console.log('PASS exact native strategy receipt, recipe evaluation, finding and dependency export after restart');
 }else{
  const planId='science-'+crypto.randomUUID(),workspace_id='recipe-'+crypto.randomUUID();
  const source={kind:sourceKind,path:path.resolve(file)};
  const snapshot=ok(await analysis({action:'snapshot',source})).source;
  const sourceBinding=Object.fromEntries(['fingerprint','sourceKey','parameters','scope'].map(k=>[k,snapshot[k]]));
  const graph={id:planId,revision:'draft1',output:'gate',nodes:[{id:'gate',parent:'',kind:'interval',feature:'area',minimum:sourceKind==='stored'?100:null,maximum:sourceKind==='stored'?110:null}]};
  const save={action:'save',planId,expectedRevision:null,commandId:crypto.randomUUID(),graph,feature:'area',source,sourceBinding};
  const strategy=ok(await analysis(save,true));assert.equal(strategy.revision,'1');
  const profiles=path.resolve(evidencePath+'.profiles');await fs.mkdir(path.join(profiles,'method'),{recursive:true});
  await fs.writeFile(path.join(profiles,'method','config.json'),'{}');await fs.writeFile(path.join(profiles,'method','profile.meta.json'),JSON.stringify({profile_id:'recipe-method'}));
  const p=await call('profile_command',{base:profiles,request:JSON.stringify({operation:'read',name:'method'})});assert.equal(p.ok,true);
  const recipe={schema_version:1,id:crypto.randomUUID(),revision:'1',title:'Native recipe',question:'Area gate',method:{source:'local',id:'recipe-method',revision:p.profile.revision},analysis_plan:{id:planId,revision:'1'},lineage:{recipe:null,finding:null}};
  const document={schema_version:1,title:'Scientific recovery',plan:{experimentId:crypto.randomUUID(),format:'single',operator:{name:'QA',id:''},rows:[{ref:'S1',sample:'Synthetic qualification',condition:'QA',experimentalUnit:'unit-1',concentrationValue:'',concentrationUnit:'',channel:'',medium:'',temperatureC:'',notes:'Not biological evidence',free:[]}]},recipe,findings:[]};
  ok(await work({action:'commit',workspace_id,command_id:crypto.randomUUID(),expected_revision:null,document},true));
  ok(await work({action:'attach_source',workspace_id,command_id:crypto.randomUUID(),expected_revision:'1',source_id:'run',path:source.path},true));
  const evaluate={action:'evaluate_recipe',workspace_id,revision:'2',source_id:'run',source_kind:sourceKind,profile_base:profiles};
  const evaluated=ok(await work(evaluate));const value=evaluated.evaluation;
  if(sourceKind==='stored'){assert.equal(value.selected,'10');assert.equal(value.mean,104.6);assert.equal(value.median,104.5);}else{
   const archived=JSON.parse(await fs.readFile(path.join(path.dirname(file),'production-run.json'),'utf8'));
   const areas=archived.rows.map(r=>r.values[0].value).sort((a,b)=>a-b);
   assert.equal(value.selected,String(areas.length));assert.equal(value.mean,areas.reduce((a,b)=>a+b,0)/areas.length);assert.equal(value.denominator,null);assert.equal(value.populationComplete,false);
  }assert.equal(value.planId,planId);assert.equal(value.revision,'1');assert.equal(evaluated.applied,false);
  const next={...save,expectedRevision:'1',commandId:crypto.randomUUID(),graph:{...graph,nodes:[{...graph.nodes[0],minimum:1000,maximum:null}]}};
  assert.equal(ok(await analysis(next,true)).revision,'2');assert.deepEqual(await work(evaluate),evaluated);
  assert.equal((await analysis({action:'evaluate',planId,revision:'999',source})).status,'not_found');
  assert.equal(ok(await analysis({action:'evaluate',planId,revision:'2',source})).evaluation.selected,'0');
  const linked=ok(await work({action:'read',workspace_id})).document;
  linked.findings=[{id:crypto.randomUUID(),recipe,source_id:'run',source_sha256:linked.sources.run.sha256,interpretation:'Synthetic QA result, not a biological claim',evaluation:value}];
  ok(await work({action:'commit',workspace_id,expected_revision:'2',command_id:crypto.randomUUID(),document:linked},true));
  const exportRequest={action:'export',workspace_id,revision:'3',job_id:crypto.randomUUID()};const exported=ok(await work(exportRequest,true));
  assert.equal(exported.manifest.payload.dependencies.length,1);assert.equal(exported.manifest.payload.dependencies[0].revision,'1');
  await fs.writeFile(path.join(profiles,'method','config.json'),'{"different":true}');assert.equal((await work(evaluate)).status,'unavailable');await fs.writeFile(path.join(profiles,'method','config.json'),'{}');
  const moved=source.path+'.relinked';await fs.copyFile(source.path,moved);
  ok(await work({action:'relink',workspace_id,expected_revision:'3',command_id:crypto.randomUUID(),source_id:'run',path:moved},true));
  assert.equal((await call('load_recording',{filePath:moved})).ok,true);
  assert.deepEqual(ok(await work({...evaluate,revision:'4'})).evaluation,value);
  assert.notEqual((await work(evaluate)).status,'ok','wrong open path is not silently substituted');
  await fs.appendFile(moved,'changed');
  assert.equal((await work({...evaluate,revision:'4'})).status,'conflict');
  assert.equal(ok(await work({action:'read',workspace_id})).revision,'4');
  await fs.copyFile(source.path,moved);
  assert.equal((await call('load_recording',{filePath:source.path})).ok,true);
  await fs.writeFile(evidencePath,JSON.stringify({workspace_id,save,strategy,evaluate,evaluated,exportRequest,exported},null,2));
  console.log('PASS native pinned lookup/evaluation, immutable old revision after strategy update, empty population, unavailable method, saved finding, dependency export, identical-byte relink');
 }
}finally{ws.close();clearTimeout(deadline);}
