// Isolated Win32 input test. Never connects to the user's music player.
const fs=require('node:fs/promises'),cp=require('node:child_process'),util=require('node:util'),net=require('node:net'),path=require('node:path'),assert=require('node:assert/strict');
const execFile=util.promisify(cp.execFile),delay=ms=>new Promise(r=>setTimeout(r,ms));
(async()=>{
 const dir=process.argv[2]||path.join(require('node:os').tmpdir(),'MiniLyrics-interaction-'+Date.now());
 await fs.mkdir(dir,{recursive:true});
 await fs.writeFile(path.join(dir,'settings.json'),JSON.stringify({Width:360,Height:110,Top:true,Translation:true,Fps:30}));
 const probe=net.createServer();await new Promise(r=>probe.listen(0,'127.0.0.1',r));const port=probe.address().port;await new Promise(r=>probe.close(r));
 const capture=path.join(dir,'capture.png'),native=cp.spawn(path.join(__dirname,'../MiniLyricsHost.exe'),['--port',String(port),'--token','0123456789abcdef0123456789abcdef','--capture',capture],{windowsHide:true,env:{...process.env,MINILYRICS_SETTINGS:path.join(dir,'settings.json')}});
 let ws,pending;const commands=[];
 try{
  for(let i=0;i<30;i++){ws=new WebSocket('ws://127.0.0.1:'+port+'/');if(await new Promise(r=>{ws.onopen=()=>r(true);ws.onerror=()=>r(false);}))break;await delay(100);}
  assert.equal(ws.readyState,1);
  ws.onmessage=e=>{const m=JSON.parse(e.data);if(m.type==='command')commands.push(m);if(pending?.type===m.type){const p=pending;pending=null;clearTimeout(p.timer);p.resolve(m);}};
  const send=m=>ws.send(JSON.stringify(m));
  const request=(m,type)=>new Promise((resolve,reject)=>{pending={type,resolve,timer:setTimeout(()=>{pending=null;reject(Error('timeout '+type));},5000)};send(m);});
  const inspect=()=>request({type:'inspect'},'inspection');
  const captureFrame=async name=>{send({type:'capture'});await inspect();for(let i=0;i<100;i++){try{await fs.access(capture);break;}catch{await delay(10);}}await fs.rename(capture,path.join(dir,name+'.png'));return inspect();};
  const action=(type,x,y)=>execFile(path.join(__dirname,'window-probe.exe'),[String(native.pid),type,String(Math.round(x)),String(Math.round(y))]);
  const click=async(x,y)=>{await request({type:'test-settings',hover:true},'test-settings-ack');await action('down',x,y);await action('up',x,y);await inspect();};
  const clock=(position,id=0,seekState='')=>request({type:'clock',songId:'seek-test',position,playing:false,valid:true,duration:90000,seekRequest:id,seekState},'host-state');
  send({type:'hello',token:'0123456789abcdef0123456789abcdef'});
  send({type:'track',id:'seek-test',title:'中文歌曲 · 自适应测试',artist:'RNP + InfLink',duration:90000,cover:''});
  await request({type:'lyrics',songId:'seek-test',revision:1,lines:[{time:0,text:'窗口变化，控件随之对齐',translation:'Responsive controls'}]},'lyrics-ack');
  await clock(10000);
  const initial=await inspect();assert.ok(initial.windowWidthPx>=360*(initial.dpi/96)-1);assert.ok(initial.windowHeightPx>=130*(initial.dpi/96)-1);
  const layouts=[];
  for(const [width,height]of [[360,110],[546,164],[900,240]]){
   await request({type:'test-settings',hover:true,width,height},'test-settings-ack');
   let s=await inspect();assert.ok(s.windowWidthPx>=360*(s.dpi/96)-1);assert.ok(s.windowHeightPx>=130*(s.dpi/96)-1);const y=s.controlYPx,x=s.barLeftPx+(s.barRightPx-s.barLeftPx)*2/3;
   await request({type:'test-settings',hover:true},'test-settings-ack');const before=await captureFrame('before-'+width);
   await action('down',x,y);await captureFrame('dragging-'+width);
   await action('up',x,y);const released=await captureFrame('released-'+width);
   const seek=commands.at(-1);assert.equal(seek.action,'seek');assert.equal(seek.songId,'seek-test');
   assert.ok(Math.abs(released.positionMs-seek.position)<1);assert.equal(released.seekPending,true);
   // A pre-seek clock already queued in the bridge cannot undo the optimistic position.
   send({type:'clock',songId:'seek-test',position:10000,playing:false,valid:true,seekRequest:0});
   assert.ok(Math.abs((await inspect()).positionMs-seek.position)<1);
   await clock(seek.position,seek.requestId,'pending');assert.equal((await inspect()).seekPending,true);
   await clock(seek.position,seek.requestId,'confirmed');assert.equal((await inspect()).seekPending,false);
   await captureFrame('confirmed-'+width);
   for(const [field,expected]of [['previousPx','previous'],['playPx','play'],['nextPx','next']]){
    const count=commands.length;await click(s[field],y);assert.equal(commands.length,count+1);assert.equal(commands.at(-1).action,expected);
   }
   layouts.push({width,height,pixelWidth:s.windowWidthPx,barLeftPx:s.barLeftPx,barRightPx:s.barRightPx,controlYPx:y,nextPx:s.nextPx,target:seek.position,afterRelease:released.positionMs});
   await clock(10000,seek.requestId);
  }
  assert.ok(layouts[1].barRightPx-layouts[1].barLeftPx>layouts[0].barRightPx-layouts[0].barLeftPx);
  assert.ok(layouts[2].barRightPx-layouts[2].barLeftPx>layouts[1].barRightPx-layouts[1].barLeftPx);
  let s=await inspect(),y=s.controlYPx,left=s.barLeftPx,length=s.barRightPx-left;
  await click(left+length*.8,y);const first=commands.at(-1);
  await click(left+length*.2,y);const second=commands.at(-1);
  assert.ok(second.requestId>first.requestId);
  send({type:'clock',songId:'seek-test',position:first.position,playing:false,valid:true,seekRequest:first.requestId,seekState:'confirmed'});
  s=await inspect();assert.equal(s.seekPending,true);assert.ok(Math.abs(s.positionMs-second.position)<1);
  await clock(10000,second.requestId,'failed');assert.equal((await inspect()).seekPending,false);
  await click(left+length*.5,y);assert.equal((await inspect()).seekPending,true);
  await delay(3650);s=await inspect();assert.equal(s.seekPending,false);assert.equal(s.error,'seek-timeout');assert.ok(Math.abs(s.positionMs-10000)<1);
  const result={pass:true,layouts,oldClockIgnored:true,latestSeekWins:true,failureRestores:true,timeoutRestores:true,buttonHitTests:9,dpi:s.dpi};
  await fs.writeFile(path.join(dir,'result.json'),JSON.stringify(result,null,2));console.log(JSON.stringify(result));
 }finally{
  if(ws?.readyState===1){ws.send(JSON.stringify({type:'exit'}));await delay(200);ws.close();}
  if(native.exitCode===null)await new Promise(resolve=>{const timer=setTimeout(()=>{native.kill();resolve();},5000);native.once('exit',()=>{clearTimeout(timer);resolve();});});
 }
})().catch(e=>{console.error(e);process.exitCode=1;});
