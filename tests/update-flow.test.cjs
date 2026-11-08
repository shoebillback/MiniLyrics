const {test}=require('node:test'),assert=require('node:assert/strict');
const {fixture}=require('./bridge-fixture.cjs');
test('each RNP update is normalized once, including in-place edits and early next-song lyrics',async()=>{
 const f=fixture();try{await f.open();let calls=0;const core=f.context.MiniLyricsCore,normalize=core.normalizeLines;core.normalizeLines=value=>{calls++;return normalize(value);};
  const update={hash:'123-1000',lyrics:[{time:0,originalLyric:'更新歌词'}]};f.emitRnp(update);assert.equal(calls,1);assert.equal(f.sent.at(-1).lines[0].text,'更新歌词');
  calls=0;update.lyrics[0].translatedLyric='Translation';f.emitRnp(update);assert.equal(calls,1);assert.equal(f.sent.at(-1).lines[0].translation,'Translation');
  calls=0;f.emitRnp({hash:'456-1001',lyrics:[{time:0,originalLyric:'下一首'}]});assert.equal(calls,0);f.setInfo({...f.info(),trackId:'local-b',ncmId:456});f.emitTimeline(0);assert.equal(calls,1);assert.equal(f.sent.filter(m=>m.type==='lyrics').at(-1).lines[0].text,'下一首');
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
test('song changes and RNP styles use one message path without redundant clears or echoes',async()=>{
 const storage={};const f=fixture({storage});try{await f.open();
  const start=f.sent.length;f.setInfo({...f.info(),trackId:'local-b',ncmId:456});f.emitTimeline(0);
  const changed=f.sent.slice(start);assert.equal(changed.filter(m=>m.type==='track').length,1);
  assert.equal(changed.filter(m=>m.type==='lyrics').length,0,'track already clears native lyrics; no second empty reset');
  const update={hash:'456-1001',lyrics:[{time:6000,originalLyric:'First'}]};f.emitRnp(update);assert.equal(f.sent.filter(m=>m.type==='lyrics').at(-1).lines[0].text,'First');
  const styleCount=()=>f.sent.filter(m=>m.type==='style').length;const before=styleCount();storage['refined-now-playing-lyric-stagger']='false';
  f.events.get('rnp-lyric-stagger')({detail:false});assert.equal(styleCount(),before+1);assert.equal(f.sent.at(-1).stagger,false);
  f.advance(1000);assert.equal(styleCount(),before+1,'periodic recovery must not resend the event style');
  f.events.get('rnp-lyric-stagger')({detail:false});assert.equal(styleCount(),before+1,'identical events are deduplicated too');
 }finally{f.dispose();}
});
