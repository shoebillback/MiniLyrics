const {test}=require('node:test'),assert=require('node:assert/strict');
const C=require('../core.js'),{fixture}=require('./bridge-fixture.cjs');
const {parseLyrics}=require('../lyrics.js');
test('word text is retained without inventing an interlude; explicit markers win',()=>{
 const [line,marked]=C.normalizeLines([{time:5000,words:[{text:'hello',time:5000}]},{time:6000,isInterlude:true,words:[{text:'gap',time:6000}]}]);
 assert.equal(line.text,'hello');assert.equal(line.isInterlude,false);assert.equal(marked.isInterlude,true);
 assert.equal(C.normalizeLines([{text:'',translation:'translation only'}])[0].isInterlude,false);
});
test('absolute word timestamps are never guessed from proximity to the line',()=>{
 assert.equal(C.normalizeLines([{time:10000,words:[{text:'early',time:8000}]}])[0].words[0].time,8000);
});
test('YRC uses absolute word time, keeps timed gaps and matches available translations',()=>{
 const result=parseLyrics({code:200,lrc:{lyric:'[00:08.00]fallback'},
  yrc:{lyric:'{"t":0,"c":[{"tx":"Credits"}]}\n[8000,1200](7990,250,0)Hello (8250,900,0)world\n[9200,5800]\n[15000,1000](15000,1000,0)Next'},
  ytlrc:{lyric:'[00:08.15]你好'},tlyric:{lyric:'[00:15.00]下一句'}});
 assert.equal(result.mode,'timed');assert.equal(result.lines.length,3);
 assert.deepEqual(result.lines[0].words,[{text:'Hello ',time:7990,duration:250},{text:'world',time:8250,duration:900}]);
 assert.equal(result.lines[0].translation,'你好');assert.equal(result.lines[2].translation,'下一句');
 assert.equal(result.lines[1].isInterlude,true);assert.equal(result.lines[1].duration,5800);
});
test('LRC fallback handles repeated timestamps, file offset and literal punctuation',()=>{
 const {mode,lines}=parseLyrics({code:200,yrc:{lyric:'[10100,900]bad word timing'},lrc:{lyric:'[ti:Title]\n[00:10.1][00:20.250]。。。\n[offset:+100]'}});
 assert.equal(mode,'timed');assert.deepEqual(lines.map(line=>[line.time,line.text,line.isInterlude]),[[10000,'。。。',false],[20150,'。。。',false]]);
});
test('bad YRC rows fall back individually and valid YRC rows keep their word timing',()=>{
 const tooMany=Array.from({length:501},(_,i)=>`(${15000+i},1,0)字`).join('');
 const result=parseLyrics({code:200,lrc:{lyric:'[00:10]First\n[00:13]Fallback\n[00:15]Word limit\n[00:18]Last'},
  yrc:{lyric:`{"c":[{"tx":"Credits"}],"t":0}\n[10030,500]  (10030,500,0)First!\ninvalid row\n[13000,500]broken\n[15000,501]${tooMany}\n[18000,500](18000,500,0)Last`}});
 assert.deepEqual(result.lines.map(line=>[line.time,line.text,line.words.length]),[[10030,'First!',1],[13000,'Fallback',0],[15000,'Word limit',0],[18000,'Last',1]]);
});
test('partial YRC replaces only corresponding LRC lines, even with a timing or punctuation difference',()=>{
 const result=parseLyrics({code:200,lrc:{lyric:'[00:10]First\n[00:14]Second\n[00:18]Third'},
  yrc:{lyric:'[10200,500](10200,500,0)First!'}});
 assert.deepEqual(result.lines.map(line=>[line.text,line.words.length]),[['First!',1],['Second',0],['Third',0]]);
});
test('YRC replaces a near spelling variant and keeps distinct adjacent and earlier lyrics',()=>{
 const result=parseLyrics({code:200,lrc:{lyric:'[00:00]作词 : Example\n[00:05]A real intro lyric\n[00:10]Colour\n[00:10.180]再见'},
  yrc:{lyric:'{"t":0,"c":[{"tx":"Credits"}]}\n[10020,70](10020,70,0)Color\n[10100,80](10100,80,0)走吧'}});
 assert.deepEqual(result.lines.map(line=>[line.time,line.text,line.words.length]),
  [[5000,'A real intro lyric',0],[10020,'Color',1],[10100,'走吧',1],[10180,'再见',0]]);
});
test('nearby translated rows stay in order and both remain available',()=>{
 const result=parseLyrics({code:200,lrc:{lyric:'[00:10.000]First\n[00:10.300]Second'},
  tlyric:{lyric:'[00:10.200]第一句\n[00:10.350]第二句'}});
 assert.deepEqual(result.lines.map(line=>line.translation),['第一句','第二句']);
});
test('a wrong exact anchor does not take a translation from an earlier line',()=>{
 const result=parseLyrics({code:200,lrc:{lyric:'[00:10.000]First\n[00:10.300]Second'},
  tlyric:{lyric:'[00:10.300]第一句\n[00:10.500]第二句'}});
 assert.deepEqual(result.lines.map(line=>line.translation),['第一句','第二句']);
});
test('a song-sized dense cluster keeps translated rows across alignment windows',()=>{
 const rows=(offset,label)=>Array.from({length:5000},(_,i)=>{
  const time=i*300+offset,seconds=Math.floor(time/1000);
  return '['+Math.floor(seconds/60)+':'+seconds%60+'.'+String(time%1000).padStart(3,'0')+']'+label+i;
 }).join('\n');
 const result=parseLyrics({code:200,lrc:{lyric:rows(0,'Original ')},tlyric:{lyric:rows(300,'Translation ')}});
 assert.equal(result.lines.length,5000);
 assert.ok(result.lines.every((line,i)=>line.translation==='Translation '+i));
});
test('lower-priority translations for already matched rows do not spill into neighbouring rows',()=>{
 const result=parseLyrics({code:200,yrc:{lyric:'[10000,400](10000,400,0)First\n[10400,400](10400,400,0)Second\n[12000,400](12000,400,0)Third'},
  ytlrc:{lyric:'[00:10.00]优先译文'},tlyric:{lyric:'[00:09.80]无对应译文\n[00:10.00]重复译文\n[00:12.00]第三句'}});
 assert.deepEqual(result.lines.map(line=>line.translation),['优先译文','','第三句']);
});
test('instrumental flag and variable-length LRC fractions work without special lyric text',()=>{
 assert.deepEqual(parseLyrics({code:200,nolyric:true}),{mode:'instrumental',lines:[]});
 const result=parseLyrics({code:200,lrc:{lyric:'[00:12.3456]First\n[00:15.4]Second'}});
 assert.deepEqual(result.lines.map(line=>line.time),[12345,15400]);
});
test('only sustained phrase-ending words get the long note effect',()=>{
 const result=parseLyrics({code:200,yrc:{lyric:'[1000,5000](1000,1500,0)First (2500,1100,0)end(3600,50,0)，(3650,800,0)短(4450,1500,0)音\n[9000,350](9000,350,0)短\n[10000,500](10000,500,0)轻拖音'}});
 assert.deepEqual(result.lines[0].words.filter(word=>word.longNote).map(word=>word.text),['end','音']);
 assert.deepEqual(result.lines[0].words.filter(word=>word.lineTail).map(word=>word.text),['音']);
 assert.deepEqual(result.lines[0].words.filter(word=>word.emphasize).map(word=>word.text),['First ','end']);
 assert.equal(result.lines[1].words[0].longNote,undefined);
 assert.equal(result.lines[2].words[0].longNote,true);
 assert.equal(result.lines[2].words[0].lineTail,true);
 const trailPunc=parseLyrics({code:200,yrc:{lyric:'[1000,2050](1000,2000,0)Hold(3000,50,0)。\n[4650,1000](4650,1000,0)Next'}});
 assert.equal(trailPunc.lines[0].words[0].longNote,true);
 assert.equal(trailPunc.lines[0].words[0].lineTail,true);
 assert.equal(trailPunc.lines[0].words[0].emphasize,true);
 assert.equal(trailPunc.lines[0].words[1].longNote,undefined);
 assert.equal(trailPunc.lines[0].words[1].lineTail,undefined);
 const midLong=parseLyrics({code:200,yrc:{lyric:'[1000,2200](1000,1500,0)First,(2500,200,0)last'}});
 assert.equal(midLong.lines[0].words[0].longNote,true);
 assert.equal(midLong.lines[0].words[0].emphasize,true);
 assert.equal(midLong.lines[0].words[0].lineTail,undefined);
 const normalized=C.normalizeLines(trailPunc.lines);
 assert.equal(normalized[0].words[0].lineTail,true);
 assert.equal(normalized[0].words[0].emphasize,true);
 assert.equal(normalized[0].words[1].lineTail,undefined);
});
test('YRC emphasis counts Unicode code points after removing all whitespace',()=>{
 const words=['𠀀','𠀀𠀁𠀂𠀃','A   B   C','12345678',' a b '];
 const yrc=words.map((word,i)=>`[${i*1500},1200](${i*1500},1200,0)${word}`).join('\n');
 const {lines}=parseLyrics({code:200,yrc:{lyric:yrc}});
 assert.deepEqual(lines.map(line=>line.words[0].emphasize===true),[false,true,true,false,true]);
});
test('YRC adds an interlude at the end of a vocal line with a four-second gap',()=>{
 const {lines}=parseLyrics({code:200,yrc:{lyric:'[5000,2000](5000,2000,0)First\n[11000,2000](11000,2000,0)Second\n[16999,100](16999,100,0)Last'}});
 assert.deepEqual(lines.map(({time,duration,text,isInterlude})=>[time,duration,text,isInterlude]),[
  [5000,2000,'First',false],[7000,4000,'',true],[11000,2000,'Second',false],[16999,100,'Last',false]
 ]);
});
test('YRC waits for the last word to finish before inserting a four-second interlude',()=>{
 const shortGap=parseLyrics({code:200,yrc:{lyric:'[0,2000](0,1000,0)Hi (1000,1500,0)there\n[6000,1000](6000,1000,0)Next'}});
 assert.equal(shortGap.lines.some(line=>line.isInterlude),false);
 const validGap=parseLyrics({code:200,yrc:{lyric:'[0,2000](0,1000,0)Hi (1000,1500,0)there\n[6500,1000](6500,1000,0)Next'}});
 assert.deepEqual(validGap.lines.map(({time,duration,text,isInterlude})=>[time,duration,text,isInterlude]),[
  [0,2000,'Hi there',false],[2500,4000,'',true],[6500,1000,'Next',false]
 ]);
});
test('explicit gaps stay single and only YRC with an explicit positive duration starts an inferred gap',()=>{
 const explicit=parseLyrics({code:200,yrc:{lyric:'[1000,2000](1000,2000,0)First\n[3000,10000]\n[13000,1000](13000,1000,0)Last'}});
 assert.deepEqual(explicit.lines.filter(line=>line.isInterlude).map(line=>[line.time,line.duration]),[[3000,10000]]);
 const lrc=parseLyrics({code:200,lrc:{lyric:'[00:01]First\n[00:12]Last'}});
 assert.equal(lrc.lines.length,2);assert.ok(lrc.lines.every(line=>!line.isInterlude));
 const mixed=parseLyrics({code:200,yrc:{lyric:'[1000,0](1000,100,0)First\n[25000,1000](25000,1000,0)Last'},
  lrc:{lyric:'[00:12]LRC only'}});
 assert.deepEqual(mixed.lines.map(line=>line.text),['First','LRC only','Last']);assert.ok(mixed.lines.every(line=>!line.isInterlude));
});
test('same-track metadata and reconnect retain lyric mode and content',async()=>{
 const f=fixture({fetchLyrics:()=>({code:200,lrc:{lyric:'read me\nsecond line'}})});try{await f.open();const count=f.sent.filter(m=>m.type==='lyrics').length;
 f.setInfo({...f.info(),duration:95000,cover:{url:'https://example.test/late-cover'}});f.emitTimeline(17000);assert.equal(f.sent.filter(m=>m.type==='lyrics').length,count);assert.equal(f.diagnostics().lyricMode,'untimed');assert.equal(f.diagnostics().positionMs,17000);
 const beforeReconnect=f.sent.length;f.closeSocket();await f.open();f.advance(0);await new Promise(setImmediate);
 const m=f.sent.slice(beforeReconnect).find(m=>m.type==='lyrics');assert.equal(m.mode,'untimed');assert.equal(m.lines[0].text,'read me');
 assert.equal(f.lyricRequests.length,1);
 }finally{f.dispose();}
});
test('an empty lyric response received offline is replayed on reconnect',async()=>{
 let resolve;const f=fixture({fetchLyrics:()=>new Promise(done=>{resolve=done;})});
 try{await f.open();f.closeSocket();const before=f.sent.length;
  resolve({code:200,uncollected:true});await new Promise(setImmediate);
  assert.equal(f.diagnostics().lyricMode,'empty');
  assert.equal(f.sent.length,before);
  await f.open();f.advance(0);await new Promise(setImmediate);
  const lyrics=f.sent.slice(before).find(m=>m.type==='lyrics');
  assert.equal(lyrics.mode,'empty');assert.deepEqual(lyrics.lines,[]);assert.equal(f.lyricRequests.length,1);
 }finally{f.dispose();}
});
test('empty and instrumental replies remain distinct',()=>{
 assert.equal(parseLyrics({code:200,uncollected:true}).mode,'empty');
 assert.equal(parseLyrics({code:200,uncollected:true,lrc:{lyric:'[00:00.00]暂无歌词'}}).mode,'empty');
 assert.equal(parseLyrics({code:200,lrc:{lyric:'[00:00.00]纯音乐，请欣赏'}}).mode,'instrumental');
});
test('lyrics-ack rejects stale songs, revisions and failed acks without losing confirmation on background updates',async()=>{
 const f=fixture();
 try{
  await f.open();const status=f.configControl('miniLyrics-status');
  const first=f.sent.filter(m=>m.type==='lyrics').at(-1);
  f.setInfo({...f.info(),trackId:'local-b',ncmId:456});f.emitTimeline(0);await new Promise(setImmediate);
  const current=f.sent.filter(m=>m.type==='lyrics').at(-1);
  f.hostMessage({type:'lyrics-ack',accepted:true,songId:'local-a',revision:first.revision,lyricCount:99});
  assert.match(status.textContent,/小窗确认：待回报/);
  f.hostMessage({type:'lyrics-ack',accepted:false,songId:'local-b',revision:current.revision,lyricCount:2});
  assert.match(status.textContent,/小窗确认：待回报/);
  f.hostMessage({type:'lyrics-ack',accepted:true,songId:'local-b',revision:current.revision-1,lyricCount:88});
  assert.match(status.textContent,/小窗确认：待回报/);
  f.hostMessage({type:'lyrics-ack',accepted:true,songId:'local-b',revision:current.revision,lyricCount:2});
  assert.match(status.textContent,/小窗确认：2$/);
  f.hostMessage({type:'background-state',background:{mode:1}});
  assert.match(status.textContent,/小窗确认：2$/);
 }finally{f.dispose();}
});
