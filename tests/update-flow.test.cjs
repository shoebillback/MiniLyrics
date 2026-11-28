const {test}=require('node:test'),assert=require('node:assert/strict');
const {fixture,findControl:setting}=require('./bridge-fixture.cjs');
const settle=()=>new Promise(setImmediate);
test('rapid A to B to A ignores stale results even when an aborted request still resolves',async()=>{
 const replies=[];const f=fixture({fetchLyrics:()=>new Promise(resolve=>replies.push(resolve))});
 try{await f.open();const song=f.info();
  f.setInfo({...song,trackId:'local-b',ncmId:456});f.emitTimeline(0);
  f.setInfo(song);f.emitTimeline(0);
  assert.deepEqual(f.lyricRequests.map(request=>[request.id,request.signal.aborted]),[['123',true],['456',true],['123',false]]);
  replies[2]({code:200,lrc:{lyric:'[00:01]current'}});await settle();
  replies[0]({code:200,lrc:{lyric:'[00:01]stale A'}});replies[1]({code:200,lrc:{lyric:'[00:01]stale B'}});await settle();
  const updates=f.sent.filter(m=>m.type==='lyrics');assert.equal(updates.length,1);assert.equal(updates[0].lines[0].text,'current');
 }finally{f.dispose();}
});
test('play-state events send one clock, preserving pause/resume and seek confirmation',async()=>{
 const f=fixture();try{await f.open();const clocks=()=>f.sent.filter(m=>m.type==='clock');
  let n=clocks().length;f.emitPlay('Paused');assert.equal(clocks().length-n,1);assert.equal(clocks().at(-1).playing,false);
  const p=f.diagnostics().positionMs;f.advance(2000);assert.equal(f.diagnostics().positionMs,p);
  n=clocks().length;f.emitPlay('Playing');assert.equal(clocks().length-n,1);assert.equal(clocks().at(-1).playing,true);
  f.seek(30000,7);f.setSnapshot(30000);n=clocks().length;f.emitPlay('Paused');assert.equal(clocks().length-n,1);assert.equal(clocks().at(-1).seekState,'confirmed');assert.equal(clocks().at(-1).seekRequest,7);assert.equal(f.diagnostics().seekPending,false);
  n=clocks().length;f.emitPlay('Paused');assert.equal(clocks().length-n,1,'unchanged timeline still delivers an explicit play-state event');
 }finally{f.dispose();}
});
test('song changes and local settings avoid redundant requests, clears and style echoes',async()=>{
 const f=fixture();try{await f.open();
  const start=f.sent.length;f.setInfo({...f.info(),trackId:'local-b',ncmId:456});f.emitTimeline(0);
  const changed=f.sent.slice(start);assert.equal(changed.filter(m=>m.type==='track').length,1);
  assert.equal(changed.filter(m=>m.type==='lyrics').length,0,'track already clears native lyrics; no second empty reset');
  await settle();assert.equal(f.sent.filter(m=>m.type==='lyrics').at(-1).songId,'local-b');
  const styleCount=()=>f.sent.filter(m=>m.type==='style').length;const before=styleCount();
  const stagger=f.configControl('miniLyrics-lyric-stagger');
  stagger.checked=false;stagger.onchange();assert.equal(styleCount(),before+1);assert.equal(f.sent.at(-1).stagger,false);
  f.advance(1000);assert.equal(styleCount(),before+1,'periodic recovery must not resend the event style');
  stagger.onchange();assert.equal(styleCount(),before+1,'identical events are deduplicated too');
  assert.equal(f.lyricRequests.length,2);assert.equal(f.savedConfig.lyricStagger,false);
 }finally{f.dispose();}
});
test('stable song objects skip normalization and new late metadata is sent once',async()=>{
 const f=fixture();try{
  await f.open();
  const tracks=()=>f.sent.filter(m=>m.type==='track'),before=tracks().length;
  const normalize=f.context.MiniLyricsCore.metadata;let calls=0;
  f.context.MiniLyricsCore.metadata=value=>{calls++;return normalize(value);};
  f.emitTimeline(15000);f.emitTimeline(17000);f.advance(1000);
  assert.equal(calls,0);assert.equal(tracks().length,before);
  f.setInfo({...f.info()});f.emitTimeline(18000);
  assert.equal(calls,1);assert.equal(tracks().length,before);
  f.setInfo({...f.info(),duration:95000,cover:{url:'https://example.test/late'}});
  f.emitTimeline(19000);f.emitTimeline(20000);
  assert.equal(calls,2);assert.equal(tracks().length,before+1);
  assert.equal(tracks().at(-1).cover,'https://example.test/late');assert.equal(tracks().at(-1).duration,95000);
 }finally{f.dispose();}
});
test('zero offset reaches the existing host on reconnect even without a valid clock',async()=>{
 const f=fixture({config:{lyricOffset:400}});try{await f.open();
  assert.equal(f.sent.filter(m=>m.type==='clock').at(-1).offset,400);
  const config=f.config();f.closeSocket();f.setSnapshot(null);
  setting(config,'miniLyrics-reset-lyric-offset').onclick();
  assert.equal(setting(config,'miniLyrics-lyric-offset').value,'0');assert.equal(f.savedConfig.lyricOffset,0);
  const before=f.sent.length;await f.open();f.advance(0);await settle();
  const clock=f.sent.slice(before).find(m=>m.type==='clock');assert.equal(clock.offset,0);assert.equal(clock.valid,false);
 }finally{f.dispose();}
});
test('reconnect sends a fresh clock without the old seek request',async()=>{
 const f=fixture();try{await f.open();f.seek(30000,7);
  assert.equal(f.diagnostics().seekRequest,7);
  f.closeSocket();const before=f.sent.length;await f.open();f.advance(0);await settle();
  const clock=f.sent.slice(before).find(m=>m.type==='clock');
  assert.equal(clock.seekRequest,0);assert.equal(clock.seekState,'');
 }finally{f.dispose();}
});
test('lyric setting writes may fail without interrupting live synchronization',async()=>{
 const f=fixture();try{await f.open();const config=f.config(),fail=()=>{throw Error('settings storage failure');};
  f.context.plugin.setConfig=fail;
  const offset=setting(config,'miniLyrics-lyric-offset'),curve=setting(config,'miniLyrics-lyric-curve'),stagger=setting(config,'miniLyrics-lyric-stagger'),wordMode=setting(config,'miniLyrics-lyric-word-mode');
  offset.value='350';assert.doesNotThrow(offset.onchange);assert.equal(f.sent.at(-1).offset,350);
  curve.value='4';assert.doesNotThrow(curve.onchange);assert.equal(f.sent.at(-1).curve,4);
  stagger.checked=false;assert.doesNotThrow(stagger.onchange);assert.equal(f.sent.at(-1).stagger,false);
  wordMode.value='1';assert.doesNotThrow(wordMode.onchange);assert.equal(f.sent.at(-1).wordMode,1);
  wordMode.onchange();assert.equal(f.sent.filter(m=>m.type==='style').length,4);
 }finally{f.dispose();}
});
test('duration-only timeline updates reach the host without rewinding interpolation',()=>{
 const f=fixture();return (async()=>{
  try{
   await f.open();const notify=f.apiEvents.get('timelineUpdate');
   notify({detail:{songId:'local-a',currentTime:10000,totalTime:90000,source:'progress'}});
   f.advance(400);
   const before=f.diagnostics().positionMs;
   notify({detail:{songId:'local-a',currentTime:10000,totalTime:125000,source:'progress'}});
   const lastClock=f.sent.filter(m=>m.type==='clock').at(-1);
   assert.equal(lastClock.duration,125000);
   assert.equal(f.diagnostics().positionMs,before);
  }finally{f.dispose();}
 })();
});
test('resuming after a long pause keeps clock valid until fresh progress arrives and skips redundant pings',async()=>{
 const f=fixture();
 try{
  await f.open();f.emitPlay('Paused');
  f.advance(6000);
  const pingsBefore=f.sent.filter(m=>m.type==='ping').length;
  assert.ok(pingsBefore>=5,'idle paused ticks send keepalive pings');
  f.emitPlay('Playing');
  f.advance(1000);
  assert.equal(f.diagnostics().valid,true,'resuming after long pause does not trip STALE_MS immediately');
  const start=f.sent.length;f.emitTimeline(12000);f.advance(500);
  assert.equal(f.sent.slice(start).some(m=>m.type==='ping'),false,'recent clock suppresses redundant ping');
 }finally{f.dispose();}
});
