import { expect, it, vi } from "vitest";
import { FramePullScheduler } from "./framePullScheduler";
import type { FramePacket } from "./framePacket";
function barrier<T>() { let resolve!: (v:T)=>void; const promise=new Promise<T>(r=>{resolve=r;}); return {promise,resolve}; }
const frame=(id:string)=>({frame_index:id,data:new Uint8Array([Number(id)%256])} as FramePacket);
async function drain() { for(let i=0;i<32;i++) await Promise.resolve(); }
it("replaces pending intent and discards late replies without cancelling native work",async()=>{
  const s=new FramePullScheduler(), a=barrier<FramePacket>(), delivered=vi.fn();
  s.mount("review",delivered,e=>{throw e;}); s.request("review",()=>a.promise); await drain();
  for(let i=1;i<=50;i++) s.request("review",async()=>frame(String(i)));
  expect(s.snapshot()).toMatchObject({inFlight:1,pending:1,replacements:49});
  a.resolve(frame("0")); await drain(); expect(delivered).toHaveBeenCalledTimes(1);
  expect(delivered.mock.calls[0][0].frame_index).toBe("50");
  expect(s.snapshot()).toMatchObject({inFlight:0,pending:0,staleReplies:1});
});
it("100 unmount/remount cycles cannot deliver to retired consumers",async()=>{
  const s=new FramePullScheduler();
  for(let i=0;i<100;i++) {
    const b=barrier<FramePacket>(), retired=vi.fn(), current=vi.fn();
    const unmount=s.mount("live",retired,retired); s.request("live",()=>b.promise); await drain(); unmount();
    const dispose=s.mount("live",current,current); b.resolve(frame(String(i))); await drain();
    expect(retired).not.toHaveBeenCalled(); expect(current).not.toHaveBeenCalled(); dispose();
    expect(s.snapshot()).toMatchObject({views:0,inFlight:0,pending:0});
  }
});
it("10,000 interleaved pulls stay within aggregate count budgets",async()=>{
  const s=new FramePullScheduler(); let received=0;
  const ids=["live","review","background","thumbnail"];
  const unmounts=ids.map(id=>s.mount(id,()=>received++,e=>{throw e;}));
  for(let i=0;i<2500;i++) {
    for(const id of ids) s.request(id,async()=>frame(String(i))); await drain();
    expect(s.snapshot().inFlight).toBeLessThanOrEqual(1); expect(s.snapshot().pending).toBeLessThanOrEqual(4);
  }
  await drain(); expect(received).toBe(10000); unmounts.forEach(f=>f());
  expect(s.snapshot()).toMatchObject({views:0,pending:0,inFlight:0});
});
it("source invalidation discards pending frames and late completion",async()=>{
  const s=new FramePullScheduler(), b=barrier<FramePacket>(), deliver=vi.fn();
  s.mount("live",deliver,deliver); s.request("live",()=>b.promise); await drain();
  s.request("live",async()=>frame("2")); s.invalidate();
  expect(s.snapshot()).toMatchObject({inFlight:1,pending:0}); b.resolve(frame("1")); await drain(); expect(deliver).not.toHaveBeenCalled();
});
it("slow live preview remains displayable while a sampled successor waits",async()=>{
  const s=new FramePullScheduler(), b=barrier<FramePacket>(), delivered=vi.fn();
  s.mount("live",delivered,delivered); s.request("live",()=>b.promise,false); await drain();
  for(let i=0;i<50;i++) s.request("live",async()=>frame("2"),false);
  b.resolve(frame("1")); await drain();
  expect(delivered.mock.calls.map(c=>c[0].frame_index)).toEqual(["1","2"]);
});
