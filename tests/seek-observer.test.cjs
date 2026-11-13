const {test}=require('node:test'),assert=require('node:assert/strict');
const {fixture}=require('./bridge-fixture.cjs');
test('channel hooks remain untouched and outgoing seek calls never trigger animation',async()=>{
 let calls=0;const original=function(){calls++;return 42;};const f=fixture({channelCall:original});
 try{await f.open();assert.equal(f.context.channel.call,original);
  const newer=function(...args){return Reflect.apply(original,this,args);};f.context.channel.call=newer;f.advance(1000);
  assert.equal(f.context.channel.call,newer);assert.equal(newer('audioplayer.seek',()=>{},['123','call-id',20]),42);
  assert.equal(calls,1);assert.equal(f.sent.filter(m=>m.type==='seek').length,0);
  f.emitTimeline(20000);assert.equal(f.sent.filter(m=>m.type==='seek').length,0);assert.equal(f.sent.filter(m=>m.type==='clock').at(-1).position,20000);
  f.dispose();assert.equal(f.context.channel.call,newer);
 }finally{f.dispose();}
});
