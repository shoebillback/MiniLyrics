const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const delay=ms=>new Promise(r=>setTimeout(r,ms));
(async()=>{
 const port=Number(process.argv[2]),capture=process.argv[3],dir=path.dirname(capture);let ws,pending,seq=0;
 const commands=[];
 try{
  for(let i=0;i<30;i++){ws=new WebSocket('ws://127.0.0.1:'+port+'/');if(await new Promise(r=>{ws.onopen=()=>r(true);ws.onerror=()=>r(false);}))break;await delay(100);}
  ws.onmessage=e=>{const m=JSON.parse(e.data);if(m.type==='command'){commands.push(m);seq=m.requestId||seq;}if(pending?.type===m.type){const p=pending;pending=null;clearTimeout(p.timer);p.resolve(m);}};
  const send=m=>ws.send(JSON.stringify(m));
  const request=(m,type)=>new Promise((resolve,reject)=>{pending={type,resolve,timer:setTimeout(()=>{pending=null;reject(Error('timeout '+type));},5000)};send(m);});
  const inspect=()=>request({type:'inspect'},'inspection');
  const settings=m=>request({type:'test-settings',...m},'test-settings-ack');
  const clock=(position,playing=true,extra={})=>request({type:'clock',position,playing,valid:true,duration:90000,seekRequest:seq,...extra},'host-state');
  const track=(id,title)=>{seq=0;send({type:'track',id,title,artist:'RNP 整组运动',duration:90000,cover:''});};
  const lyrics=(id,lines,revision=1)=>request({type:'lyrics',songId:id,revision,lines},'lyrics-ack');
  const shot=async name=>{send({type:'capture'});const s=await inspect();fs.renameSync(capture,path.join(dir,name+'.png'));return s;};
  const group=(s,id)=>{const g=s.groups.find(g=>g.index===id);assert.ok(g,'cached group '+id);return g;};
  const near=(a,b,msg)=>assert.ok(Math.abs(a-b)<.02,(msg||'values differ')+': '+a+' / '+b);
  const step=async(wall,position)=>{await settings({motionWall:wall});return clock(position);};
  send({type:'hello',token:'0123456789abcdef0123456789abcdef'});await settings({fps:60,hover:false});
  track('fps','帧率采样');await lyrics('fps',[{time:0,duration:90000,text:'持续逐字动画',words:[{time:0,duration:90000,text:'持续逐字动画'}]}]);await clock(1000);await delay(300);
  const before=await inspect(),t0=performance.now();await delay(2000);const after=await inspect(),elapsed=performance.now()-t0;console.log(JSON.stringify({requestedFps:60,renderCalls:after.renderCalls-before.renderCalls,elapsedMs:elapsed,observedRenderHz:(after.renderCalls-before.renderCalls)*1000/elapsed,intervalMs:after.intervalMs}));
 }finally{if(ws?.readyState===1){ws.send(JSON.stringify({type:'exit'}));await delay(200);ws.close();}}
})().catch(e=>{console.error(e);process.exitCode=1;});
