const {test}=require('node:test'),assert=require('node:assert/strict');
const {fixture}=require('./bridge-fixture.cjs');
async function withFixture(fn,options){const f=fixture(options);await f.open();try{await fn(f);}finally{f.dispose();}}
test('player adapter supplies metadata and RNP supplies the only lyrics; no native, Redux or online access',()=>withFixture(f=>{
 const track=f.sent.filter(m=>m.type==='track').at(-1);assert.equal(track.id,'local-a');assert.equal(track.title,'A');assert.equal(track.artist,'Artist');assert.equal(track.cover,'https://example.test/a');
 assert.equal(f.diagnostics().lyricCount,2);assert.equal(f.diagnostics().lyricSource,'RNP 歌词');assert.equal(f.seeks.length,0);
 f.events.get('rnp-lyric-page-opened')();assert.equal(f.diagnostics().lyricCount,2);
}));
test('new arrays carrying identical RNP lyrics do not resend or restart the row',()=>withFixture(f=>{
 const count=f.sent.filter(m=>m.type==='lyrics').length;f.emitRnp(JSON.parse(JSON.stringify(f.context.currentLyrics)));
 assert.equal(f.sent.filter(m=>m.type==='lyrics').length,count);
 const d=JSON.parse(JSON.stringify(f.context.currentLyrics));d.lyrics[0].translatedLyric='译文';f.emitRnp(d);
 assert.equal(f.sent.filter(m=>m.type==='lyrics').at(-1).lines[0].translation,'译文');
}));
test('RNP interlude markers and duration reach the host and marker changes are not deduplicated',()=>withFixture(f=>{
 const d={hash:'123-999',lyrics:[{time:0,duration:6000,originalLyric:'',isInterlude:true},{time:6000,originalLyric:'next'}]};f.emitRnp(d);
 let line=f.sent.filter(m=>m.type==='lyrics').at(-1).lines[0];assert.equal(line.isInterlude,true);assert.equal(line.duration,6000);assert.equal(line.text,'');
 d.lyrics[1].isInterlude=true;f.emitRnp(d);line=f.sent.filter(m=>m.type==='lyrics').at(-1).lines[1];assert.equal(line.isInterlude,true);
 d.lyrics[1].isInterlude=false;f.emitRnp(d);line=f.sent.filter(m=>m.type==='lyrics').at(-1).lines[1];assert.equal(line.isInterlude,false);
}));
test('local track identity is separate from its online lyric ID',()=>withFixture(f=>{
 f.emitRnp({hash:'0-777',lyrics:[{time:0,text:'wrong'}]});assert.equal(f.diagnostics().lyricCount,2);
 f.emitRnp({hash:'local-a-777',lyrics:[{time:0,text:'local match'}]});assert.equal(f.sent.filter(m=>m.type==='lyrics').at(-1).lines[0].text,'local match');
}));
test('RNP lyrics arriving before the player adapter song notification are retained',()=>withFixture(f=>{
 f.emitRnp({hash:'456-77',lyrics:[{time:0,text:'B'}]});assert.equal(f.diagnostics().songId,'local-a');
 f.setInfo({...f.info(),trackId:'local-b',ncmId:456});f.emitTimeline(0);
 assert.equal(f.diagnostics().songId,'local-b');assert.equal(f.sent.filter(m=>m.type==='lyrics').at(-1).lines[0].text,'B');
 f.emitRnp({hash:'123-88',lyrics:[{time:0,text:'stale'}]});assert.equal(f.sent.filter(m=>m.type==='lyrics').at(-1).lines[0].text,'B');
}));
test('metadata polling detects a song change while the cover-dependent songChange event is delayed',()=>withFixture(f=>{
 f.setInfo({...f.info(),trackId:'local-b',ncmId:456});f.setSnapshot(0);f.advance(1000);
 assert.equal(f.diagnostics().songId,'local-b');assert.equal(f.diagnostics().lyricCount,0);
 f.setInfo({...f.info(),cover:{url:'https://example.test/new'}});f.advance(1000);
 assert.equal(f.sent.filter(m=>m.type==='track').at(-1).cover,'https://example.test/new');
}));
test('temporary missing metadata retains the current song',()=>withFixture(f=>{f.setInfo(null);f.advance(1000);assert.equal(f.diagnostics().songId,'local-a');}));
test('unchanged player adapter snapshots do not rewind interpolation',()=>withFixture(f=>{const p=f.diagnostics().positionMs;f.advance(1000);assert.equal(f.diagnostics().positionMs,p+1000);}));
test('stalled player adapter freezes safely and a new progress sample recovers without native polling',()=>withFixture(f=>{
 f.advance(5000);assert.equal(f.diagnostics().valid,false);const p=f.diagnostics().positionMs;f.advance(1000);assert.equal(f.diagnostics().positionMs,p);
 f.emitTimeline(18000);assert.equal(f.diagnostics().valid,true);assert.equal(f.diagnostics().positionMs,18000);
}));
test('paused snapshots stay fixed and resume correctly',()=>withFixture(f=>{
 f.emitPlay('Paused');const p=f.diagnostics().positionMs;f.advance(12000);assert.equal(f.diagnostics().positionMs,p);assert.equal(f.diagnostics().valid,true);
 f.emitPlay('Playing');f.advance(1000);assert.ok(f.diagnostics().positionMs>p);
}));
test('backward seek is confirmed by the actual timeline',()=>withFixture(f=>{
 f.emitTimeline(60000);f.seek(5000);f.emitTimeline(5100);assert.equal(f.diagnostics().positionMs,5100);assert.equal(f.diagnostics().seekPending,false);
}));
test('external position can supersede a pending widget seek',()=>withFixture(f=>{
 f.seek(60000,1);f.seek(30000,2);f.emitTimeline(20000);
 assert.equal(f.diagnostics().seekPending,false);assert.equal(f.diagnostics().positionMs,20000);
}));
test('unconfirmed seek expires and restores the actual snapshot',()=>withFixture(f=>{
 f.seek(60000);f.setSnapshot(12000);f.advance(2600);assert.equal(f.diagnostics().seekPending,false);assert.ok(f.diagnostics().positionMs>=12000&&f.diagnostics().positionMs<13000);
 assert.equal(f.sent.filter(m=>m.type==='clock'&&m.seekState==='failed').at(-1).seekRequest,1);
}));
test('explicit seek failure clears pending state immediately',()=>withFixture(f=>{
 f.setSeek(()=>false);f.seek(60000);assert.equal(f.diagnostics().seekPending,false);assert.equal(f.diagnostics().positionMs,10000);
}));
test('song change cancels a pending seek and its timeout cannot overwrite the new song',()=>withFixture(f=>{
 f.seek(60000);f.setInfo({...f.info(),trackId:'local-b',ncmId:456});f.emitTimeline(0);f.advance(2600);
 assert.equal(f.diagnostics().songId,'local-b');assert.equal(f.diagnostics().seekPending,false);assert.ok(f.diagnostics().positionMs<5000);
}));
test('playback controls use player adapter exclusively',()=>withFixture(f=>{
 f.command('play');f.emitPlay('Paused');f.command('play');f.command('next');f.command('previous');assert.deepEqual(f.controls,['pause','play','next','previous']);
}));
test('player adapter can load after the widget starts',()=>withFixture(f=>{
 assert.equal(f.diagnostics().valid,false);f.context.testPlayer=f.api;f.advance(1000);assert.equal(f.diagnostics().songId,'local-a');assert.equal(f.diagnostics().lyricCount,2);
},{noApi:true}));
test('unloading player adapter freezes time and removes event listeners',()=>withFixture(f=>{
 delete f.context.testPlayer;f.advance(1000);assert.equal(f.diagnostics().valid,false);assert.equal(f.apiEvents.size,0);
}));
test('connection retries preserve the existing host and port',()=>withFixture(f=>{
 assert.equal(f.launches.length,1);const ports=f.launches.map(s=>Number(s.match(/--port (\d+)/)[1]));assert.ok(ports.every(p=>p>=42000&&p<48000));
},{failConnections:15}));

test('RNP curve and stagger settings are forwarded on connection',()=>withFixture(f=>{
 const style=f.sent.filter(m=>m.type==='style').at(-1);assert.equal(style.curve,3);assert.equal(style.stagger,false);
},{classes:['rnp-lyric-animation-timing-easeout'],storage:{'refined-now-playing-lyric-stagger':'false'}}));
