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
  const clock=(position,playing=true,extra={})=>request({type:'clock',position,playing,valid:true,duration:90000,seekRequest:seekId,...extra},'host-state');
  const track=(id,title)=>{seekId=0;send({type:'track',id,title,artist:'统一布局 · 末句不收缩',duration:90000,cover:''});};
  const lyrics=(id,lines)=>request({type:'lyrics',songId:id,revision:1,lines},'lyrics-ack');
  const group=(s,i)=>{const g=s.groups.find(g=>g.index===i);assert.ok(g,'cached '+i);return g;};
  const near=(a,b,msg='values differ')=>assert.ok(Math.abs(a-b)<.03,msg+': '+a+' / '+b);
  const sameBounds=(a,b)=>{for(const key of ['viewportTop','viewportBottom','layoutAnchor'])near(a[key],b[key],key+' must not depend on lyric count or end');};
  const shot=async name=>{send({type:'capture'});const s=await inspect();fs.renameSync(capture,path.join(dir,name+'.png'));return s;};
  const step=async(wall,time)=>{await settings({motionWall:wall});return clock(time);};
  send({type:'hello',token:'0123456789abcdef0123456789abcdef'});
  const cases=[['plain-min',360,130,'plain'],['plain-mid',540,192,'plain'],['plain-max',900,240,'plain'],['translated-min',360,130,'translated'],['translated-max',900,240,'translated'],['mixed-min',360,130,'mixed']];const results=[];
  for(let c=0;c<cases.length;c++){
   const [id,width,height,mode]=cases[c],wall=1000+c*5000;
   await settings({motionWall:wall,freezeClock:true,width,height,hover:false});track(id,'最后一句 · '+mode);
   const lines=Array.from({length:4},(_,i)=>({time:i*2000,text:'第'+i+'句',...(mode==='translated'||mode==='mixed'&&i<3?{translation:'译文'+i}:{})}));await lyrics(id,lines);await clock(1000);const initial=await inspect();near(initial.viewportTop,32);assert.ok(initial.viewportBottom<=initial.controlYPx/(initial.dpi/96)-14+.04);
   await clock(1799);const middle=await clock(1800);sameBounds(initial,middle);near(group(middle,1).target,initial.layoutAnchor);
   let s=await step(wall+200,2000);sameBounds(initial,s);s=await step(wall+750,2550);near(group(s,1).top,initial.layoutAnchor);
   await clock(3550);await clock(4000);await step(wall+1500,4750);await settings({motionWall:wall+2000});await clock(5799);const before=await shot(id+'-before-final');assert.equal(before.index,2);assert.equal(before.focusIndex,2);near(group(before,2).top,initial.layoutAnchor);
   await clock(5800);const layout=await shot(id+'-final-layout');sameBounds(initial,layout);near(group(layout,2).top,group(before,2).top);near(group(layout,3).top,group(before,3).top);near(group(layout,3).target,initial.layoutAnchor);assert.ok(fs.readFileSync(path.join(dir,id+'-before-final.png')).equals(fs.readFileSync(path.join(dir,id+'-final-layout.png'))),'lookahead into final lyric must not crop or move pixels immediately');
   s=await step(wall+2200,6000);assert.equal(s.index,3);assert.equal(group(s,2).rendered,true,'penultimate lyric keeps its exit');sameBounds(initial,s);await shot(id+'-final-start');
   s=await step(wall+2450,6250);assert.ok(group(s,3).top>initial.layoutAnchor&&group(s,3).top<group(before,3).top);await shot(id+'-final-mid');
   await clock(6250,false);const paused=await shot(id+'-paused-a');await settings({motionWall:wall+2700});const later=await shot(id+'-paused-b');assert.ok(fs.readFileSync(path.join(dir,id+'-paused-a.png')).equals(fs.readFileSync(path.join(dir,id+'-paused-b.png'))));near(group(paused,3).top,group(later,3).top);await clock(6250,true);
   s=await step(wall+3000,6550);sameBounds(initial,s);near(group(s,3).top,initial.layoutAnchor);near(group(s,3).fontSize,21);assert.equal(s.lyricCount,4);assert.ok(s.groups.every(g=>g.index<4),'no dummy final row is inserted');assert.ok(group(s,3).top+group(s,3).renderedHeight<=s.viewportBottom+.03);assert.equal(s.transitionActive,false);await shot(id+'-final-end');
   s=await step(wall+3500,7000);sameBounds(initial,s);near(group(s,3).top,initial.layoutAnchor);
   track(id+'-single','单句与多句共用布局');await lyrics(id+'-single',[{time:0,text:'只有一句',...(mode==='translated'?{translation:'Single line'}:{})}]);await clock(1000,false);s=await inspect();sameBounds(initial,s);near(group(s,0).top,initial.layoutAnchor);assert.equal(s.lyricCount,1);assert.equal(s.groups.length,1);
   results.push({id,width,height,mode,stableFinalLayout:true,lookaheadPixelsIdentical:true,exitContinues:true,pausePixelsIdentical:true,singleRowSameAnchor:true});
  }
  // The same visible-row hit rule also applies to a retiring row in the small window.
  await settings({width:360,height:130,motionWall:40000});track('visible-click','可见即绘制与命中');await lyrics('visible-click',[{time:0,text:'上一句'},{time:2000,text:'当前句'},{time:4000,text:'下一句'}]);await clock(1000);await clock(1800);let s=await step(40200,2000),g=group(s,0);assert.equal(g.rendered,true);const x=s.barLeftPx/(s.dpi/96)+12,y=Math.max(s.viewportTop+2,g.top+g.renderedHeight/2);await request({type:'test-click',x,y},'test-click-ack');assert.equal(commands.at(-1).action,'seek');near(commands.at(-1).position,0);await clock(0,false,{seekState:'confirmed'});
  const result={pass:true,unifiedViewport:true,stableLastLyric:true,noDummyLyrics:true,visibleExitClickable:true,cases:results};fs.writeFileSync(path.join(dir,'layout-result.json'),JSON.stringify(result,null,2));console.log(JSON.stringify(result));
 }finally{if(ws?.readyState===1){ws.send(JSON.stringify({type:'exit'}));await delay(200);ws.close();}}
})().catch(e=>{console.error(e);process.exitCode=1;});
