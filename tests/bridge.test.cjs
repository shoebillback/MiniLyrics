const {test}=require('node:test'),assert=require('node:assert/strict');
const {fixture}=require('./bridge-fixture.cjs');
async function withFixture(fn,options){const f=fixture(options);await f.open();try{await fn(f);}finally{f.dispose();}}
test('player metadata and direct NetEase lyrics work without any other plugin',()=>withFixture(f=>{
 const track=f.sent.filter(m=>m.type==='track').at(-1);assert.equal(track.id,'local-a');assert.equal(track.title,'A');assert.equal(track.artist,'Artist');assert.equal(track.cover,'https://example.test/a');
 assert.equal(f.diagnostics().lyricCount,2);assert.equal(f.diagnostics().lyricSource,'网易云歌词');assert.equal(f.seeks.length,0);
 assert.equal(f.lyricRequests.length,1);assert.equal(f.lyricRequests[0].id,'123');assert.equal(new URL(f.lyricRequests[0].url).hostname,'music.163.com');
 const request=new URL(f.lyricRequests[0].url);assert.equal(request.pathname,'/api/song/lyric/v1');
 assert.deepEqual(Object.fromEntries(request.searchParams),{tv:'0',lv:'0',rv:'0',kv:'0',yv:'0',ytv:'0',yrv:'0',cp:'false',id:'123'});
}));
test('tracks without an eligible online ID do not issue a request',()=>withFixture(f=>{
 f.setInfo({...f.info(),trackId:'123456',lyricId:''});f.emitTimeline(0);
 assert.equal(f.lyricRequests.length,1);
 assert.equal(f.sent.filter(m=>m.type==='lyrics').at(-1).mode,'unsupported');
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
test('player adapter can load after the widget starts',()=>withFixture(async f=>{
 assert.equal(f.diagnostics().valid,false);f.context.testPlayer=f.api;f.advance(1000);await new Promise(setImmediate);assert.equal(f.diagnostics().songId,'local-a');assert.equal(f.diagnostics().lyricCount,2);
},{noApi:true}));
test('unloading player adapter freezes time and removes event listeners',()=>withFixture(f=>{
 delete f.context.testPlayer;f.advance(1000);assert.equal(f.diagnostics().valid,false);assert.equal(f.apiEvents.size,0);
}));
test('connection retries preserve the existing host and port',()=>withFixture(f=>{
 assert.equal(f.launches.length,1);const ports=f.launches.map(s=>Number(s.match(/--port (\d+)/)[1]));assert.ok(ports.every(p=>p>=42000&&p<48000));
},{failConnections:15}));

test('MiniLyrics curve, stagger and offset settings are forwarded on connection',()=>withFixture(f=>{
 const style=f.sent.filter(m=>m.type==='style').at(-1);assert.equal(style.curve,3);assert.equal(style.stagger,false);assert.equal(style.wordMode,0);
 assert.equal(f.sent.filter(m=>m.type==='clock').at(-1).offset,350);
},{config:{lyricCurve:3,lyricStagger:false,lyricOffset:350}}));

test('MiniLyrics spring curve and sweep wordMode settings are stored and forwarded',()=>withFixture(f=>{
 const style=f.sent.filter(m=>m.type==='style').at(-1);assert.equal(style.curve,4);assert.equal(style.wordMode,1);
 assert.equal(f.diagnostics().curve,4);assert.equal(f.diagnostics().wordMode,1);
},{config:{lyricCurve:4,lyricWordMode:1}}));

test('invalid persisted style settings fall back to safe defaults',()=>withFixture(f=>{
 const style=f.sent.filter(m=>m.type==='style').at(-1);
 assert.equal(style.curve,0);assert.equal(style.stagger,true);assert.equal(style.wordMode,0);
 assert.equal(f.diagnostics().curve,0);assert.equal(f.diagnostics().stagger,true);assert.equal(f.diagnostics().wordMode,0);
},{config:{lyricCurve:true,lyricStagger:'false',lyricWordMode:true}}));

test('out-of-range and string persisted style settings fall back to defaults',()=>withFixture(f=>{
 const style=f.sent.filter(m=>m.type==='style').at(-1);
 assert.equal(style.curve,0);assert.equal(style.wordMode,0);
},{config:{lyricCurve:99,lyricWordMode:'1'}}));
