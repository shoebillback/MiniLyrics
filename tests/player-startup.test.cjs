const {test}=require('node:test'),assert=require('node:assert/strict');
const {createPlayerAdapter}=require('../player.js');
const {fixture,names}=require('./player-startup-fixture.cjs');

test('cold startup leaves native first-registration slots available',async()=>{
 const f=fixture();try{
  assert.equal(f.api.ready,true);assert.equal(f.legacyReads(),0);
  await Promise.resolve();assert.equal(f.api.getTimeline().currentTime,82000);
  for(const name of names)assert.equal(f.adapter.registerCallMap['audioplayer.on'+name],undefined,`${name} must remain unreserved`);
  assert.deepEqual(f.boot(),[true,true,true]);
  f.api.getCurrentSong();f.emit('PlayProgress','startup-track',12);
  assert.equal(f.counts.PlayProgress,1);assert.equal(f.api.getTimeline().currentTime,12000);
 }finally{f.api.dispose();}
});

test('direct cold-start seek cannot prevent strict native callback initialization',()=>{
 const f=fixture({bootOnSeek:true});try{
  assert.equal(f.api.ready,true);
  assert.doesNotThrow(()=>assert.equal(f.api.seekTo(120000),true));
  assert.deepEqual(f.commands,[{type:'playing/setPlayingPosition',payload:{duration:120}}]);
  f.emit('Seek','startup-track','request',0,120);f.emit('PlayProgress','startup-track',121);
  assert.equal(f.counts.Seek,1);assert.equal(f.counts.PlayProgress,1);assert.equal(f.api.getTimeline().currentTime,121000);
 }finally{f.api.dispose();}
});

test('default native registration is not silently rejected after MiniLyrics loads',()=>{
 const f=fixture({mode:null});try{
  assert.deepEqual(f.boot(),[true,true,true]);f.api.getCurrentSong();
  f.emit('PlayProgress','startup-track',3);assert.equal(f.counts.PlayProgress,1);assert.equal(f.api.getTimeline().currentTime,3000);
 }finally{f.api.dispose();}
});

test('original playback first, disposal and reopening retain native progress handlers',()=>{
 const f=fixture({warm:true});try{
  f.emit('PlayProgress','startup-track',4);assert.equal(f.api.getTimeline().currentTime,4000);assert.equal(f.counts.PlayProgress,1);
  f.api.getCurrentSong();f.api.getCurrentSong();f.emit('PlayProgress','startup-track',5);assert.equal(f.counts.PlayProgress,2);
  assert.equal(f.adapter.registerCallMap['audioplayer.onPlayProgress'].length,2,'only one MiniLyrics callback');
  f.api.dispose();f.emit('PlayProgress','startup-track',6);assert.equal(f.counts.PlayProgress,3);
  const next=createPlayerAdapter(f.env);try{f.emit('PlayProgress','startup-track',7);assert.equal(next.getTimeline().currentTime,7000);assert.equal(f.counts.PlayProgress,4);}finally{next.dispose();}
 }finally{f.api.dispose();}
});

test('partially initialized native events wait independently for their original handler',()=>{
 const f=fixture();try{
  assert.equal(f.original('PlayState'),true);f.api.getCurrentSong();
  f.emit('PlayState','startup-track','play',1);assert.equal(f.api.getPlaybackStatus(),'Playing');
  assert.equal(f.adapter.registerCallMap['audioplayer.onPlayProgress'],undefined);
  assert.equal(f.adapter.registerCallMap['audioplayer.onSeek'],undefined);
  assert.equal(f.original('PlayProgress'),true);assert.equal(f.original('Seek'),true);f.api.getCurrentSong();
  f.emit('Seek','startup-track','seek',0,9);assert.equal(f.api.getTimeline().currentTime,9000);
 }finally{f.api.dispose();}
});

test('replacement native handler is observed without duplicate MiniLyrics listeners',()=>{
 const f=fixture({warm:true});try{
  assert.equal(f.original('PlayProgress',1),true);f.notify();f.api.getCurrentSong();
  f.emit('PlayProgress','startup-track',8);assert.equal(f.counts.PlayProgress,1);assert.equal(f.api.getTimeline().currentTime,8000);
  assert.equal(f.adapter.registerCallMap['audioplayer.onPlayProgress'].length,2);
 }finally{f.api.dispose();}
});
test('unknown callback registry waits without registering or sending a seek',()=>{
 const f=fixture({inspectable:false});try{assert.equal(f.api.ready,false);assert.equal(f.api.seekTo(10000),false);assert.equal(f.slots.size,0);assert.equal(f.commands.length,0);assert.equal(f.legacyReads(),0);}finally{f.api.dispose();}
});
