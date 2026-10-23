// Real Studio WebSocket/native SQLite recovery probe. Use an isolated owned runtime.
// node tools/ique_acceptance/workspace_ws.mjs ws://127.0.0.1:18089/ws <evidence.json> [recover]
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
const [url, evidencePath, recover] = process.argv.slice(2);
if (!url || !evidencePath) throw new Error('URL and evidence path required');
const ws = new WebSocket(url);
const deadline = setTimeout(() => { console.error('Workspace probe deadline exceeded'); process.exit(99); }, 25000);
await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
let sequence=0;const pending=new Map();
ws.onmessage=event=>{const r=JSON.parse(event.data);const cb=pending.get(r.request_id);if(cb){pending.delete(r.request_id);cb(r);}};
async function call(cmd,args) {const request_id=++sequence;const reply=new Promise(resolve=>pending.set(request_id,resolve));ws.send(JSON.stringify({request_id,cmd,args}));const r=await reply;assert.equal(r.error,undefined,JSON.stringify(r));return r.ok;}
const send=(q,write=false)=>call(write?'workspace_write':'workspace_read',{request:JSON.stringify(q)});
try {
  assert.equal(await call('init',{dataDir:''}),true);
  if(recover) {
    const e=JSON.parse(await fs.readFile(evidencePath,'utf8'));
    assert.deepEqual(await send(e.exportRequest,true),e.exported);
    assert.deepEqual(await send({action:'export_status',workspace_id:e.workspace_id,job_id:e.exportRequest.job_id}),e.exported);
    assert.equal((await send(e.importRequest,true)).revision,'1');
    assert.equal((await send(e.relink,true)).revision,'3');
    assert.equal((await send({action:'read',workspace_id:e.importRequest.workspace_id})).revision,'3');
    console.log('PASS: actual app restart preserved export job, import/relink receipts and head');
  } else {
    const workspace_id='qa-'+crypto.randomUUID();
    const plan={experimentId:crypto.randomUUID(),format:'single',operator:{name:'QA declaration',id:''},rows:[{ref:'S1',sample:'Synthetic',condition:'A',experimentalUnit:'unit-1',concentrationValue:'',concentrationUnit:'',channel:'',medium:'',temperatureC:'',notes:'',free:[]}]};
    const profiles=path.resolve(evidencePath+'.profiles');await fs.mkdir(path.join(profiles,'method-a'),{recursive:true});
    await fs.writeFile(path.join(profiles,'method-a','config.json'),'{}');
    await fs.writeFile(path.join(profiles,'method-a','profile.meta.json'),JSON.stringify({profile_id:'qa-method'}));
    const profile=await call('profile_command',{base:profiles,request:JSON.stringify({operation:'read',name:'method-a'})});
    assert.equal(profile.ok,true);
    const recipe={schema_version:1,id:crypto.randomUUID(),revision:'1',title:'QA recipe',question:'',method:{source:'local',id:'qa-method',revision:profile.profile.revision},analysis_plan:{id:'not-a-provider-fixture',revision:'1'},lineage:{recipe:null,finding:null}};
    const document={schema_version:1,title:'Recovery QA',plan,recipe,definitions:{example:{kind:'metadata-test'}},findings:[{id:'f1',interpretation:'Not a scientific result'}]};
    const q={action:'commit',workspace_id,command_id:crypto.randomUUID(),expected_revision:null,document};
    assert.equal((await send(q,true)).revision,'1');assert.equal((await send(q,true)).revision,'1');
    const resolve={action:'resolve_recipe',workspace_id,revision:'1',profile_base:profiles};
    const resolved=await send(resolve);assert.equal(resolved.method.status,'resolved');assert.equal(resolved.compatibility,'unknown');
    await fs.writeFile(path.join(profiles,'method-a','config.json'),'{"changed":true}');
    assert.equal((await send(resolve)).method.status,'unresolved');
    const archive={...q,command_id:crypto.randomUUID(),expected_revision:'1',document:{...document,archived:true}};
    assert.equal((await send(archive,true)).revision,'2');
    assert.equal((await send({...archive,command_id:crypto.randomUUID()},true)).status,'conflict');
    const restore={action:'restore',workspace_id,expected_revision:'2',command_id:crypto.randomUUID(),target_revision:'1'};
    assert.equal((await send(restore,true)).revision,'3');assert.equal((await send(restore,true)).revision,'3');
    assert.deepEqual((await send({action:'history',workspace_id})).revisions,['1','2','3']);
    const exportRequest={action:'export',workspace_id,revision:'3',job_id:crypto.randomUUID()};
    const exported=await send(exportRequest,true);assert.equal(exported.state,'complete');
    assert.deepEqual(exported.manifest.payload.document,document);
    const importRequest={action:'import',workspace_id:'copy-'+crypto.randomUUID(),command_id:crypto.randomUUID(),manifest:exported.manifest};
    assert.equal((await send(importRequest,true)).revision,'1');
    const damaged=structuredClone(importRequest);damaged.workspace_id='bad-'+crypto.randomUUID();damaged.manifest.payload.document.title='tampered';
    assert.equal((await send(damaged,true)).status,'invalid');
    const file=path.resolve(evidencePath+'.source');const moved=file+'.moved';
    await fs.writeFile(file,'Synthetic immutable run source\n');await fs.copyFile(file,moved);
    const attach={action:'attach_source',workspace_id:importRequest.workspace_id,expected_revision:'1',command_id:crypto.randomUUID(),source_id:'run-1',path:file};
    assert.equal((await send(attach,true)).revision,'2');await fs.unlink(file);assert.equal((await send(attach,true)).revision,'2');
    const relink={...attach,action:'relink',expected_revision:'2',command_id:crypto.randomUUID(),path:moved};
    assert.equal((await send(relink,true)).revision,'3');
    assert.equal((await send({action:'verify_source',workspace_id:importRequest.workspace_id,revision:'3',source_id:'run-1'})).verified,true);
    await fs.writeFile(moved,'Altered bytes');
    const changed={...relink,expected_revision:'3',command_id:crypto.randomUUID()};
    assert.equal((await send(changed,true)).status,'conflict');
    assert.equal((await send({action:'read',workspace_id:importRequest.workspace_id})).revision,'3');
    assert.equal((await send({...q,workspace_id:'viewer-probe'})).status,'invalid');
    assert.ok((await send({action:'list',limit:100})).workspaces.some(w=>w.workspace_id===workspace_id));
    await fs.writeFile(evidencePath,JSON.stringify({workspace_id,exportRequest,exported,importRequest,relink},null,2));
    console.log('PASS: real app create/archive/history/CAS restore/export/import/relink/mismatch/replay; all payloads preserved');
  }
} finally {ws.close();clearTimeout(deadline);}
