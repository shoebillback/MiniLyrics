const {test}=require('node:test'),assert=require('node:assert/strict');
const C=require('../core.js'),{fixture}=require('./bridge-fixture.cjs');
test('word text is retained without inventing an interlude; explicit markers win',()=>{
 const [line,marked]=C.normalizeLines([{time:5000,dynamicLyric:[{word:'hello',time:5000}]},{time:6000,isInterlude:true,dynamicLyric:[{word:'gap',time:6000}]}]);
 assert.equal(line.text,'hello');assert.equal(line.isInterlude,false);assert.equal(marked.isInterlude,true);
 assert.equal(C.normalizeLines([{text:'',translation:'translation only'}])[0].isInterlude,false);
});
test('RNP absolute word timestamps are never guessed from proximity to the line',()=>{
 assert.equal(C.normalizeLines([{time:10000,dynamicLyric:[{word:'early',time:8000}]}])[0].words[0].time,8000);
 assert.equal(C.normalizeLines([{time:10000,dynamicLyric:[{word:'relative',time:500}]}],{wordTiming:'relative'})[0].words[0].time,10500);
});
test('untimed mode survives transport, changes deduplication, and retains input order',async()=>{
 const f=fixture();try{await f.open();const d={songId:'123',unsynced:true,lyrics:[{time:9000,text:'first'},{time:1000,text:'second'}]};f.emitRnp(d);
 let m=f.sent.filter(m=>m.type==='lyrics').at(-1);assert.equal(m.mode,'untimed');assert.equal(m.lines[0].text,'first');
 d.unsynced=false;f.emitRnp(d);m=f.sent.filter(m=>m.type==='lyrics').at(-1);assert.equal(m.mode,'timed');assert.equal(m.lines[0].text,'second');
 d.lyrics[0].unsynced=true;f.emitRnp(d);assert.equal(f.sent.filter(m=>m.type==='lyrics').at(-1).mode,'untimed');
 }finally{f.dispose();}
});
test('first empty result is sent after a track change, late lyrics can replace it',async()=>{
 const f=fixture();try{await f.open();f.setInfo({...f.info(),trackId:'b',ncmId:456});f.emitTimeline(0);const count=f.sent.filter(m=>m.type==='lyrics').length;
 f.emitRnp({songId:'456',lyrics:[]});let m=f.sent.filter(m=>m.type==='lyrics').at(-1);assert.equal(f.sent.filter(m=>m.type==='lyrics').length,count+1);assert.equal(m.mode,'empty');
 f.emitRnp({songId:'456',lyrics:[{time:0,text:'late'}]});m=f.sent.filter(m=>m.type==='lyrics').at(-1);assert.equal(m.mode,'timed');assert.equal(m.lines[0].text,'late');
 f.emitRnp({songId:'123',lyrics:[]});assert.equal(f.diagnostics().lyricCount,1);
 }finally{f.dispose();}
});

test('same-track metadata and reconnect retain lyric mode and content',async()=>{
 const f=fixture();try{await f.open();f.emitRnp({songId:'123',unsynced:true,lyrics:[{text:'read me'}]});const count=f.sent.filter(m=>m.type==='lyrics').length;
 f.setInfo({...f.info(),duration:95000,cover:{url:'https://example.test/late-cover'}});f.emitTimeline(17000);assert.equal(f.sent.filter(m=>m.type==='lyrics').length,count);assert.equal(f.diagnostics().lyricMode,'untimed');assert.equal(f.diagnostics().positionMs,17000);
 f.closeSocket();await f.open();const m=f.sent.filter(m=>m.type==='lyrics').at(-1);assert.equal(m.mode,'untimed');assert.equal(m.lines[0].text,'read me');
 }finally{f.dispose();}
});
