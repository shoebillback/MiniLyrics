const {test}=require('node:test'),assert=require('node:assert/strict');
const {fixture,names}=require('./player-startup-fixture.cjs');

test('seek return attaches immediately without a state notification after missing synchronous confirmation',async()=>{
 const f=fixture({bootOnSeek:true,bootNotify:false,confirmDuringSeek:true});try{
  await Promise.resolve();const samples=[];f.api.addEventListener('timelineUpdate',e=>samples.push(e.detail));
  assert.equal(f.api.seekTo(120000),true);assert.equal(f.counts.Seek,1);assert.equal(samples.length,0,'a missed acknowledgement is not reconstructed');
  f.emit('PlayProgress','startup-track',121);
  assert.equal(samples.length,1,'later progress must be received before any poll');assert.equal(samples[0].currentTime,121000);
  const d=f.api.getDiagnostics();assert.equal(d.lastAcceptedSeekAt,null);assert.equal(d.hasActualProgress,true);
 }finally{f.api.dispose();}
});

test('initialization after dispatch with no notification recovers through normal sampling',()=>{
 const f=fixture();try{
  f.api.seekTo(50000);f.boot();f.emit('Seek','startup-track','early',0,50);
  assert.equal(f.counts.Seek,1);assert.equal(f.api.getDiagnostics().lastAcceptedSeekAt,null);
  f.api.getTimeline();f.emit('PlayProgress','startup-track',51);
  assert.equal(f.api.getTimeline().currentTime,51000);assert.equal(f.api.getDiagnostics().hasActualProgress,true);
 }finally{f.api.dispose();}
});

test('independently registered events leave absent slots free and recover on sampling',()=>{
 const f=fixture();try{
  f.api.seekTo(50000);f.original('PlayState');f.emit('PlayState','startup-track','early',1);f.api.getTimeline();
  let d=f.api.getDiagnostics();assert.equal(d.subscriptions.PlayState,'subscribed');assert.equal(d.subscriptions.Seek,'waiting');assert.equal(d.subscriptions.PlayProgress,'waiting');
  assert.equal(f.adapter.registerCallMap['audioplayer.onSeek'],undefined);
  f.original('Seek');f.api.getTimeline();f.emit('Seek','startup-track','later',0,50);
  d=f.api.getDiagnostics();assert.equal(d.lastAcceptedSeekAt,100);assert.equal(d.hasActualProgress,false);
  assert.equal(f.adapter.registerCallMap['audioplayer.onPlayProgress'],undefined);
  f.original('PlayProgress');f.api.getTimeline();f.emit('PlayProgress','startup-track',51);
  assert.equal(f.api.getDiagnostics().hasActualProgress,true);
 }finally{f.api.dispose();}
});

test('restored startup position is separate from control readiness and actual progress',async()=>{
 const f=fixture();try{
  await Promise.resolve();const d=f.api.getDiagnostics();
  assert.equal(d.canControl,true);assert.equal(d.hasActualProgress,false);assert.equal(d.timelineSource,'restore');
  assert.equal(d.lastReceivedProgressAt,null);assert.equal(d.lastAcceptedProgressAt,null);assert.equal(d.lastAcceptedSeekAt,null);
  assert.equal(d.streamState,'waiting-subscription');assert.equal(d.originalInitialization,'unverified');
  assert.equal(f.api.getTimeline().currentTime,82000);
 }finally{f.api.dispose();}
});

test('received, accepted, mismatched and invalid native progress have separate diagnostics',()=>{
 const f=fixture({warm:true});try{
  f.emit('PlayProgress','old-song',12);let d=f.api.getDiagnostics();
  assert.equal(d.lastReceivedProgressAt,100);assert.equal(d.lastAcceptedProgressAt,null);assert.equal(d.events.PlayProgress.filtered,1);
  assert.equal(d.events.PlayProgress.lastFilterReason,'song-mismatch');assert.equal(d.streamState,'events-filtered');
  f.advance(10);f.emit('PlayProgress','startup-track','13');d=f.api.getDiagnostics();assert.equal(d.events.PlayProgress.lastFilterReason,'invalid-time');
  f.advance(10);f.emit('PlayProgress','startup-track',13);d=f.api.getDiagnostics();
  assert.equal(d.hasActualProgress,true);assert.equal(d.lastAcceptedProgressAt,120);assert.equal(d.events.PlayProgress.received,3);assert.equal(d.events.PlayProgress.accepted,1);
  assert.equal(d.events.PlayProgress.lastFilterReason,null);assert.equal(d.timelineSource,'progress');
  f.update({resourceTrackId:'new-track',onlineResourceId:456});d=f.api.getDiagnostics();assert.equal(d.hasActualProgress,false);assert.equal(d.lastAcceptedProgressAt,null);
 }finally{f.api.dispose();}
});

test('seek confirmation and failures do not count as continuous native progress',()=>{
 const f=fixture({warm:true});try{
  f.emit('Seek','startup-track','bad',1,50);let d=f.api.getDiagnostics();assert.equal(d.events.Seek.lastFilterReason,'seek-failed');assert.equal(d.lastAcceptedSeekAt,null);
  f.advance(10);f.emit('Seek','startup-track','ok',0,50);d=f.api.getDiagnostics();assert.equal(d.lastAcceptedSeekAt,110);assert.equal(d.hasActualProgress,false);assert.equal(d.timelineSource,'seek');
  f.emit('PlayState','old-song','pause',2);assert.equal(f.api.getDiagnostics().events.PlayState.lastFilterReason,'song-mismatch');
 }finally{f.api.dispose();}
});

test('stalled live progress is distinct from paused progress and control availability',()=>{
 const f=fixture({warm:true});try{
  f.emit('PlayState','startup-track','play',1);f.emit('PlayProgress','startup-track',1);
  assert.equal(f.api.getDiagnostics().streamState,'progress-live');f.advance(5000);
  let d=f.api.getDiagnostics();assert.equal(d.streamState,'progress-stalled');assert.equal(d.canControl,true);
  f.emit('PlayState','startup-track','pause',2);f.advance(5000);assert.equal(f.api.getDiagnostics().streamState,'progress-paused');
 }finally{f.api.dispose();}
});

test('diagnostics report a removed subscription without re-registering or switching bridges',()=>{
 const f=fixture({warm:true});try{
  f.adapter.removeRegisterCall('PlayProgress','audioplayer');const callback=f.slots.get('audioplayer.onPlayProgress');
  const d=f.api.getDiagnostics();assert.equal(d.subscriptions.PlayProgress,'lost');assert.equal(d.streamState,'subscription-lost');
  assert.equal(f.adapter.registerCallMap['audioplayer.onPlayProgress'],undefined);assert.equal(f.slots.get('audioplayer.onPlayProgress'),callback);assert.equal(f.legacyReads(),0);
 }finally{f.api.dispose();}
});

test('a foreign placeholder does not establish original initialization or fix its first-registration failure',async()=>{
 const f=fixture();try{
  await Promise.resolve();let foreign=0;f.Bridge.appendRegisterCall('PlayProgress','audioplayer',()=>foreign++);
  f.api.getCurrentSong();const d=f.api.getDiagnostics();
  assert.equal(d.subscriptions.PlayProgress,'subscribed');assert.equal(d.originalInitialization,'unverified');assert.equal(d.hasActualProgress,false);assert.equal(d.streamState,'waiting-progress');
  assert.throws(()=>f.original('PlayProgress'),/already a/,'foreign preemption remains outside the empty-slot guard');
  assert.equal(f.counts.PlayProgress,0);assert.equal(foreign,0);assert.equal(f.adapter.registerCallMap['audioplayer.onSeek'],undefined);assert.equal(f.legacyReads(),0);
 }finally{f.api.dispose();}
});