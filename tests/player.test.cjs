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
test('adapter starts without InfLink, updates late metadata, and does not require a cover',()=>{
 const f=fixture();try{assert.equal(f.api.ready,true);assert.equal(f.api.getCurrentSong().duration,0);
 f.emit('PlayProgress','local-a',3);assert.equal(f.api.getTimeline().currentTime,3000);
 f.update({curTrack:{duration:90000},resourceCoverUrl:''});assert.equal(f.api.getTimeline().totalTime,90000);assert.equal(f.api.getCurrentSong().cover.url,'');
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
