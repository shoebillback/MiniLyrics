const {test}=require('node:test'),assert=require('node:assert/strict');
const {fixture}=require('./bridge-fixture.cjs');
async function run(fn){const f=fixture();try{await f.open();await fn(f);}finally{f.dispose();}}
const clocks=f=>f.sent.filter(m=>m.type==='clock');
test('widget sends a command without optimistic movement, then accepts actual position',()=>run(f=>{
 const p=f.diagnostics().positionMs;f.seek(60000);assert.deepEqual(f.seeks,[60000]);assert.equal(f.diagnostics().positionMs,p);
 f.emitTimeline(60100);assert.equal(f.diagnostics().positionMs,60100);assert.equal(f.diagnostics().seekPending,false);assert.equal(clocks(f).at(-1).seekState,'confirmed');
 assert.equal(f.sent.some(m=>m.type==='seek'),false);
}));
test('widget then rapid original timeline changes are both delivered without a guard period',()=>run(f=>{
 f.seek(60000);f.emitTimeline(60000);f.advance(50);f.emitTimeline(20000);
 assert.equal(f.diagnostics().positionMs,20000);assert.equal(clocks(f).at(-1).position,20000);
 f.advance(2600);assert.equal(clocks(f).some(m=>m.seekState==='failed'),false);
}));
test('original timeline supersedes an unconfirmed widget command',()=>run(f=>{
 f.seek(60000);f.emitTimeline(20000);assert.equal(f.diagnostics().positionMs,20000);assert.equal(f.diagnostics().seekPending,false);
}));
test('normal samples keep pending requests alive until the actual target is reached',()=>run(f=>{
 f.seek(60000);f.emitTimeline(11000);assert.equal(f.diagnostics().seekPending,true);
 f.emitTimeline(60000);assert.equal(f.diagnostics().seekPending,false);
}));
test('synchronous confirmation survives later asynchronous rejection',()=>run(async f=>{
 f.setSeek(target=>{f.emitTimeline(target);return Promise.resolve(false);});f.seek(30000);await Promise.resolve();
 assert.equal(f.diagnostics().positionMs,30000);assert.equal(clocks(f).some(m=>m.seekState==='failed'),false);
}));
test('open, close, reopen and dispose never read or alter native registrations',async()=>{
 const f=fixture({noNativeEvents:true});let accesses=0;const original=f.context.channel;
 Object.defineProperty(f.context,'legacyNativeCmder',{get(){accesses++;throw Error('native bridge must remain untouched');}});
 f.context.channel=new Proxy(original,{get(){accesses++;throw Error('channel must remain untouched');},set(){accesses++;throw Error('channel must remain untouched');}});
 try{await f.open();f.emitTimeline(60000);f.advance(1000);f.config().children.find(c=>c.textContent==='关闭小窗').onclick();await f.open();f.dispose();
  assert.equal(accesses,0);
 }finally{f.dispose();}
});
