const {test}=require('node:test'),assert=require('node:assert/strict');
const {fixture}=require('./bridge-fixture.cjs');
async function pump(f,condition){for(let i=0;i<30&&!condition();i++){await new Promise(setImmediate);if(!condition())f.advance(200);}assert.ok(condition());}
const close=f=>f.config().children.find(c=>c.textContent==='关闭小窗').onclick();
test('page disposal during handshake disconnects without closing the persistent host',async()=>{
 const f=fixture({delaySockets:true});try{const p=f.context.__miniLyrics.open();await pump(f,()=>f.sockets.length===1);
  f.dispose();f.sockets[0].connect();await p;
  assert.deepEqual(f.sent,[]);assert.equal(f.sockets[0].readyState,3);assert.equal(f.apiEvents.size,0);
 }finally{f.dispose();}
});
test('cancelled connection cannot change a newer window or forward stale commands',async()=>{
 const f=fixture({delaySockets:true});try{const old=f.context.__miniLyrics.open();await pump(f,()=>f.sockets.length===1);close(f);
  const next=f.context.__miniLyrics.open();await pump(f,()=>f.sockets.length===2);f.sockets[1].connect();await next;
  const n=f.sent.length;f.sockets[0].connect();await old;
  assert.deepEqual(f.sent.slice(n).map(m=>m.type),['hello','exit']);assert.equal(f.sockets[1].readyState,1);assert.equal(f.context.__miniLyrics.getStatus(),'已连接');
  const handler=f.sockets[1].onmessage;close(f);handler({data:JSON.stringify({type:'command',action:'next'})});assert.equal(f.controls.length,0);
 }finally{f.dispose();}
});
test('cancellation while launcher is pending retires that host and permits a new open',async()=>{
 let complete;const f=fixture({exec:()=>new Promise(r=>complete=r)});try{
  const old=f.context.__miniLyrics.open();close(f);complete(true);let done=false;old.finally(()=>done=true);await pump(f,()=>done);await old;
  assert.deepEqual(f.sent.map(m=>m.type),['hello','exit']);assert.equal(f.context.__miniLyrics.getStatus(),'小窗已关闭');
 }finally{f.dispose();}
});
