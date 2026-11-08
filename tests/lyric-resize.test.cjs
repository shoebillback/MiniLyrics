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
  const clock=(position,extra={})=>request({type:'clock',position,playing:false,valid:true,duration:240000,seekRequest:seekId,...extra},'host-state');
  const group=(s,i)=>{const g=s.groups.find(g=>g.index===i);assert.ok(g,'cached '+i);return g;};
  const near=(a,b)=>assert.ok(Math.abs(a-b)<.03,a+' / '+b);
  const fullRows=s=>s.groups.filter(g=>g.rendered&&g.top>=s.viewportTop-.02&&g.top+g.renderedHeight<=s.viewportBottom+.02);
  const shot=async name=>{send({type:'capture'});const s=await inspect();fs.renameSync(capture,path.join(dir,name+'.png'));return s;};
  const track=(id,title)=>{seekId=0;send({type:'track',id,title,artist:'字号不变 · 扩大可见面积',duration:240000,cover:''});};
  send({type:'hello',token:'0123456789abcdef0123456789abcdef'});await settings({motionWall:1000,freezeClock:true,width:360,height:130,hover:false});
  track('area','窗口变高显示更多歌词');await request({type:'lyrics',songId:'area',revision:1,lines:Array.from({length:60},(_,i)=>({time:i*4000,text:'第 '+i+' 句：保持原有字号'}))},'lyrics-ack');await clock(40000);
  let s=await shot('area-minimum');assert.equal(s.index,10);assert.equal(fullRows(s).length,2);near(group(s,10).fontSize,21);near(group(s,11).fontSize,16);const built=s.lineBuildCalls,identity=group(s,10).identity;const rows=[];
  for(const [width,height]of [[540,192],[900,240],[360,240],[900,130]]){
   await settings({width,height,hover:true});s=await inspect();assert.equal(s.lineBuildCalls,built,'resizing must reuse text layouts');assert.equal(group(s,10).identity,identity);near(group(s,10).fontSize,21);near(group(s,11).fontSize,16);near(group(s,9).fontSize,16);near(s.groupGap,7);
   assert.ok(s.viewportTop>=32);assert.ok(s.viewportBottom<=s.controlYPx/(s.dpi/96)-14+.05,'lyrics stay above controls');
   if(height>130){assert.ok(fullRows(s).length>=4,'taller viewport must show more complete lyric rows');assert.ok(group(s,9).rendered,'past context is visible in taller window');}
   else {assert.equal(fullRows(s).length,2);}
   rows.push({width,height,fullRows:fullRows(s).length,mainFont:group(s,10).fontSize,previewFont:group(s,11).fontSize});await shot('area-'+width+'-'+height);
  }
  await settings({width:360,height:130,hover:false});await shot('area-minimum-again');assert.ok(fs.readFileSync(path.join(dir,'area-minimum.png')).equals(fs.readFileSync(path.join(dir,'area-minimum-again.png'))),'return to minimum restores identical pixels');
  // Visible history in the expanded area can be clicked, unlike hidden history in compact mode.
  await settings({width:540,height:192});s=await inspect();const past=group(s,9),x=s.barLeftPx/(s.dpi/96)+12;assert.ok(past.top>s.viewportTop);await request({type:'test-click',x,y:past.top+3},'test-click-ack');assert.equal(commands.at(-1).action,'seek');near(commands.at(-1).position,36000);await clock(36000,{seekState:'confirmed'});
  s=await inspect();await request({type:'test-wheel',x,y:(s.viewportTop+s.viewportBottom)/2,delta:120},'test-wheel-ack');s=await inspect();assert.equal(s.browseIndex,8);await settings({motionWall:5001});s=await inspect();assert.equal(s.browsing,false);assert.equal(s.focusIndex,9);
  track('translated-area','翻译模式同样扩展可见范围');await request({type:'lyrics',songId:'translated-area',revision:1,lines:Array.from({length:30},(_,i)=>({time:i*4000,text:'第 '+i+' 句歌词',translation:'Translation for line '+i}))},'lyrics-ack');await settings({width:900,height:240,hover:true});await clock(40000);s=await shot('area-translation');near(group(s,10).fontSize,21);near(group(s,10).scale,1);assert.ok(s.groups.filter(g=>g.rendered).length>=3);assert.ok(group(s,10).top>=s.viewportTop&&group(s,10).top+group(s,10).renderedHeight<=s.viewportBottom);
  track('moving-area','放大时保留原有交接');await request({type:'lyrics',songId:'moving-area',revision:1,lines:Array.from({length:12},(_,i)=>({time:i*2000,text:'第 '+i+' 句移动歌词'}))},'lyrics-ack');await settings({width:540,height:192,motionWall:6000,hover:false});await clock(1000,{playing:true});await clock(1799,{playing:true});await clock(1800,{playing:true});await settings({motionWall:6250});await clock(2050,{playing:true});const moving=await shot('area-moving');assert.ok(group(moving,1).fontSize>16&&group(moving,1).fontSize<21);const movingBuilds=moving.lineBuildCalls;await settings({width:900,height:240});s=await inspect();near(group(s,1).fontSize,group(moving,1).fontSize);assert.equal(group(s,1).identity,group(moving,1).identity);assert.equal(s.lineBuildCalls,movingBuilds);await settings({motionWall:7000});s=await inspect();near(group(s,1).fontSize,21);await shot('area-moving-end');
  track('paused-nearby','暂停时首次同步到缓存中的歌词');await request({type:'lyrics',songId:'paused-nearby',revision:1,lines:Array.from({length:12},(_,i)=>({time:i*4000,text:'第 '+i+' 句暂停歌词'}))},'lyrics-ack');await clock(12000);s=await inspect();assert.equal(s.focusIndex,3);near(group(s,3).mainAlpha,1);near(group(s,3).fontSize,21);
  const result={pass:true,pausedFirstSyncCorrect:true,resizeDuringMotionKeepsFont:true,fontSizesUnchanged:[21,16],gapDip:7,layoutsReused:true,minimumPixelsRestored:true,visiblePastClickSeeks:true,wheelAndAutoFollow:true,translationAreaExpands:true,rows};fs.writeFileSync(path.join(dir,'resize-result.json'),JSON.stringify(result,null,2));console.log(JSON.stringify(result));
 }finally{if(ws?.readyState===1){ws.send(JSON.stringify({type:'exit'}));await delay(200);ws.close();}}
})().catch(e=>{console.error(e);process.exitCode=1;});
