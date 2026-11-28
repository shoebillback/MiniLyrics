const {test}=require('node:test');
const assert=require('node:assert/strict');
const C=require('../core.js');
test('normalize absolute word timestamps with invalid numbers',()=>{
 const [line]=C.normalizeLines([{time:10000,words:[{text:'a',time:10100,duration:200},{text:'b',time:500,duration:NaN}]}]);
 assert.equal(line.words[0].time,10100);assert.equal(line.words[1].time,500);
 assert.equal(line.words[1].duration,1);assert.equal(C.finite(Infinity),0);
});
test('interlude markers and durations survive normalization without replacing literal punctuation',()=>{
 const lines=C.normalizeLines([{time:0,duration:6000,text:'',isInterlude:true},
  {time:6000,text:'。。。'}, {time:9000,text:'…',isInterlude:true},
  {time:12000,text:'',words:[{text:'word',time:12000,duration:500}],translation:'translation'}]);
 assert.equal(lines[0].isInterlude,true);assert.equal(lines[0].text,'');assert.equal(lines[0].duration,6000);
 assert.equal(lines[1].isInterlude,false);assert.equal(lines[1].text,'。。。');
 assert.equal(lines[2].isInterlude,true);assert.equal(lines[3].isInterlude,false);
 assert.equal(lines[3].words[0].text,'word');assert.equal(lines[3].translation,'translation');
});
test('Player metadata keeps local identity, online alias, precision and artwork',()=>{
 const m=C.metadata({trackId:'local-id',ncmId:123,songName:'Local',authorName:'Artist',cover:{url:'https://example.test/a'},duration:30000});
 assert.equal(m.id,'local-id');assert.equal(m.ncmId,'123');assert.equal(m.lyricId,'123');assert.equal(m.title,'Local');assert.equal(m.duration,30000);assert.equal(m.cover,'https://example.test/a');
 assert.equal(C.metadata({trackId:'local-only',ncmId:0}).ncmId,'');assert.equal(C.metadata(null),null);
 const large=C.metadata({trackId:'9007199254740993',ncmId:'9007199254740993',lyricUserId:'101'});
 assert.equal(large.ncmId,'9007199254740993');assert.equal(large.lyricId,'9007199254740993');assert.equal(large.lyricUserId,'101');
});
test('limits visual node data and tolerates null lyric input',()=>{
 assert.deepEqual(C.normalizeLines(null),[]);
 assert.equal(C.normalizeLines([{words:new Array(600).fill({text:'x',time:0})}])[0].words.length,500);
});
 test('malformed provider entries do not break normalization or metadata',()=>{
 const lines=C.normalizeLines([null,undefined,'bad',{time:0,text:'valid',words:[null,false,{time:0,text:'valid',duration:100}]}]);
 assert.equal(lines.length,1);assert.equal(lines[0].words.length,1);
 assert.equal(C.metadata(null),null);
 assert.equal(C.metadata({trackId:'a',authorName:'Artist'}).artist,'Artist');
});
test('wellFormed replaces lone UTF-16 surrogates while preserving valid emoji pairs',()=>{
 assert.equal(C.wellFormed('hi\uD83D!\uDC00🎵'), 'hi\uFFFD!\uFFFD🎵');
 const [line]=C.normalizeLines([{time:0,text:'bad\uD83Dline 🎵',translation:'tr\uDC00',words:[{text:'w\uD83D',time:0,duration:100}]}]);
 assert.equal(line.text,'bad\uFFFDline 🎵');assert.equal(line.translation,'tr\uFFFD');assert.equal(line.words[0].text,'w\uFFFD');
 const meta=C.metadata({trackId:'id\uD83D',songName:'Song 🎵\uDC00',authorName:'A\uD800'});
 assert.equal(meta.id,'id\uFFFD');assert.equal(meta.title,'Song 🎵\uFFFD');assert.equal(meta.artist,'A\uFFFD');
});
