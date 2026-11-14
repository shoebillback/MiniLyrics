const {test}=require('node:test'),assert=require('node:assert/strict');
const {fixture}=require('./player-startup-fixture.cjs');
// Only model the branches supplied from the user's live NCM effects. This
// verifies command ordering and guards, not a real player's initialization.
function controlFixture(patch={}){
 const f=fixture({warm:true}),nativeSeeks=[];let starts=0;
 f.update(patch);
 f.setDispatch(action=>{
  const p=f.state(),seconds=action.payload?.duration;
  if(action.type==='playing/setPlayingPosition'){
   if(p.restoreResource)f.update({restoreResource:{...p.restoreResource,current:seconds}});
   else if(p.isLoadingFirst)f.update({loadingSeekDuration:seconds});
   else{assert.ok(p.playId,'a native seek requires a loaded playback session');nativeSeeks.push(seconds);}
  }else if(action.type==='playing/onLoadingSeekUpdate')f.update({loadingSeekDuration:action.payload.loadingSeekDuration});
  else if(action.type==='playing/resume'){starts++;f.update({playingState:2,isLoadingFirst:true});}
  else if(action.type==='playing/pause')f.update({playingState:1});
  else if(action.type==='playing/switchResumeOrPause')f.update({playingState:p.playingState===2?1:2});
 });
 return {...f,nativeSeeks,starts:()=>starts,complete:()=>f.update({playId:'loaded-session',restoreResource:null,isLoadingFirst:false})};
}
const cold={playId:null,restoreResource:{id:'startup-track',current:82},isLoadingFirst:false,playingState:1};
const types=f=>f.commands.map(a=>a.type);

test('control flow: restored first seek queues its target before original resume',async()=>{
 const f=controlFixture(cold);try{
  await Promise.resolve();assert.equal(f.api.seekTo(50000,1),true);
  assert.deepEqual(types(f),['playing/setPlayingPosition','playing/resume']);assert.equal(f.state().restoreResource.current,50);assert.deepEqual(f.nativeSeeks,[]);
  f.complete();await Promise.resolve();assert.deepEqual(f.nativeSeeks,[50]);
  assert.equal(f.api.getTimeline().source,'restore','sending controls does not invent progress');
 }finally{f.api.dispose();}
});

test('control flow: cold seek to zero clears the old loading position before resume',async()=>{
 const f=controlFixture({...cold,loadingSeekDuration:82});try{
  assert.equal(f.api.seekTo(0,1),true);assert.equal(f.state().restoreResource.current,0);assert.equal(f.state().loadingSeekDuration,0);
  assert.deepEqual(types(f),['playing/setPlayingPosition','playing/onLoadingSeekUpdate','playing/resume']);
  f.complete();await Promise.resolve();assert.deepEqual(f.nativeSeeks,[0]);
 }finally{f.api.dispose();}
});

test('control flow: missing playId never sends a direct native seek',async()=>{
 const f=controlFixture({...cold,restoreResource:null});try{
  assert.equal(f.api.seekTo(45000,1),true);assert.deepEqual(types(f),['playing/onLoadingSeekUpdate','playing/resume']);assert.deepEqual(f.nativeSeeks,[]);
  f.complete();await Promise.resolve();assert.deepEqual(f.nativeSeeks,[45]);
 }finally{f.api.dispose();}
});

test('control flow: loading seeks keep the latest target and do not restart playback',async()=>{
 const f=controlFixture({...cold,restoreResource:null,isLoadingFirst:true});try{
  f.api.seekTo(60000,1);f.api.seekTo(30000,2);assert.equal(f.starts(),0);assert.equal(f.state().loadingSeekDuration,30);
  f.complete();await Promise.resolve();assert.deepEqual(f.nativeSeeks,[30]);
 }finally{f.api.dispose();}
});

test('control flow: repeated startup seeks issue only one original resume',async()=>{
 const f=controlFixture(cold);try{
  f.api.seekTo(60000,1);f.api.seekTo(30000,2);assert.equal(f.starts(),1);
  f.complete();await Promise.resolve();assert.deepEqual(f.nativeSeeks,[30]);
 }finally{f.api.dispose();}
});

test('control flow: a loaded paused session seeks without starting playback',()=>{
 const f=controlFixture({playingState:1});try{
  f.api.seekTo(30000,1);assert.deepEqual(types(f),['playing/setPlayingPosition']);assert.deepEqual(f.nativeSeeks,[30]);assert.equal(f.state().playingState,1);assert.equal(f.starts(),0);
 }finally{f.api.dispose();}
});

test('control flow: pause, track change and dispose cancel the deferred MiniLyrics seek',async()=>{
 for(const action of ['pause','track','dispose']){
  const f=controlFixture(cold);try{
   f.api.seekTo(50000,1);
   if(action==='pause')f.api.pause();else if(action==='track')f.update({resourceTrackId:'another-track',onlineResourceId:456,curPlaying:{resourceId:'another-track',track:{id:456}}});else f.api.dispose();
   f.complete();await Promise.resolve();assert.deepEqual(f.nativeSeeks,[],action);
  }finally{f.api.dispose();}
 }
});

test('control flow: playback toggle delegates the decision to the original model',()=>{
 const f=controlFixture({playingState:1});try{
  assert.equal(f.api.togglePlay(),true);assert.equal(f.state().playingState,2);assert.equal(f.api.togglePlay(),true);assert.equal(f.state().playingState,1);
  assert.deepEqual(types(f),['playing/switchResumeOrPause','playing/switchResumeOrPause']);
 }finally{f.api.dispose();}
});

test('control flow: no restored resource means no invalid startup command',()=>{
 const f=controlFixture({...cold,restoreResource:null,curPlaying:null});try{
  assert.equal(f.api.seekTo(10000,1),false);assert.deepEqual(f.commands,[]);
 }finally{f.api.dispose();}
});