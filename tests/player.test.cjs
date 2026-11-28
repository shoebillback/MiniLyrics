const {test}=require('node:test'),assert=require('node:assert/strict');
const {createPlayerAdapter}=require('../player.js');
function fixture({late=false,saved=null}={}){
 let state={playing:{resourceTrackId:'local-a',onlineResourceId:123,resourceName:'A',resourceArtists:[{name:'Artist'}],resourceCoverUrl:'cover',curTrack:{duration:0},playingState:1,playId:'test-session',restoreResource:null,isLoadingFirst:false,curPlaying:{resourceId:'local-a',track:{id:123}}}};
 const listeners=new Set(),native=new Map(),timers=new Map(),commands=[];let serial=0;
 const store={getState:()=>state,subscribe:fn=>{listeners.add(fn);return()=>listeners.delete(fn);},dispatch:action=>commands.push(action)};
 const bridge={appendRegisterCall:(name,ns,fn)=>{if(!native.has(name))native.set(name,new Set());native.get(name).add(fn);},removeRegisterCall:(name,ns,fn)=>native.get(name)?.delete(fn)};
 const original=()=>{};bridge._Adapter={get registerCallMap(){return Object.fromEntries(['PlayProgress','Seek','PlayState'].map(name=>['audioplayer.on'+name,[original,...native.get(name)??[]]]));}};
 const require={c:{a:{exports:{a:{app:{_store:store}}}},b:{exports:{Bridge:bridge}},c:{exports:{b:{lastPlaying:{get:()=>saved}}}}}};
 const chunks=[[]];chunks.push=entry=>Object.values(entry[1])[0]({}, {}, require);
 const env={webpackJsonp:late?null:chunks,console,setTimeout:fn=>{timers.set(++serial,fn);return serial;},clearTimeout:id=>timers.delete(id)};
 const api=createPlayerAdapter(env),emit=(name,...args)=>{for(const fn of [...native.get(name)??[]])fn(...args);};
 return {api,env,chunks,native,commands,listeners,timers,emit,state:()=>state,update:patch=>{state={...state,playing:{...state.playing,...patch}};for(const fn of [...listeners])fn();},retry:()=>{const pending=[...timers.values()];timers.clear();pending.forEach(fn=>fn());}};
}
test('adapter starts without InfLink and keeps the song reference until late metadata changes',()=>{
 const f=fixture();try{assert.equal(f.api.ready,true);const first=f.api.getCurrentSong();assert.equal(first.duration,0);
 f.emit('PlayProgress','local-a',3);assert.equal(f.api.getTimeline().currentTime,3000);
 assert.equal(f.api.getCurrentSong(),first);
 f.update({curTrack:{duration:90000},resourceCoverUrl:''});assert.equal(f.api.getTimeline().totalTime,90000);assert.equal(f.api.getCurrentSong().cover.url,'');
 assert.notEqual(f.api.getCurrentSong(),first);
 }finally{f.api.dispose();}
});
test('old track state/progress/seek cannot corrupt the current song; native and Redux enums differ',()=>{
 const f=fixture();try{f.update({resourceTrackId:'local-b',onlineResourceId:456,playingState:2});
 f.emit('PlayState','local-a','end',4);f.emit('PlayProgress','123_old',55);f.emit('Seek','local-a','seek',0,60);
 assert.equal(f.api.getPlaybackStatus(),'Playing');assert.equal(f.api.getTimeline(),null);
 f.emit('PlayProgress','local-b_session',2);assert.equal(f.api.getTimeline().songId,'local-b');
 f.emit('PlayState','456_session','pause',2);assert.equal(f.api.getPlaybackStatus(),'Paused');
 f.emit('PlayState','','play',1);assert.equal(f.api.getPlaybackStatus(),'Paused');
 }finally{f.api.dispose();}
});
test('paused seek emits immediately, failures and outgoing commands never invent progress',()=>{
 const f=fixture();try{const samples=[];f.api.addEventListener('timelineUpdate',e=>samples.push(e.detail));
 f.api.seekTo(50000);assert.equal(samples.length,0);assert.deepEqual(f.commands.at(-1),{type:'playing/setPlayingPosition',payload:{duration:50}});
 f.emit('Seek','local-a','request',1,50);assert.equal(samples.length,0);
 f.emit('Seek','local-a','request',0,50);assert.equal(samples.length,1);assert.equal(samples[0].currentTime,50000);assert.equal(samples[0].seek,true);
 f.api.pause();f.api.play();f.api.next();f.api.previous();assert.deepEqual(f.commands.slice(1).map(a=>a.type),['playing/pause','playing/resume','playingList/jump2Track','playingList/jump2Track']);
 }finally{f.api.dispose();}
});
test('incomplete podcast metadata cannot let the previous track keep updating',()=>{
 const f=fixture();try{
  f.emit('PlayProgress','local-a',1);
  f.update({resourceTrackId:'voice-b',onlineResourceId:456,resourceType:'voice'});
  f.emit('PlayProgress','local-a',70);f.emit('Seek','voice-b','seek',0,50);
  assert.equal(f.api.getTimeline().currentTime,1000);
 }finally{f.api.dispose();}
});
test('only verified online track IDs are exposed to the lyric provider',()=>{
 const f=fixture();try{
  assert.equal(f.api.getCurrentSong().lyricId,'123');
  f.update({resourceTrackId:'456',onlineResourceId:undefined});assert.equal(f.api.getCurrentSong().lyricId,'');
  f.update({resourceType:'track'});assert.equal(f.api.getCurrentSong().lyricId,'456');
  f.update({resourceType:undefined,curTrack:{id:456}});assert.equal(f.api.getCurrentSong().lyricId,'456');
  f.update({resourceType:'localTrack'});assert.equal(f.api.getCurrentSong().lyricId,'');
  f.update({resourceType:'downloadVoice',onlineResourceId:456});assert.equal(f.api.getCurrentSong().lyricId,'');
  f.update({resourceType:undefined});
  f.update({onlineResourceId:456,trackFileType:'local'});assert.equal(f.api.getCurrentSong().lyricId,'');
  f.update({trackFileType:'mp3',resourceIsCloudSong:true});assert.equal(f.api.getCurrentSong().lyricId,'');
 }finally{f.api.dispose();}
});
test('late discovery, disposal and repeated loading preserve other subscribers',()=>{
 const f=fixture({late:true});assert.equal(f.api.ready,false);f.env.webpackJsonp=f.chunks;f.retry();assert.equal(f.api.ready,true);
 const foreign=()=>{};f.native.get('PlayProgress').add(foreign);f.api.dispose();f.api.dispose();
 assert.deepEqual([...f.native.get('PlayProgress')],[foreign]);assert.equal(f.listeners.size,0);assert.equal(f.api.play(),false);
 const next=createPlayerAdapter(f.env);assert.equal(f.native.get('PlayProgress').size,2);next.dispose();assert.equal(f.native.get('PlayProgress').size,1);
 const pending=fixture({late:true});pending.api.dispose();pending.env.webpackJsonp=pending.chunks;pending.retry();assert.equal(pending.native.size,0);
});
test('startup restoration cannot overwrite live progress, a new track, or a disposed adapter',async()=>{
 let resolve;const f=fixture({saved:new Promise(r=>resolve=r)});f.emit('PlayProgress','local-a',12);resolve({trackId:'local-a',current:2});await Promise.resolve();assert.equal(f.api.getTimeline().currentTime,12000);f.api.dispose();
 const g=fixture({saved:Promise.resolve({trackId:'local-a',current:8})});g.update({resourceTrackId:'local-b'});await Promise.resolve();assert.equal(g.api.getTimeline(),null);g.api.dispose();
 const h=fixture({saved:Promise.resolve({trackId:'local-a',current:8})});h.api.dispose();await Promise.resolve();assert.equal(h.api.getTimeline(),null);
});
test('resetting progress also withdraws an established playback inference',()=>{
 const s=require('../core.js').createPlaybackState();s.sample(0,0,'Paused');s.sample(500,500,'Paused');assert.equal(s.sample(1000,1000,'Paused'),true);
 s.resetProgress();assert.equal(s.sample(50000,1100,'Paused',true),false);
});
test('renamed webpack exports, throwing module getters, non-array artists, and require caching',()=>{
 const state={playing:{resourceTrackId:'r-1',onlineResourceId:99,resourceName:'Track',resourceArtists:{bad:true},resourceCoverUrl:'',curTrack:{duration:12000},playingState:1,playId:'p'}};
 const store={getState:()=>state,subscribe:()=>()=>{},dispatch:()=>{}};
 const nativeBridge={appendRegisterCall:()=>{},removeRegisterCall:()=>{},_Adapter:{registerCallMap:{}}};
 const badModule={};Object.defineProperty(badModule,'exports',{get(){throw Error('boom');}});
 const req={c:{
  bad:badModule,
  renamedStore:{exports:{c:{app:{_store:store}}}},
  renamedStorage:{exports:{d:{lastPlaying:{get:async()=>null}}}},
  bridge:{exports:{Bridge:nativeBridge}}
 }};
 const chunks=[[]];let pushes=0;
 chunks.push=entry=>{pushes++;Object.values(entry[1])[0]({}, {}, req);};
 const timers=new Map();let nextId=0;
 const env={webpackJsonp:chunks,console,setTimeout:fn=>{timers.set(++nextId,fn);return nextId;},clearTimeout:id=>timers.delete(id)};
 const api=createPlayerAdapter(env);
 try{
  assert.equal(api.ready,true);assert.equal(api.getCurrentSong().authorName,'');assert.equal(pushes,1);
 }finally{api.dispose();}

 // When webpackRequire is captured before store is ready, retries reuse cached require without pushing new chunks.
 let storeReady=false;
 const delayedReq={c:{
  get s(){return storeReady?{exports:{c:{app:{_store:store}}}}:{exports:{}};},
  bridge:{exports:{Bridge:nativeBridge}}
 }};
 const dChunks=[[]];let dPushes=0;dChunks.push=entry=>{dPushes++;Object.values(entry[1])[0]({}, {}, delayedReq);};
 const dTimers=new Map();let dId=0;
 const dEnv={webpackJsonp:dChunks,console,setTimeout:fn=>{dTimers.set(++dId,fn);return dId;},clearTimeout:id=>dTimers.delete(id)};
 const dApi=createPlayerAdapter(dEnv);
 try{
  assert.equal(dApi.ready,false);assert.equal(dPushes,1);
  for(const fn of [...dTimers.values()]){dTimers.clear();fn();}
  assert.equal(dPushes,1,'retry must reuse cached require instead of pushing another chunk');
  storeReady=true;
  for(const fn of [...dTimers.values()]){dTimers.clear();fn();}
  assert.equal(dApi.ready,true);assert.equal(dPushes,1);
 }finally{dApi.dispose();}
});
test('MiniLyrics callback failures cannot interrupt later shared listeners',()=>{
 const a={registerCallMap:{},registerCallStubs:{}};
 a.queue=event=>a.registerCallStubs[event]??=(...args)=>{const entry=a.registerCallMap[event];if(entry)(Array.isArray(entry)?entry:[entry]).forEach(fn=>fn.apply(globalThis,args));};
 const b={
  appendRegisterCall(name,ns,fn){const ev=ns+'.on'+name;if(!a.registerCallMap[ev])a.registerCallMap[ev]=[];if(!Array.isArray(a.registerCallMap[ev]))a.registerCallMap[ev]=[a.registerCallMap[ev]];a.registerCallMap[ev].push(fn);},
  removeRegisterCall(name,ns,fn){const entry=a.registerCallMap[ns+'.on'+name];if(Array.isArray(entry)){const i=entry.indexOf(fn);if(i>=0)entry.splice(i,1);}}
 };
 b._Adapter=a;
 b.appendRegisterCall('PlayProgress','audioplayer',()=>{});
 let state={playing:{resourceTrackId:'3437177548',onlineResourceId:3437177548,resourceArtists:[],playingState:2}};
 const store={getState:()=>state,subscribe:()=>()=>{},dispatch:()=>{}};
 const req={c:{store:{exports:{a:{app:{_store:store}}}},bridge:{exports:{Bridge:b}}}};
 const chunks=[[]];chunks.push=entry=>Object.values(entry[1])[0]({}, {}, req);
 const api=createPlayerAdapter({webpackJsonp:chunks,console,setTimeout,clearTimeout});
 try{
  const badPlaying={resourceTrackId:'3437177548',onlineResourceId:3437177548,playingState:2};
  Object.defineProperty(badPlaying,'resourceArtists',{get(){throw new TypeError('malformed');}});
  state={playing:badPlaying};
  let later=0;
  b.appendRegisterCall('PlayProgress','audioplayer',()=>later++);
  assert.doesNotThrow(()=>a.queue('audioplayer.onPlayProgress')('3437177548_5D7DZG',195.899,1));
  assert.equal(later,1);
 }finally{api.dispose();}
});
test('string precision is preserved for large online IDs and isSeekSessionReady reflects readiness',()=>{
 const f=fixture();
 try{
  assert.equal(f.api.isSeekSessionReady(),true);
  f.update({resourceTrackId:'cloud-1',onlineResourceId:'9007199254740993'});
  assert.equal(f.api.getCurrentSong().ncmId,'9007199254740993');
  f.emit('PlayProgress','9007199254740993_abc',12);
  assert.equal(f.api.getTimeline().currentTime,12000);
  f.update({playId:null});
  assert.equal(f.api.isSeekSessionReady(),false);
 }finally{f.api.dispose();}
});
