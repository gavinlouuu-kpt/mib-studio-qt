// No take_control: first connection owns control only on an otherwise unused QA runtime.
import assert from 'node:assert/strict';
const url=process.argv[2];const clients=[];let sequence=0;const deadline=setTimeout(()=>process.exit(99),10000);
async function connect(){const ws=new WebSocket(url);clients.push(ws);let session;ws.addEventListener('message',e=>{if(typeof e.data==='string'){const r=JSON.parse(e.data);if(r.session)session={...session,...r.session};}});await new Promise((r,j)=>{ws.onopen=r;ws.onerror=j;});return {ws,get session(){return session;}};}
async function call(c,cmd,args){return new Promise(resolve=>{const id=++sequence;const cb=e=>{if(typeof e.data!=='string')return;const r=JSON.parse(e.data);if(r.request_id===id){c.ws.removeEventListener('message',cb);resolve(r);}};c.ws.addEventListener('message',cb);c.ws.send(JSON.stringify({request_id:id,cmd,args}));});}
try{const controller=await connect(),viewer=await connect();
 const read=await call(viewer,'workspace_read',{request:JSON.stringify({action:'list',limit:1})});assert.equal(read.ok.status,'ok');
 assert.equal(controller.session.client_id,controller.session.controller_id,'QA is in use; do not take control');
 for(const cmd of ['workspace_write','analysis_write']){const r=await call(viewer,cmd,{request:cmd==='workspace_write'?'{}':{}});assert.match(r.error,/VIEWER_ONLY/);}
 assert.equal(controller.session.client_id,controller.session.controller_id);console.log('PASS real viewer reads, workspace/strategy writes denied, original controlling connection preserved');
}finally{clients.forEach(c=>c.close());clearTimeout(deadline);}
