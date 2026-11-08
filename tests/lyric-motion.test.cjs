const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const delay=ms=>new Promise(r=>setTimeout(r,ms));
(async()=>{
 const port=Number(process.argv[2]),capture=process.argv[3],dir=path.dirname(capture);let ws,pending,seq=0;
 const commands=[];let wallOffset=0;
 try{
  for(let i=0;i<30;i++){ws=new WebSocket('ws://127.0.0.1:'+port+'/');if(await new Promise(r=>{ws.onopen=()=>r(true);ws.onerror=()=>r(false);}))break;await delay(100);}
  ws.onmessage=e=>{const m=JSON.parse(e.data);if(m.type==='command'){commands.push(m);seq=m.requestId||seq;}if(pending?.type===m.type){const p=pending;pending=null;clearTimeout(p.timer);p.resolve(m);}};
  const send=m=>ws.send(JSON.stringify(m));
  const request=(m,type)=>new Promise((resolve,reject)=>{pending={type,resolve,timer:setTimeout(()=>{pending=null;reject(Error('timeout '+type));},5000)};send(m);});
  const inspect=()=>request({type:'inspect'},'inspection');
  const settings=m=>request({type:'test-settings',...m,...(m.motionWall!==undefined?{motionWall:m.motionWall+wallOffset}:{})},'test-settings-ack');
  const clock=(position,playing=true,extra={})=>request({type:'clock',position,playing,valid:true,duration:90000,seekRequest:seq,...extra},'host-state');
  const track=(id,title)=>{seq=0;send({type:'track',id,title,artist:'RNP 整组运动',duration:90000,cover:''});};
  const lyrics=(id,lines,revision=1)=>request({type:'lyrics',songId:id,revision,lines},'lyrics-ack');
  const shot=async name=>{send({type:'capture'});const s=await inspect();fs.renameSync(capture,path.join(dir,name+'.png'));return s;};
  const group=(s,id)=>{const g=s.groups.find(g=>g.index===id);assert.ok(g,'cached group '+id);return g;};
  const near=(a,b,msg)=>assert.ok(Math.abs(a-b)<.02,(msg||'values differ')+': '+a+' / '+b);
  const step=async(wall,position)=>{await settings({motionWall:wall});return clock(position);};
  send({type:'hello',token:'0123456789abcdef0123456789abcdef'});
  await settings({motionWall:1000,freezeClock:true,width:460,height:130,hover:false});
  send({type:'style',curve:0,stagger:true});
  track('translated','原文与翻译 · 整组上移');
  const translated=[{time:0,text:'晚风轻轻吹过',translation:'The evening breeze'},
   {time:2000,text:'把下一句唱给你听',translation:'Let the next line reach you'},
   {time:4000,text:'新的歌词正在靠近',translation:'Another line approaches'},
   {time:6000,text:'最后一组',translation:'The final group'}];
  await lyrics('translated',translated);await clock(1000);
  const before=await step(1100,1799),a0=group(before,0),b0=group(before,1),c0=group(before,2);
  near(a0.scale,1);near(b0.scale,16/21);near(b0.translationFontSize,16*16/21);await shot('translated-before');
  let s=await clock(1800);assert.equal(s.index,0);assert.equal(s.focusIndex,1);assert.equal(s.lookaheadMs,200);
  near(group(s,0).startAt-s.animationTimeMs,150);near(group(s,1).startAt-s.animationTimeMs,200);near(group(s,2).startAt-s.animationTimeMs,250);
  assert.equal(group(s,1).identity,b0.identity);assert.ok(Math.abs(group(s,1).target-b0.top)>40,'translated group must move by a full measured block, not 6 px');
  near(b0.top-a0.top,a0.height+before.groupGap,'measured block spacing');
  s=await step(1249,1949);near(group(s,0).top,a0.top);near(group(s,1).top,b0.top);
  s=await step(1300,2000);assert.equal(s.index,1);near(group(s,1).top,b0.top);
  await shot('translated-start');
  s=await step(1550,2250);const middle=group(s,1);assert.ok(middle.scale>16/21&&middle.scale<1);near(middle.translationFontSize,16*middle.scale);assert.ok(middle.top<b0.top&&middle.top>middle.target);
  assert.equal(middle.identity,b0.identity);near(middle.subOffset,(middle.mainHeight+6.3)*middle.scale);
  await shot('translated-mid');
  const dup=s;await lyrics('translated',JSON.parse(JSON.stringify(translated)),2);s=await inspect();
  assert.equal(s.lineBuildCalls,dup.lineBuildCalls);assert.equal(group(s,1).identity,b0.identity);near(group(s,1).top,middle.top);
  await clock(2250,false);const paused=await shot('paused-a');await settings({motionWall:2550});const pausedLater=await shot('paused-b');
  near(paused.animationTimeMs,pausedLater.animationTimeMs);near(group(paused,1).top,group(pausedLater,1).top);assert.equal(pausedLater.frameRunning,false);
  assert.ok(fs.readFileSync(path.join(dir,'paused-a.png')).equals(fs.readFileSync(path.join(dir,'paused-b.png'))));
  await clock(2250,true);s=await step(2800,2500);near(group(s,1).top,group(s,1).target);assert.ok(group(s,1).top+group(s,1).height<=s.viewportBottom+.01);await shot('translated-end');
  s=await step(2850,2550);assert.equal(s.transitionActive,false);
  // Click the translated row at its rendered location; it belongs to the same lyric group.
  const g=group(s,1),x=(s.coverLeftPx+s.coverSidePx)/(s.dpi/96)+28,y=g.top+g.subOffset+3;
  await request({type:'test-click',x,y},'test-click-ack');assert.equal(commands.at(-1).action,'seek');near(commands.at(-1).position,2000);await clock(2000,true,{seekState:'confirmed'});
  track('preview','下一句预览 · 连续队列');
  const preview=[{time:0,text:'上一句缓缓向上离开'},{time:2000,text:'这一句接着唱给你听'},{time:4000,text:'新的下一句从下方进入'},{time:6000,text:'再下一句'}];
  await lyrics('preview',preview);await clock(1000);s=await step(3000,1799);const pb=group(s,1),pc=group(s,2);near(group(s,0).fontSize,21);near(pb.fontSize,16);near(pc.fontSize,16);near(s.groupGap,7);assert.ok(pb.top-(group(s,0).top+group(s,0).renderedHeight)<=7.02,'compact gap');await shot('preview-before');
  s=await clock(1800);assert.equal(group(s,1).identity,pb.identity);near(group(s,2).mainAlpha,.4);
  near(group(s,2).startAt-group(s,1).startAt,50);near(group(s,1).scaleStartAt,group(s,1).startAt);near(group(s,1).scaleDurationMs,500);near(group(s,1).durationMs,500);near(group(s,2).durationMs,500);
  s=await step(3200,2000);near(group(s,1).fontSize,16);near(group(s,1).top,pb.top);near(group(s,2).top,pc.top);assert.equal(group(s,0).rendered,true,'outgoing group must remain visible at the boundary');assert.ok(group(s,0).fontSize<21&&group(s,0).fontSize>16,'outgoing group is shrinking');assert.ok(group(s,0).top<group(before,0).top||group(s,0).top<s.viewportTop+2,'outgoing group moves up');assert.equal(group(s,1).rendered,true);await shot('preview-start');
  s=await step(3250,2050);assert.equal(group(s,0).rendered,true,'exit remains visible after the boundary');near(group(s,2).top,pc.top);await shot('preview-50');
  s=await step(3450,2250);assert.ok(group(s,1).fontSize>16&&group(s,1).fontSize<21);assert.ok(group(s,0).fontSize>16&&group(s,0).fontSize<21);near(group(s,2).fontSize,16);assert.ok(group(s,2).top<pc.top);near(group(s,2).mainAlpha,.4);assert.equal(group(s,2).identity,pc.identity);await shot('preview-mid');
  await clock(2250,false);const frozenScale=await shot('preview-paused-a');wallOffset+=200;await settings({motionWall:3450});const frozenLater=await shot('preview-paused-b');near(group(frozenScale,1).scale,group(frozenLater,1).scale);assert.ok(fs.readFileSync(path.join(dir,'preview-paused-a.png')).equals(fs.readFileSync(path.join(dir,'preview-paused-b.png'))));await clock(2250,true);
  s=await step(3700,2500);near(group(s,1).top,group(s,1).target);assert.ok(group(s,2).top>group(s,2).target);await shot('preview-500');
  s=await step(3750,2550);near(group(s,2).top,group(s,2).target);assert.equal(s.transitionActive,false);assert.ok(group(s,1).top-2>=s.viewportTop-.01);near(group(s,0).fontSize,16);assert.equal(group(s,0).rendered,false);near(group(s,1).fontSize,21);near(group(s,2).fontSize,16);assert.ok(group(s,2).top+group(s,2).mainHeight*group(s,2).scale<=s.viewportBottom+.01);await shot('preview-end');
  const nextG=group(s,2);await request({type:'test-click',x,y:nextG.top+3},'test-click-ack');near(commands.at(-1).position,4000);await clock(4000,true,{seekState:'confirmed'});s=await inspect();assert.equal(s.transitionActive,false);
  // A new target during a running transition starts from the exact presented positions.
  track('rapid','连续短句');await lyrics('rapid',[{time:0,text:'A'},{time:2000,text:'B'},{time:2200,text:'C'},{time:2400,text:'D'},{time:5000,text:'E'}]);
  await clock(1000);await step(4000,1800);const running=await step(4250,1999);s=await clock(2000);
  for(const id of [0,1,2]){near(group(s,id).top,group(running,id).top,'retarget continuity');near(group(s,id).scale,group(running,id).scale,'scale retarget continuity');}
  await clock(1900);s=await inspect();assert.equal(s.transitionActive,false,'backwards seek snaps');assert.equal(s.index,0);near(group(s,0).fontSize,21);near(group(s,1).fontSize,16);
  // Curves and disabling stagger must affect actual position tracks.
  send({type:'style',curve:1,stagger:false});track('plain','关闭错位');await lyrics('plain',preview);await clock(1900);await settings({motionWall:5000});s=await clock(2000);
  assert.equal(s.focusIndex,1);assert.equal(s.lookaheadMs,0);assert.equal(s.curve,1);
  for(const id of [0,1,2])near(group(s,id).startAt,s.animationTimeMs);s=await step(5500,2500);assert.equal(s.transitionActive,false);
  send({type:'style',curve:0,stagger:true});track('moving-click','动画中点击');await lyrics('moving-click',translated);await clock(1000);await settings({motionWall:6000});await clock(1800);s=await step(6450,2250);
  const movingText=group(s,1);assert.ok(movingText.top>s.viewportTop&&movingText.top<s.viewportBottom);await request({type:'test-click',x,y:movingText.top+3},'test-click-ack');near(commands.at(-1).position,2000);await clock(2000,true,{seekState:'confirmed'});
  track('cache','有界缓存');await lyrics('cache',Array.from({length:50},(_,i)=>({time:i*1000,text:'第'+i+'句',translation:i%2?'Translation '+i:''})));
  for(let i=0;i<25;i++){s=await step(7000+i*300,i*1000+250);assert.ok(s.groups.length<=32);}
  // A long preview must stay pixel-identical while the short current lyric advances.
  const longText='这是一句超出窗口宽度的预览歌词，固定从句首显示。'.repeat(3);
  track('fixed-preview','下一句预览固定');await lyrics('fixed-preview',[{time:0,text:'当前短句',duration:6000},{time:6000,text:longText,duration:6000},{time:12000,text:'后续歌词'}]);
  await settings({motionWall:16000});await clock(1000);await shot('fixed-preview-a');s=await step(17500,2500);await shot('fixed-preview-b');
  assert.ok(fs.readFileSync(path.join(dir,'fixed-preview-a.png')).equals(fs.readFileSync(path.join(dir,'fixed-preview-b.png'))),'long preview must not move horizontally');assert.equal(s.intervalMs,200,'long preview alone must not force animation-rate refresh');
  s=await step(18000,6000);assert.equal(s.index,1);await shot('promoted-long-start');await step(19000,7000);await shot('promoted-long-scroll');assert.ok(!fs.readFileSync(path.join(dir,'promoted-long-start.png')).equals(fs.readFileSync(path.join(dir,'promoted-long-scroll.png'))),'promoted current lyric still scrolls');
  track('translation-scroll','翻译仍可横向滚动');await lyrics('translation-scroll',[{time:0,text:'当前短句',translation:longText,duration:6000},{time:6000,text:'下一句'}]);await settings({motionWall:20000});await clock(1000);await shot('translation-scroll-a');await step(21500,2500);await shot('translation-scroll-b');assert.ok(!fs.readFileSync(path.join(dir,'translation-scroll-a.png')).equals(fs.readFileSync(path.join(dir,'translation-scroll-b.png'))),'current translation must retain horizontal scrolling');
  const result={pass:true,fixedPreviewPixelsIdentical:true,previewNoFastRefresh:true,currentScrollRetained:true,translationScrollRetained:true,translatedGroupMoves:true,previewFontSize:16,currentFontSize:21,smoothPromotion:true,compactGapDip:7,outgoingVisibleDuringMotion:true,pastHiddenAfterExit:true,unifiedVisibleRows:true,scalePausePixelsIdentical:true,translationScalesWithGroup:true,measuredHeight:true,lookaheadMs:200,delaysMs:[150,200,250],durationMs:500,stableIdentities:true,pausePixelsIdentical:true,previewOpacityStable:true,clicksRetained:true,movingClickFollowsPositions:true,retargetContinuous:true,cacheLimit:32};
  fs.writeFileSync(path.join(dir,'motion-result.json'),JSON.stringify(result,null,2));console.log(JSON.stringify(result));
 }finally{if(ws?.readyState===1){ws.send(JSON.stringify({type:'exit'}));await delay(200);ws.close();}}
})().catch(e=>{console.error(e);process.exitCode=1;});
