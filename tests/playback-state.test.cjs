const {test}=require('node:test'),assert=require('node:assert/strict');
const {createPlaybackState}=require('../core.js');
const {fixture}=require('./bridge-fixture.cjs');

test('sustained real-time progress corrects a stale paused provider flag',()=>{
 const state=createPlaybackState();
 assert.equal(state.sample(10000,0,'Paused'),false);
 assert.equal(state.sample(10500,500,'Paused'),false,'one update is insufficient');
 assert.equal(state.sample(11000,1000,'Paused'),true);
 assert.equal(state.sample(11500,1500,'Paused'),true);
 assert.deepEqual(state.snapshot(),{reportedPlaying:false,playing:true,inferredFromProgress:true,progressSamples:3});
 assert.equal(state.sample(11500,3500,'Paused'),false,'stationary progress revokes the inferred state');
});

test('explicit pause, repeated reads, jumps and pending seeks do not establish playback',()=>{
 const state=createPlaybackState();
 state.event('Playing');assert.equal(state.sample(10000,0,'Playing'),true);
 state.event('Paused');assert.equal(state.current(),false);
 for(const time of [100,500,1000,2000])assert.equal(state.sample(10000,time,'Paused'),false);
 assert.equal(state.sample(60000,2500,'Paused'),false,'large external seek');
 assert.equal(state.sample(20000,3000,'Paused'),false,'backward external seek');
 for(let i=1;i<=4;i++)assert.equal(state.sample(20000+i*500,3000+i*500,'Paused',true),false,'pending widget seek');
 state.reset();assert.equal(state.sample(30000,6000,'Paused'),false,'new track or connection has no old evidence');
});

test('MiniLyrics forwards the corrected state and still honors a new pause event',async()=>{
 const f=fixture();
 try{
  await f.open();f.emitPlay('Paused');
  f.advance(500);f.emitTimeline(10500,'Paused');assert.equal(f.diagnostics().playing,false);
  f.advance(500);f.emitTimeline(11000,'Paused');assert.equal(f.diagnostics().playing,true);
  assert.equal(f.sent.filter(m=>m.type==='clock').at(-1).playing,true);
  assert.equal(f.diagnostics().playbackState.reportedPlaying,false);
  f.emitPlay('Paused');assert.equal(f.diagnostics().playing,false);
  f.seek(50000);f.emitTimeline(50000,'Paused');assert.equal(f.diagnostics().playing,false);
 }finally{f.dispose();}
});
