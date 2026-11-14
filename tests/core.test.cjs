const {test}=require('node:test');
const assert=require('node:assert/strict');
const C=require('../core.js');
test('normalize absolute word timestamps with invalid numbers',()=>{
 const [line]=C.normalizeLines([{time:10000,dynamicLyric:[{word:'a',time:10100,duration:200},{word:'b',time:500,duration:NaN}]}]);
 assert.equal(line.words[0].time,10100);assert.equal(line.words[1].time,500);
 assert.equal(line.words[1].duration,1);assert.equal(C.finite(Infinity),0);
});
test('RNP interlude markers and durations survive normalization without replacing literal punctuation',()=>{
 const lines=C.normalizeLines([{time:0,duration:6000,originalLyric:'',isInterlude:true},
  {time:6000,originalLyric:'。。。'}, {time:9000,text:'…',isInterlude:true},
  {time:12000,originalLyric:'',dynamicLyric:[{word:'word',time:12000,duration:500}],translatedLyric:'translation'}]);
 assert.equal(lines[0].isInterlude,true);assert.equal(lines[0].text,'');assert.equal(lines[0].duration,6000);
 assert.equal(lines[1].isInterlude,false);assert.equal(lines[1].text,'。。。');
 assert.equal(lines[2].isInterlude,true);assert.equal(lines[3].isInterlude,false);
 assert.equal(lines[3].words[0].text,'word');assert.equal(lines[3].translation,'translation');
});
test('Player metadata keeps local identity, online alias and artwork',()=>{
 const m=C.metadata({trackId:'local-id',ncmId:123,songName:'Local',authorName:'Artist',cover:{url:'https://example.test/a'},duration:30000});
 assert.equal(m.id,'local-id');assert.equal(m.ncmId,'123');assert.equal(m.title,'Local');assert.equal(m.duration,30000);assert.equal(m.cover,'https://example.test/a');
 assert.equal(C.metadata({trackId:'local-only',ncmId:0}).ncmId,'');assert.equal(C.metadata(null),null);
});
test('limits visual node data and tolerates null lyric input',()=>{
 assert.deepEqual(C.normalizeLines(null),[]);
 assert.equal(C.normalizeLines([{dynamicLyric:new Array(600).fill({word:'x',time:0})}])[0].words.length,500);
});
test('malformed provider entries do not break normalization or metadata',()=>{
 const lines=C.normalizeLines([null,undefined,'bad',{time:0,text:'valid',dynamicLyric:[null,false,{time:0,word:'valid',duration:100}]}]);
 assert.equal(lines.length,1);assert.equal(lines[0].words.length,1);
 assert.equal(C.metadata(null),null);
 assert.equal(C.metadata({trackId:'a',authorName:'Artist'}).artist,'Artist');
});
