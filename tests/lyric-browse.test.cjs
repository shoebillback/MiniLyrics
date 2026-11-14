const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const delay=ms=>new Promise(r=>setTimeout(r,ms));
(async()=>{
 const port=Number(process.argv[2]),capture=process.argv[3],dir=path.dirname(capture);let ws,pending,seekId=0;const commands=[];
 try{
  for(let i=0;i<30;i++){ws=new WebSocket('ws://127.0.0.1:'+port+'/');if(await new Promise(r=>{ws.onopen=()=>r(true);ws.onerror=()=>r(false);}))break;await delay(100);}
  ws.onmessage=e=>{const m=JSON.parse(e.data);if(m.type==='command'){commands.push(m);seekId=m.requestId||seekId;}if(pending?.type===m.type){const p=pending;pending=null;clearTimeout(p.timer);p.resolve(m);}};
  const send=m=>ws.send(JSON.stringify(m));
  const request=(m,type)=>new Promise((resolve,reject)=>{pending={type,resolve,timer:setTimeout(()=>{pending=null;reject(Error('timeout '+type));},5000)};send(m);});
  const settings=m=>request({type:'test-settings',...m},'test-settings-ack'),inspect=()=>request({type:'inspect'},'inspection');
  const clock=(position,playing=true,extra={})=>request({type:'clock',position,playing,valid:true,duration:180000,seekRequest:seekId,...extra},'host-state');
  const lyrics=lines=>request({type:'lyrics',songId:'browse',revision:1,lines},'lyrics-ack');
  const group=(s,i)=>{const g=s.groups.find(g=>g.index===i);assert.ok(g,'cached '+i);return g;};
  const near=(a,b)=>assert.ok(Math.abs(a-b)<.02,a+' / '+b);
  const shot=async name=>{send({type:'capture'});const s=await inspect();fs.renameSync(capture,path.join(dir,name+'.png'));return s;};
  const wheel=async(delta,extra={})=>{const s=await inspect();await request({type:'test-wheel',delta,x:s.barLeftPx/(s.dpi/96)+12,y:(s.viewportTop+s.viewportBottom)/2,...extra},'test-wheel-ack');return inspect();};
  send({type:'hello',token:'0123456789abcdef0123456789abcdef'});
  await settings({motionWall:1000,freezeClock:true,width:460,height:130,hover:false});
  send({type:'track',id:'browse',title:'滚轮回看歌词',artist:'不改变播放进度',duration:180000,cover:''});
  const all=Array.from({length:80},(_,i)=>({time:i*2000,text:'第 '+i+' 句：滚动浏览歌词'}));
  await lyrics(all);await clock(20000);let s=await inspect();assert.equal(s.index,10);assert.equal(s.browsing,false);assert.ok(s.groups.length>=13&&s.groups.length<=32);assert.equal(s.groupCacheLimit,32);
  s=await wheel(120,{x:1});assert.equal(s.browsing,false,'wheel over cover is ignored');
  s=await wheel(30);assert.equal(s.browsing,false);await wheel(30);await wheel(30);s=await wheel(30);assert.equal(s.browseIndex,9);assert.equal(s.focusIndex,9);assert.equal(s.index,10);near(s.positionMs,20000);assert.equal(commands.length,0,'wheel never seeks');near(group(s,9).durationMs,250);
  await settings({motionWall:1300});s=await shot('browse-up');assert.equal(group(s,9).rendered,true);near(group(s,9).fontSize,21);near(group(s,9).top,group(s,9).target);assert.ok(s.groups.some(g=>g.index===3),'six earlier groups cached');
  await clock(21000);await clock(22000);s=await inspect();assert.equal(s.index,11);assert.equal(s.focusIndex,9,'live row changes do not pull review back');near(s.positionMs,22000);assert.equal(commands.length,0);
  s=await wheel(720);assert.equal(s.browseIndex,3);s=await wheel(-120);assert.equal(s.browseIndex,4);await settings({motionWall:1600});s=await shot('browse-older');assert.equal(group(s,4).rendered,true);
  for(let i=0;i<8;i++){s=await wheel(i%2?12000:-12000);assert.ok(s.browseIndex>=0&&s.browseIndex<80);assert.ok(s.groups.length<=32);}
  s=await wheel(12000);assert.equal(s.browseIndex,0);s=await wheel(-12000);assert.equal(s.browseIndex,79);
  await settings({motionWall:5601});s=await inspect();assert.equal(s.browsing,false,'four seconds without scrolling returns to live');assert.equal(s.focusIndex,s.index);near(s.positionMs,22000);assert.equal(commands.length,0);
  await clock(22000,false);s=await wheel(120);assert.equal(s.browseIndex,10);assert.equal(s.focusIndex,10);assert.equal(s.frameRunning,true,'paused review animates temporarily');near(group(s,10).durationMs,250);await settings({motionWall:5901});s=await inspect();assert.equal(s.frameRunning,false,'paused review stops frames after its transition');near(group(s,10).top,group(s,10).target);await shot('browse-paused');
  const g=group(s,10),x=s.barLeftPx/(s.dpi/96)+12;await request({type:'test-click',x,y:g.top+3},'test-click-ack');assert.equal(commands.at(-1).action,'seek');near(commands.at(-1).position,20000);s=await inspect();assert.equal(s.browsing,false,'click exits review');assert.equal(s.index,10);await clock(20000,false,{seekState:'confirmed'});
  await settings({motionWall:6000,through:true});s=await wheel(120);assert.equal(s.browsing,false,'through mode ignores wheel');await settings({through:false});
  s=await wheel(120);assert.equal(s.browsing,true);await settings({motionWall:10001});s=await inspect();assert.equal(s.browsing,false,'auto-return also works while paused');assert.equal(s.focusIndex,s.index);assert.equal(s.frameRunning,true,'paused auto-return animates');await settings({motionWall:10501});s=await inspect();assert.equal(s.frameRunning,false,'paused auto-return stops frames after settling');
  await wheel(120);await lyrics(all.slice(0,3));s=await inspect();assert.ok(s.browseIndex<3);assert.ok(s.focusIndex<3);assert.ok(s.groups.length<=32);await lyrics([]);s=await wheel(120);assert.equal(s.browsing,false);assert.equal(s.lyricCount,0);
  // Manual inspection is also possible before the first timed line.
  await lyrics([{time:5000,text:'前奏后的第一句'},{time:8000,text:'第二句'}]);await clock(0,false);s=await wheel(-120);assert.equal(s.index,-1);assert.equal(s.browsing,true);assert.equal(group(s,s.browseIndex).rendered,true);await shot('browse-intro');
  send({type:'track',id:'next-song',title:'切歌退出浏览',artist:'',duration:90000,cover:''});s=await inspect();assert.equal(s.browsing,false);assert.equal(s.lyricCount,0);
  const result={pass:true,cacheLimit:32,neighborsEachSide:6,highResolutionWheel:true,wheelDoesNotSeek:true,clockContinues:true,boundsClamped:true,autoReturnMs:4000,pausedBrowse:true,pausedAutoReturn:true,reviewClickSeeks:true,throughIgnored:true,emptyAndIntroSafe:true,songChangeResets:true};fs.writeFileSync(path.join(dir,'browse-result.json'),JSON.stringify(result,null,2));console.log(JSON.stringify(result));
 }finally{if(ws?.readyState===1){ws.send(JSON.stringify({type:'exit'}));await delay(200);ws.close();}}
})().catch(e=>{console.error(e);process.exitCode=1;});
