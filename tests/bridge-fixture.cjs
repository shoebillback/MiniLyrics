const vm=require('node:vm'),fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
function fixture(options={}){
 let now=100,nextTimer=0,load,configure,lastSocket,connections=0;const savedConfig={...(options.config??{})};
 let info={trackId:'local-a',ncmId:123,songName:'A',authorName:'Artist',cover:{url:'https://example.test/a'},duration:90000};
 let snapshot={currentTime:10000,totalTime:90000},playback=options.coldPlayer?'Paused':'Playing',seekHandler=null;
 const timers=new Map(),events=new Map(),apiEvents=new Map(),sent=[],seeks=[],launches=[],controls=[];
 const api={version:'3.4.0',getCurrentSong:()=>info,getTimeline:()=>snapshot,getPlaybackStatus:()=>playback,
  addEventListener:(name,fn)=>apiEvents.set(name,fn),removeEventListener:name=>apiEvents.delete(name),
  seekTo:ms=>{seeks.push(ms);return seekHandler?.(ms);},
  play:()=>controls.push('play'),pause:()=>controls.push('pause'),next:()=>controls.push('next'),previous:()=>controls.push('previous')};
 const sockets=[];
 class WS{constructor(){lastSocket=this;this.readyState=0;sockets.push(this);if(!options.delaySockets)queueMicrotask(()=>this.connect());}
  connect(){if(this.readyState!==0)return;if(++connections<=(options.failConnections??0)){this.readyState=3;this.onerror?.();}else{this.readyState=1;this.onopen?.();}}
  send(s){sent.push(JSON.parse(s));}close(){this.readyState=3;this.onclose?.();}}
 const nativeEvents=new Map();let nativeInitialized=!options.coldPlayer,nativeInitializations=0,nativeLoading=false,startupTarget=null;
 const nativeBridge={appendRegisterCall:(name,namespace,fn)=>{const key=namespace+'.'+name;const set=nativeEvents.get(key)??new Set();set.add(fn);nativeEvents.set(key,set);},removeRegisterCall:(name,namespace,fn)=>nativeEvents.get(namespace+'.'+name)?.delete(fn)};
 const nativeOriginal=()=>{};nativeBridge._Adapter={get registerCallMap(){return Object.fromEntries(['PlayProgress','Seek','PlayState'].map(name=>{const callbacks=[...(nativeInitialized?[nativeOriginal]:[]),...nativeEvents.get('audioplayer.'+name)??[]];return ['audioplayer.on'+name,callbacks.length?callbacks:undefined];}));}};
 const add=(name,fn)=>events.set(name,fn),remove=name=>events.delete(name);
 const context={console,performance:{now:()=>now},crypto:require('node:crypto').webcrypto,Uint8Array,WebSocket:WS,
  setTimeout:(fn,ms)=>{const id=++nextTimer;timers.set(id,{at:now+ms,fn});return id;},clearTimeout:id=>timers.delete(id),
  plugin:{pluginPath:'C:/test/plugin',onLoad:fn=>load=fn,onConfig:fn=>configure=fn,getConfig:(key,fallback)=>savedConfig[key]??fallback,setConfig:(key,value)=>savedConfig[key]=JSON.parse(JSON.stringify(value))},loadedPlugins:{},
  betterncm:{app:{exec:async command=>{launches.push(command);return options.exec?options.exec(command):true;}}},
  document:{createElement:tag=>({tagName:tag,style:{},children:[],textContent:'',append(...children){this.children.push(...children);}}),addEventListener:add,removeEventListener:remove,getElementById:()=>{throw Error('Redux access removed');},querySelector:()=>{throw Error('DOM player access removed');}},
  channel:{call:options.channelCall??(()=>{throw Error('native API access removed');})},fetch:()=>{throw Error('online lyrics removed');},
  localStorage:{getItem:key=>options.storage?.[key]??(key==='refined-now-playing-lyric-offset'?options.offset??null:null)},addEventListener:add,removeEventListener:remove};
 context.document.body={classList:{contains:name=>(options.classes??[]).includes(name)}};
 if(!options.noNativeEvents)context.legacyNativeCmder=nativeBridge;
 context.window=context;if(!options.noApi)context.testPlayer=api;
 api.dispose=()=>{};Object.defineProperty(api,'ready',{get:()=>context.testPlayer===api});
 context.currentLyrics=options.rnp??{hash:'123-999',lyrics:[{time:0,originalLyric:'first'},{time:12000,originalLyric:'next'}]};
 function completeNativeStartup(){
  if(nativeInitialized)return;
  for(const name of ['PlayProgress','Seek','PlayState'])assert.equal(nativeEvents.get('audioplayer.'+name)?.size??0,0,'native initialization needs an unreserved '+name+' event');
  nativeInitialized=true;nativeLoading=false;nativeInitializations++;
  if(startupTarget>0){seeks.push(startupTarget);seekHandler?.(startupTarget);}
 }
 if(options.actualPlayer){
 const store={getState:()=>({playing:{resourceTrackId:info?.trackId,onlineResourceId:info?.ncmId,resourceName:info?.songName,resourceArtists:[{name:info?.authorName}],resourceCoverUrl:info?.cover?.url,curTrack:{duration:info?.duration},playingState:playback==='Playing'?2:1,playId:nativeInitialized?'loaded-session':null,restoreResource:nativeInitialized?null:{id:info?.trackId,current:startupTarget===null?snapshot.currentTime/1000:startupTarget/1000},isLoadingFirst:nativeLoading,loadingSeekDuration:startupTarget===null?0:startupTarget/1000,curPlaying:info?{resourceId:info.trackId,track:{id:info.ncmId}}:null}}),subscribe:()=>()=>{},dispatch:action=>{
  if(action.type==='playing/setPlayingPosition'){
   const ms=action.payload.duration*1000;if(!nativeInitialized){startupTarget=ms;return;}
   seeks.push(ms);return seekHandler?.(ms);
  }
  if(action.type==='playing/onLoadingSeekUpdate'){startupTarget=action.payload.loadingSeekDuration*1000;return;}
  let type=action.type;if(type==='playing/switchResumeOrPause')type=playback==='Playing'?'playing/pause':'playing/resume';
  if(type==='playing/pause'){controls.push('pause');playback='Paused';}
  else if(type==='playing/resume'){controls.push('play');playback='Playing';if(!nativeInitialized){nativeLoading=true;if(!options.deferStartup)completeNativeStartup();}}
 }};
 const require={c:{store:{exports:{a:{app:{_store:store,_models:[{namespace:'playing',reducers:{'playing/onLoadingSeekUpdate':()=>{}}}]}}}},bridge:{exports:{Bridge:nativeBridge}},storage:{exports:{b:{lastPlaying:{get:async()=>({trackId:info.trackId,current:snapshot.currentTime/1000})}}}}}};
 context.webpackJsonp=[[]];context.webpackJsonp.push=entry=>Object.values(entry[1])[0]({}, {}, require);
 }
 vm.createContext(context);vm.runInContext(fs.readFileSync(path.join(__dirname,'../core.js'),'utf8'),context);if(options.actualPlayer)vm.runInContext(fs.readFileSync(path.join(__dirname,'../player.js'),'utf8'),context);else context.MiniLyricsPlayer={createPlayerAdapter:()=>api};context.MINILYRICS_VERSION=JSON.parse(fs.readFileSync(path.join(__dirname,'../manifest.json'),'utf8')).version;vm.runInContext(fs.readFileSync(path.join(__dirname,options.actualPlayer?'../main.js':'../plugin.js'),'utf8'),context);load();
 function advance(ms){const target=now+ms;let count=0;while(true){const entry=[...timers].filter(([,t])=>t.at<=target).sort((a,b)=>a[1].at-b[1].at)[0];if(!entry)break;if(++count>1000)throw Error('timer loop');now=entry[1].at;timers.delete(entry[0]);entry[1].fn();}now=target;}
 const emitTimeline=(ms,state=playback)=>{snapshot={currentTime:ms,totalTime:90000};playback=state;apiEvents.get('timelineUpdate')?.({detail:snapshot});};
 return {sockets,completeNativeStartup,nativeInitializations:()=>nativeInitializations,emitNativeSeek:(seconds,id='native-1',playId='123',code=0)=>{for(const fn of [...nativeEvents.get('audioplayer.Seek')??[]])fn(playId,id,code,seconds);},nativeEvents,context,api,sent,seeks,launches,controls,events,apiEvents,advance,emitTimeline,savedConfig,config:()=>configure(),closeSocket:()=>lastSocket.close(),hostMessage:m=>lastSocket.onmessage({data:JSON.stringify(m)}),
  info:()=>info,setInfo:value=>info=value,setSnapshot:ms=>snapshot={currentTime:ms,totalTime:90000},
  setSeek:fn=>seekHandler=fn,emitPlay:state=>{playback=state;apiEvents.get('playStateChange')?.({detail:state});},
  emitRnp:d=>{context.currentLyrics=d;events.get('lyrics-updated')?.({detail:d});},
  seek:(ms,id=1,songId='local-a')=>lastSocket.onmessage({data:JSON.stringify({type:'command',action:'seek',position:ms,requestId:id,songId})}),
  command:action=>lastSocket.onmessage({data:JSON.stringify({type:'command',action})}),
  diagnostics:()=>context.__miniLyrics.getDiagnostics(),
  async open(){let done=false;const promise=context.__miniLyrics.open().finally(()=>done=true);for(let i=0;i<60&&!done;i++){await new Promise(setImmediate);if(!done)advance(200);}assert.ok(done);await promise;},
  dispose:()=>context.__miniLyrics.dispose()};
}
module.exports={fixture};
