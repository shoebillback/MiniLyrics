const {createPlayerAdapter}=require('../player.js');
// Registration policy transcribed from the user's live NCM 3.x _Adapter.
// First-registration assertions do not depend on native backend fanout behavior.
const names=['PlayProgress','Seek','PlayState'];
function fixture({warm=false,bootOnSeek=false,mode=0,inspectable=true,bootNotify=true,confirmDuringSeek=false}={}){
 let now=100,dispatchHandler=null;
 const slots=new Map(),commands=[],listeners=new Set(),counts=Object.fromEntries(names.map(n=>[n,0]));
 const adapter={registerCallMap:{},registerCallStubs:{}};
 adapter._registerCallQueue=event=>adapter.registerCallStubs[event]??=(...args)=>{
  const entry=adapter.registerCallMap[event];if(entry)(Array.isArray(entry)?entry:[entry]).forEach(fn=>fn(...args));
 };
 adapter.registerCallAdapter=(event,fn)=>slots.set(event,fn);
 adapter.registerCall=(name,ns,fn,policy)=>{
  const event=`${ns}.on${name}`;
  switch(policy){
   case 1:adapter.registerCallMap[event]=fn;adapter.registerCallAdapter(event,fn);return true;
   case 2:
    if(!adapter.registerCallMap[event]){adapter.registerCallMap[event]=[];adapter.registerCallAdapter(event,adapter._registerCallQueue(event));}
    if(!Array.isArray(adapter.registerCallMap[event])){adapter.registerCallMap[event]=[adapter.registerCallMap[event]];adapter.registerCallAdapter(event,adapter._registerCallQueue(event));}
    adapter.registerCallMap[event].push(fn);return true;
   default:
    if(adapter.registerCallMap[event]){if(policy===0)throw Error(`[native-rpc] There is already a ${event} callback`);return false;}
    adapter.registerCallMap[event]=fn;adapter.registerCallAdapter(event,fn);return true;
  }
 };
 adapter.removeRegisterCall=(name,ns,fn)=>{
  const event=`${ns}.on${name}`,entry=adapter.registerCallMap[event];if(!entry)return false;
  if(fn){if(Array.isArray(entry)){const i=entry.indexOf(fn);if(i>=0)entry.splice(i,1);}else if(entry===fn)adapter.registerCallMap[event]=undefined;}
  else adapter.registerCallMap[event]=undefined;
  return true;
 };
 const Bridge={_Adapter:adapter,appendRegisterCall:(name,ns,fn)=>adapter.registerCall(name,ns,fn,2),removeRegisterCall:adapter.removeRegisterCall};
 let state={playing:{resourceTrackId:'startup-track',onlineResourceId:123,resourceName:'Restored song',resourceArtists:[],curTrack:{duration:240000},playingState:1,playId:'test-session',restoreResource:null,isLoadingFirst:false,loadingSeekDuration:0,curPlaying:{resourceId:'startup-track',track:{id:123}}}};
 function original(name,policy=mode){return adapter.registerCall(name,'audioplayer',()=>counts[name]++,policy===null?undefined:policy);}
 const boot=()=>names.map(name=>original(name));
 const notify=()=>{for(const fn of [...listeners])fn();};
 const store={getState:()=>state,subscribe:fn=>{listeners.add(fn);return()=>listeners.delete(fn);},dispatch:action=>{
  commands.push(action);if(bootOnSeek&&action.type==='playing/setPlayingPosition'){boot();if(confirmDuringSeek)slots.get('audioplayer.onSeek')?.('startup-track','first-seek',0,action.payload.duration);if(bootNotify)notify();}dispatchHandler?.(action);
 }};
 if(warm)boot();
 if(!inspectable)Bridge._Adapter=null;
 const req={c:{store:{exports:{a:{app:{_store:store,_models:[{namespace:'playing',reducers:{'playing/onLoadingSeekUpdate':()=>{}}}]}}}},bridge:{exports:{Bridge}},storage:{exports:{b:{lastPlaying:{get:async()=>({trackId:'startup-track',current:82})}}}}}};
 const chunks=[[]];chunks.push=entry=>Object.values(entry[1])[0]({}, {}, req);
 const env={webpackJsonp:chunks,console,performance:{now:()=>now},setTimeout,clearTimeout};
 let legacyReads=0;Object.defineProperty(env,'legacyNativeCmder',{get(){legacyReads++;return undefined;}});
 const api=createPlayerAdapter(env);
 return {api,adapter,Bridge,env,counts,slots,commands,original,boot,notify,state:()=>state.playing,advance:ms=>now+=ms,setDispatch:fn=>dispatchHandler=fn,update:patch=>{state={...state,playing:{...state.playing,...patch}};notify();},legacyReads:()=>legacyReads,
  emit:(name,...args)=>slots.get('audioplayer.on'+name)?.(...args)};
}


module.exports={fixture,names};
