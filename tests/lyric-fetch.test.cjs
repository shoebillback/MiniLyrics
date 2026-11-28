const {test}=require('node:test'),assert=require('node:assert/strict');
const {fixture}=require('./bridge-fixture.cjs');
const settle=()=>new Promise(setImmediate);
const timed=text=>({code:200,lrc:{lyric:'[00:01]'+text}});
const requestIds=f=>f.lyricRequests.map(request=>request.id);
const retry=f=>f.configControl('miniLyrics-retry-lyrics');
const close=f=>f.configControl('miniLyrics-close').onclick();
function changeSong(f,id){
 f.setInfo({...f.info(),trackId:'track-'+id,ncmId:id});
 f.emitTimeline(0);
 return settle();
}

test('playlist changes and focus only acquire lyrics while a window session exists',async()=>{
 const replies=[];
 const f=fixture({fetchLyrics:()=>new Promise(resolve=>replies.push(resolve))});
 try{
  assert.equal(retry(f).disabled,true);
  await changeSong(f,456);f.events.get('focus')();f.events.get('visibilitychange')();f.advance(3000);
  assert.equal(f.apiEvents.size,0);assert.deepEqual(requestIds(f),[]);
  await f.open();assert.deepEqual(requestIds(f),['456']);
  close(f);assert.ok(f.lyricRequests[0].signal.aborted);
  replies[0](timed('Discarded'));await settle();
  await changeSong(f,789);f.events.get('focus')();f.events.get('visibilitychange')();f.advance(3000);
  assert.equal(f.apiEvents.size,0);assert.deepEqual(requestIds(f),['456']);
  assert.equal(f.diagnostics().lyricMode,'waiting');
  await changeSong(f,456);await f.open();
  assert.deepEqual(requestIds(f),['456','456']);
  replies[1](timed('Reopened'));await settle();
  assert.equal(f.sent.filter(message=>message.type==='lyrics').at(-1).lines[0].text,'Reopened');
 }finally{f.dispose();}
});

test('the bundled player adapter releases native subscriptions when the window closes',async()=>{
 const f=fixture({actualPlayer:true});
 const subscribers=()=>[...f.nativeEvents.values()].reduce((total,handlers)=>total+handlers.size,0);
 try{
  assert.equal(subscribers(),0);
  await f.open();const active=subscribers();assert.ok(active>0);
  close(f);assert.equal(subscribers(),0);
  f.events.get('focus')();f.advance(3000);assert.equal(subscribers(),0);
  await f.open();assert.equal(subscribers(),active);
  assert.deepEqual(requestIds(f),['123']);
 }finally{f.dispose();}
});

test('transient network and HTTP failures back off while loading and clear diagnostics on recovery',async()=>{
 let calls=0;
 const f=fixture({fetchResponse:()=>{
  if(++calls===1)throw new TypeError('Failed to fetch (CORS)');
  if(calls===2)return {ok:false,status:503,statusText:'Service Unavailable'};
  return {ok:true,json:async()=>timed('Recovered')};
 }});
 try{
  await f.open();
  assert.equal(f.diagnostics().lyricMode,'loading');assert.match(f.diagnostics().lyricError,/Failed to fetch \(CORS\)/);
  assert.equal(f.sent.filter(message=>message.type==='lyrics').at(-1).mode,'loading');
  f.advance(999);await settle();assert.equal(calls,1);
  f.advance(1);await settle();assert.equal(calls,2);assert.match(f.diagnostics().lyricError,/HTTP 503 Service Unavailable/);
  f.advance(2499);await settle();assert.equal(calls,2);assert.equal(f.diagnostics().lyricMode,'loading');
  f.advance(1);await settle();
  assert.equal(calls,3);assert.equal(f.diagnostics().lyricMode,'timed');assert.equal(f.diagnostics().lyricError,'');
  assert.equal(f.sent.filter(message=>message.type==='lyrics'&&message.mode==='error').length,0);
 }finally{f.dispose();}
});

test('exhausted server failures expose the payload code and a manual retry can recover',async()=>{
 let recover=false;
 const f=fixture({fetchLyrics:()=>recover?timed('Recovered'):{code:-460}});
 try{
  await f.open();f.advance(1000);await settle();f.advance(2500);await settle();
  assert.equal(f.diagnostics().lyricMode,'error');assert.match(f.diagnostics().lyricError,/code: -460/);
  f.advance(8000);await settle();assert.deepEqual(requestIds(f),['123','123','123']);
  recover=true;retry(f).onclick();await settle();
  const messages=f.sent.filter(message=>message.type==='lyrics');
  assert.deepEqual(messages.map(message=>message.mode),['loading','error','loading','timed']);
  assert.equal(messages.at(-1).lines[0].text,'Recovered');assert.equal(f.diagnostics().lyricError,'');
 }finally{f.dispose();}
});

test('timeout aborts its request and a late response cannot overtake the next attempt',async()=>{
 const replies=[];
 const f=fixture({fetchLyrics:()=>new Promise(resolve=>replies.push(resolve))});
 try{
  await f.open();f.advance(8000);
  assert.ok(f.lyricRequests[0].signal.aborted);assert.match(f.diagnostics().lyricError,/超时/);
  f.advance(1000);
  replies[0](timed('Stale'));await settle();
  assert.equal(f.diagnostics().lyricMode,'loading');
  replies[1](timed('Current'));await settle();
  assert.equal(f.sent.filter(message=>message.type==='lyrics').at(-1).lines[0].text,'Current');
 }finally{f.dispose();}
});

test('ending a session or changing songs cancels a waiting retry',async()=>{
 for(const stop of [close,f=>f.closeSocket({code:1000,reason:'host-closed'}),f=>f.dispose()]){
  const f=fixture({fetchLyrics:()=>({code:503})});
  try{
   await f.open();assert.equal(f.diagnostics().lyricMode,'loading');
   stop(f);f.advance(4000);await settle();assert.deepEqual(requestIds(f),['123']);
  }finally{f.dispose();}
 }
 const f=fixture({fetchLyrics:id=>id==='123'?{code:503}:timed('B')});
 try{
  await f.open();f.closeSocket();await changeSong(f,456);
  f.advance(4000);await settle();
  assert.deepEqual(requestIds(f),['123','456']);
  assert.equal(f.diagnostics().lyricError,'');assert.equal(f.diagnostics().lyricMode,'timed');
 }finally{f.dispose();}
});

test('manual retry replaces the current backoff and aborts a replaced in-flight attempt',async()=>{
 const replies=[];
 const f=fixture({fetchLyrics:()=>{
  if(!replies.length){replies.push(null);return {code:503};}
  return new Promise(resolve=>replies.push(resolve));
 }});
 try{
  await f.open();const button=retry(f);assert.equal(button.disabled,false);
  button.onclick();button.onclick();assert.ok(f.lyricRequests[1].signal.aborted);
  replies[1](timed('Stale'));replies[2](timed('Manual'));await settle();
  f.advance(4000);await settle();
  assert.deepEqual(requestIds(f),['123','123','123']);assert.equal(button.disabled,true);
  assert.equal(f.sent.filter(message=>message.type==='lyrics').at(-1).lines[0].text,'Manual');
 }finally{f.dispose();}
});

test('successful A to B to A restores cached lyrics in each successful mode',async()=>{
 const results={'123':timed('A'),'456':{code:200,nolyric:true},'789':{code:200},'999':{code:200,lrc:{lyric:'Untimed'}}};
 const f=fixture({fetchLyrics:id=>results[id]});
 try{
  await f.open();await changeSong(f,456);await changeSong(f,123);await changeSong(f,456);
  assert.deepEqual(requestIds(f),['123','456']);
  await changeSong(f,789);await changeSong(f,999);await changeSong(f,789);await changeSong(f,999);
  assert.deepEqual(requestIds(f),['123','456','789','999']);
  const messages=f.sent.filter(message=>message.type==='lyrics');
  assert.deepEqual(messages.map(message=>message.mode),['timed','instrumental','timed','instrumental','empty','untimed','empty','untimed']);
  assert.equal(messages[2].lines[0].text,'A');
 }finally{f.dispose();}
});

test('the 24-song cache refreshes recently used entries before evicting the least recent',async()=>{
 const f=fixture({fetchLyrics:id=>timed(id)});
 try{
  await f.open();
  for(let id=1001;id<=1023;id++)await changeSong(f,id);
  assert.equal(f.lyricRequests.length,24);
  await changeSong(f,123);assert.equal(f.lyricRequests.length,24);
  await changeSong(f,1024);await changeSong(f,123);assert.equal(f.lyricRequests.length,25);
  await changeSong(f,1001);assert.equal(f.lyricRequests.length,26);
  assert.equal(requestIds(f).at(-1),'1001');
 }finally{f.dispose();}
});

test('HTTP client errors keep their status and do not get retried or cached',async()=>{
 const f=fixture({fetchResponse:id=>id==='123'?{ok:false,status:403,statusText:'Forbidden'}:{ok:true,json:async()=>timed('B')}});
 try{
  await f.open();assert.equal(f.diagnostics().lyricMode,'error');assert.match(f.diagnostics().lyricError,/HTTP 403 Forbidden/);
  f.advance(4000);await settle();assert.deepEqual(requestIds(f),['123']);
  await changeSong(f,456);assert.equal(f.diagnostics().lyricError,'');
  await changeSong(f,123);assert.deepEqual(requestIds(f),['123','456','123']);
 }finally{f.dispose();}
});

test('a -400 response becomes an actionable error and cannot survive as an empty cache entry',async()=>{
 let recover=false;
 const f=fixture({fetchLyrics:id=>id==='123'&&!recover?{code:200,data:-400}:timed(id)});
 try{
  await f.open();f.advance(1000);await settle();f.advance(2500);await settle();
  assert.equal(f.diagnostics().lyricMode,'error');assert.match(f.diagnostics().lyricError,/data: -400/);
  assert.equal(retry(f).disabled,false);
  assert.equal(f.sent.some(message=>message.type==='lyrics'&&message.mode==='empty'),false);
  await changeSong(f,456);recover=true;await changeSong(f,123);
  assert.deepEqual(requestIds(f),['123','123','123','456','123']);
  assert.equal(f.diagnostics().lyricMode,'timed');
 }finally{f.dispose();}
});

test('bundled lyrics can load from Redux before the playback bridge is present',async()=>{
 const f=fixture({actualPlayer:true,noNativeEvents:true});
 try{
  await f.open();await settle();
  assert.deepEqual(requestIds(f),['123']);
  assert.equal(f.diagnostics().lyricMode,'timed');assert.equal(f.diagnostics().playerDiagnostics.canControl,false);
  f.enableNativeEvents();f.advance(1000);
  assert.equal(f.diagnostics().playerDiagnostics.bridgeSource,'legacy');
  f.emitNativeSeek(30);assert.equal(f.diagnostics().positionMs,30000);
  assert.deepEqual(requestIds(f),['123']);
 }finally{f.dispose();}
});

test('downloaded online songs keep online lyrics and imported files do not request them',async()=>{
 const f=fixture({actualPlayer:true,fetchLyrics:id=>timed(id)});
 const select=(id,resourceType,songType=0)=>f.setInfo({...f.info(),trackId:String(id),ncmId:id,resourceType,trackFileType:'local',songType});
 try{
  select(123,'track');await f.open();await settle();
  assert.equal(f.diagnostics().lyricMode,'timed');assert.deepEqual(requestIds(f),['123']);
  select(456,'downloadTrack');f.advance(1000);await settle();
  assert.equal(f.diagnostics().lyricMode,'timed');assert.deepEqual(requestIds(f),['123','456']);
  select(789,'localTrack');f.advance(1000);await settle();
  assert.equal(f.diagnostics().lyricMode,'unsupported');assert.deepEqual(requestIds(f),['123','456']);
 }finally{f.dispose();}
});

test('cloud lyrics use the playing resource and account ID, with native string lyric fields',async()=>{
 const f=fixture({actualPlayer:true,fetchLyrics:()=>({
  lrc:'[00:01.00]Cloud lyric',
  yrc:'[1000,600](1000,600,0)Cloud lyric',
  ytlrc:'[00:01.00]Cloud translation'
 })});
 try{
  f.setInfo({...f.info(),trackId:'9007199254740993',ncmId:456,resourceType:'track',songType:1});
  await f.open();await settle();
  assert.deepEqual(requestIds(f),[]);
  f.setInfo({...f.info(),userId:'101'});
  f.advance(1000);await settle();
  assert.deepEqual(requestIds(f),['9007199254740993']);
  assert.equal(f.lyricRequests[0].url,'https://music.163.com/api/cloud/lyric/get?songId=9007199254740993&userId=101&lv=-1&kv=-1');
  assert.equal(f.diagnostics().lyricTransport,'cloud');
  const [line]=f.sent.filter(message=>message.type==='lyrics').at(-1).lines;
  assert.equal(line.text,'Cloud lyric');
  assert.equal(line.words[0].text,'Cloud lyric');
  assert.equal(line.translation,'Cloud translation');
  assert.equal(f.sent.some(message=>message.type==='track'&&Object.hasOwn(message,'lyricUserId')),false);
 }finally{f.dispose();}
});

test('cloud lyrics retry a network failure and cache results separately by account and route',async()=>{
 let calls=0;
 const f=fixture({actualPlayer:true,fetchLyrics:(id,signal,url)=>{
  const account=new URL(url).searchParams.get('userId');
  if(++calls===1)throw new Error('temporary network failure');
  return timed(account?'Cloud account '+account:'Online catalog');
 }});
 try{
  f.setInfo({...f.info(),trackId:'123',resourceType:'cloudTrack',userId:'101'});
  await f.open();await settle();
  assert.equal(f.diagnostics().lyricMode,'loading');
  f.advance(1000);await settle();
  assert.equal(f.diagnostics().lyricTransport,'cloud');
  const cloudInfo=f.info();
  f.setInfo({...cloudInfo,resourceType:'track'});
  f.advance(1000);await settle();
  assert.equal(f.diagnostics().lyricTransport,'direct');
  assert.equal(calls,3);
  f.setInfo({...cloudInfo,userId:'202'});
  f.advance(1000);await settle();
  assert.equal(f.sent.filter(message=>message.type==='lyrics').at(-1).lines[0].text,'Cloud account 202');
  assert.equal(calls,4);
  f.setInfo(cloudInfo);
  f.advance(1000);await settle();
  assert.equal(f.diagnostics().lyricTransport,'memory');
  assert.equal(f.sent.filter(message=>message.type==='lyrics').at(-1).lines[0].text,'Cloud account 101');
  assert.equal(calls,4);
 }finally{f.dispose();}
});

test('switching away aborts cloud requests and rejects late responses',async()=>{
 let finishCloud;
 const f=fixture({actualPlayer:true,fetchLyrics:id=>id==='999'?new Promise(resolve=>{finishCloud=resolve;}):timed('Online')});
 try{
  f.setInfo({...f.info(),trackId:'999',ncmId:456,resourceIsCloudSong:true,userId:'101'});
  await f.open();await settle();
  assert.deepEqual(requestIds(f),['999']);
  f.setInfo({...f.info(),trackId:'123',ncmId:123,resourceIsCloudSong:false});
  f.advance(1000);await settle();
  assert.equal(f.lyricRequests[0].signal.aborted,true);
  finishCloud(timed('Stale cloud'));await settle();
  assert.equal(f.sent.filter(message=>message.type==='lyrics').at(-1).lines[0].text,'Online');
 }finally{f.dispose();}
});

test('a successful cloud response with an empty lyric string does not retry',async()=>{
 const f=fixture({actualPlayer:true,fetchLyrics:()=>({lrc:''})});
 try{
  f.setInfo({...f.info(),trackId:'999',resourceType:'cloudTrack',userId:'101'});
  await f.open();await settle();
  f.advance(4000);await settle();
  assert.deepEqual(requestIds(f),['999']);
  assert.equal(f.diagnostics().lyricMode,'empty');
 }finally{f.dispose();}
});

test('direct errors and empty responses can recover using the client cache for the same song',async()=>{
 for(const response of [{code:200},{code:200,data:-400}]){
  const lookups=[];
  const f=fixture({actualPlayer:true,fetchLyrics:()=>response,clientLyricCache:id=>{
   lookups.push(id);return Promise.resolve({url:id,lyric:timed('From desktop')});
  }});
  try{
   await f.open();await settle();f.advance(4000);await settle();
   assert.deepEqual(lookups,['123']);assert.deepEqual(requestIds(f),['123']);
   assert.equal(f.diagnostics().lyricError,'');assert.equal(f.diagnostics().lyricTransport,'client-cache');
   assert.equal(f.sent.filter(message=>message.type==='lyrics').at(-1).lines[0].text,'From desktop');
  }finally{f.dispose();}
 }
});

test('a mismatched cache result cannot override the next direct attempt',async()=>{
 let calls=0;
 const f=fixture({actualPlayer:true,fetchLyrics:()=>++calls===1?{code:503}:timed('Online'),
  clientLyricCache:()=>({url:'456',lyric:timed('Another song')})});
 try{
  await f.open();await settle();assert.equal(f.diagnostics().lyricMode,'loading');
  f.advance(1000);await settle();
  assert.equal(f.diagnostics().lyricTransport,'direct');
  assert.equal(f.sent.filter(message=>message.type==='lyrics').at(-1).lines[0].text,'Online');
 }finally{f.dispose();}
});

test('client cache lookup is bounded and its late results cannot replace current lyrics',async()=>{
 for(const switchSong of [false,true]){
  let resolveCache,calls=0;
  const f=fixture({actualPlayer:true,fetchLyrics:id=>++calls===1?{code:503}:timed(id),
   clientLyricCache:()=>new Promise(resolve=>{resolveCache=resolve;})});
  try{
   await f.open();await settle();
   if(switchSong){f.setInfo({...f.info(),trackId:'new-track',ncmId:456});f.advance(1000);await settle();}
   else{f.advance(1500);await settle();f.advance(1000);await settle();}
   assert.equal(f.diagnostics().lyricTransport,'direct');
   resolveCache({url:'123',lyric:timed('Late cache')});await settle();
   assert.equal(f.sent.filter(message=>message.type==='lyrics').at(-1).lines[0].text,switchSong?'456':'123');
  }finally{f.dispose();}
 }
});
