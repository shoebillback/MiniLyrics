const {test}=require('node:test'),assert=require('node:assert/strict');
const {fixture}=require('./bridge-fixture.cjs');
async function run(fn,call=()=>true){const f=fixture({channelCall:call});await f.open();try{await fn(f);}finally{f.dispose();}}
const seek=(f,p)=>f.context.channel.call('audioplayer.seek',()=>{},['play-id','seek-id',p]);
test('RNP-style seek observation preserves call arguments, receiver and result',()=>{
 let receiver,argumentsSeen;function original(...args){receiver=this;argumentsSeen=args;return 42;}
 return run(f=>{const bridge=f.context.channel,cb=()=>{},payload=['play-id','seek-id',60];
 assert.equal(bridge.call('audioplayer.seek',cb,payload),42);assert.equal(receiver,bridge);assert.equal(argumentsSeen[1],cb);assert.equal(argumentsSeen[2],payload);
 assert.equal(f.sent.filter(m=>m.type==='seek'&&!m.requestId).at(-1).position,60000);
 f.emitTimeline(11000);assert.equal(f.diagnostics().positionMs,60000);f.emitTimeline(60100);assert.equal(f.diagnostics().positionMs,60100);
 },original);
});
test('native correction alone is not an animated seek and invalid commands are ignored',()=>run(f=>{
 f.emitTimeline(50000);assert.equal(f.sent.filter(m=>m.type==='seek'&&!m.requestId).length,0);
 for(const target of [NaN,Infinity,-1,null,undefined,''])seek(f,target);
 f.context.channel.call('audioplayer.pause',()=>{},['id','id',1]);assert.equal(f.sent.filter(m=>m.type==='seek'&&!m.requestId).length,0);
}));
test('RNP hook mount, cleanup and reattachment keep exactly one active observer',()=>run(f=>{
 const originalObserver=f.context.channel.call;let rnpCalls=0;
 function rnpHook(...args){rnpCalls++;return Reflect.apply(originalObserver,this,args);}
 f.context.channel.call=rnpHook;f.advance(1000);seek(f,60);
 assert.equal(rnpCalls,1);assert.equal(f.sent.filter(m=>m.type==='seek'&&!m.requestId).length,1);
 f.context.channel.call=originalObserver;f.advance(1000);seek(f,20);
 assert.equal(f.sent.filter(m=>m.type==='seek'&&!m.requestId).length,2);
}));
test('own seek is not animated twice; pause and continuous external seeks remain supported',()=>run(f=>{
 f.setSeek(target=>seek(f,target/1000));f.seek(60000);assert.equal(f.sent.filter(m=>m.type==='seek'&&!m.requestId).length,0);
 f.emitTimeline(60100);f.advance(1500);f.emitPlay('Paused');seek(f,10);seek(f,40);
 assert.equal(f.sent.filter(m=>m.type==='seek'&&!m.requestId).length,2);assert.equal(f.diagnostics().playing,false);
 f.emitTimeline(40100,'Paused');assert.equal(f.diagnostics().positionMs,40100);
}));
test('disposing never removes a newer RNP hook; stale observer becomes pass-through',async()=>{
 let calls=0;const original=()=>++calls;const f=fixture({channelCall:original});await f.open();
 const observer=f.context.channel.call,newer=function(...args){return Reflect.apply(observer,this,args)};
 f.context.channel.call=newer;f.dispose();assert.equal(f.context.channel.call,newer);
 const count=f.sent.filter(m=>m.type==='seek'&&!m.requestId).length;seek(f,30);
 assert.equal(calls,1);assert.equal(f.sent.filter(m=>m.type==='seek'&&!m.requestId).length,count);
});
