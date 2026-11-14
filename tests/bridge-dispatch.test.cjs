const {test}=require('node:test');
const assert=require('node:assert/strict');
const {createPlayerAdapter}=require('../player.js');
// APPEND and removal mirror the implementations supplied from the live client.
// Single-slot channel is an explicit model assumption, not a verified native implementation.
function fixture(){
 const slots=new Map(),channel={registerCall:(event,fn)=>slots.set(event,fn)};
 function adapter(){const a={registerCallMap:{},registerCallStubs:{}};a.queue=event=>a.registerCallStubs[event]??=(...args)=>{const entry=a.registerCallMap[event];if(entry)(Array.isArray(entry)?entry:[entry]).forEach(fn=>fn.apply(globalThis,args));};
 const b={appendRegisterCall(name,ns,fn){const ev=ns+'.on'+name;if(!a.registerCallMap[ev]){a.registerCallMap[ev]=[];channel.registerCall(ev,a.queue(ev));}if(!Array.isArray(a.registerCallMap[ev])){a.registerCallMap[ev]=[a.registerCallMap[ev]];channel.registerCall(ev,a.queue(ev));}a.registerCallMap[ev].push(fn);},removeRegisterCall(name,ns,fn){const entry=a.registerCallMap[ns+'.on'+name];if(Array.isArray(entry)){const i=entry.indexOf(fn);if(i>=0)entry.splice(i,1);}}};b._Adapter=a;return {a,b};}
 const internal=adapter(),legacy=adapter();let nativeTicks=0,globalTicks=0;
 internal.b.appendRegisterCall('PlayProgress','audioplayer',()=>nativeTicks++);
 legacy.b.appendRegisterCall('PlayProgress','audioplayer',()=>globalTicks++);
 let state={playing:{resourceTrackId:'3437177548',onlineResourceId:3437177548,resourceArtists:[],playingState:2}};
 const store={getState:()=>state,subscribe:()=>()=>{},dispatch:()=>{}};
 const req={c:{store:{exports:{a:{app:{_store:store}}}},bridge:{exports:{Bridge:internal.b}}}};
 const chunks=[[]];chunks.push=entry=>Object.values(entry[1])[0]({}, {}, req);
 const env={webpackJsonp:chunks,legacyNativeCmder:legacy.b,console,setTimeout,clearTimeout};
 return {internal,legacy,env,setState:v=>state=v,emit:()=>slots.get('audioplayer.onPlayProgress')('3437177548_5D7DZG',195.899,1),counts:()=>({nativeTicks,globalTicks})};
}
test('single-slot model reproduces live split with the production MiniLyrics adapter',()=>{
 const f=fixture(),api=createPlayerAdapter(f.env);try{f.emit();assert.deepEqual(f.counts(),{nativeTicks:0,globalTicks:1});assert.equal(api.getTimeline(),null);}finally{api.dispose();}
});
test('actual queue structure lets one listener exception stop later listeners',()=>{
 const f=fixture();let later=0;const bad=()=>{throw Error('foreign-listener');};f.legacy.b.appendRegisterCall('PlayProgress','audioplayer',bad);f.legacy.b.appendRegisterCall('PlayProgress','audioplayer',()=>later++);
 assert.throws(f.emit,/foreign-listener/);assert.equal(later,0);f.legacy.b.removeRegisterCall('PlayProgress','audioplayer',bad);f.emit();assert.equal(later,1);
});
test('MiniLyrics callback failures cannot interrupt later shared listeners',()=>{
 const f=fixture(),api=createPlayerAdapter(f.env);try{f.setState({playing:{resourceTrackId:'3437177548',onlineResourceId:3437177548,resourceArtists:{malformed:true},playingState:2}});
 let later=0;f.internal.b.appendRegisterCall('PlayProgress','audioplayer',()=>later++);assert.doesNotThrow(()=>f.internal.a.queue('audioplayer.onPlayProgress')('3437177548_5D7DZG',195.899,1));assert.equal(later,1);
 }finally{api.dispose();}
});
