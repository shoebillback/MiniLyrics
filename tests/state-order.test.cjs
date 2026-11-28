const {test}=require('node:test'),assert=require('node:assert/strict');
const {fixture}=require('./bridge-fixture.cjs');
async function pump(f,condition){for(let i=0;i<30&&!condition();i++){await new Promise(setImmediate);if(!condition())f.advance(200);}assert.ok(condition());}
const close=f=>f.configControl('miniLyrics-close').onclick();
test('page disposal during handshake disconnects without closing the persistent host',async()=>{
 const f=fixture({delaySockets:true});try{const p=f.context.__miniLyrics.open();await pump(f,()=>f.sockets.length===1);
  f.dispose();f.sockets[0].connect();await p;
  assert.deepEqual(f.sent,[]);assert.equal(f.sockets[0].readyState,3);assert.equal(f.apiEvents.size,0);
 }finally{f.dispose();}
});
test('cancelled connection cannot change a newer window or forward stale commands',async()=>{
 const f=fixture({delaySockets:true});try{const old=f.context.__miniLyrics.open();await pump(f,()=>f.sockets.length===1);close(f);
  const next=f.context.__miniLyrics.open();await pump(f,()=>f.sockets.length===2);f.sockets[1].connect();await next;await new Promise(setImmediate);
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
const sessionKey='MiniLyrics.window';
test('a crashed host retries ten times, clears its session and can be launched again',async()=>{
 let dead=false;
 const f=fixture({socketBehavior:()=>dead?'refuse':null});
 try{
  await f.open();assert.ok(f.storage.has(sessionKey));dead=true;f.closeSocket();
  const attemptsBefore=f.sockets.length;
  f.advance(1000);await new Promise(setImmediate);
  for(let i=1;i<9;i++){f.advance(2000);await new Promise(setImmediate);}
  assert.equal(f.sockets.length-attemptsBefore,9);assert.ok(f.storage.has(sessionKey));
  f.advance(2000);await new Promise(setImmediate);
  assert.equal(f.sockets.length-attemptsBefore,10);assert.equal(f.storage.has(sessionKey),false);
  assert.equal(f.context.__miniLyrics.getStatus(),'小窗连接失败，请重新打开');
  f.events.get('focus')();f.emitTimeline(12000);
  assert.equal(f.context.__miniLyrics.getStatus(),'小窗连接失败，请重新打开');
  f.advance(60000);await new Promise(setImmediate);
  assert.equal(f.sockets.length-attemptsBefore,10);
  dead=false;await f.open();
  assert.equal(f.launches.length,2);assert.ok(f.storage.has(sessionKey));
 }finally{f.dispose();}
});
test('opening probes the saved port once and replaces a socket that never answers the handshake',async()=>{
 const port=42001,storage=new Map([[sessionKey,JSON.stringify({port,token:'a'.repeat(48)})]]);
 const f=fixture({storage,socketBehavior:url=>url.includes(':'+port+'/')?'silent':null});
 try{
  await f.open();
  assert.equal(f.sockets.filter(ws=>ws.url.includes(':'+port+'/')).length,1);
  assert.equal(f.sockets[0].readyState,3);assert.equal(f.launches.length,1);
  assert.notEqual(JSON.parse(storage.get(sessionKey)).port,port);
  assert.equal(f.context.__miniLyrics.getStatus(),'已连接');
 }finally{f.dispose();}
});
