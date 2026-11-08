const assert=require('node:assert/strict');
const fs=require('node:fs');
const http=require('node:http');
const path=require('node:path');
const zlib=require('node:zlib');
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
function crc32(bytes){let crc=0xffffffff;for(const byte of bytes){crc^=byte;for(let bit=0;bit<8;bit++)crc=(crc>>>1)^((crc&1)?0xedb88320:0);}return (crc^0xffffffff)>>>0;}
function chunk(type,bytes){const name=Buffer.from(type),head=Buffer.alloc(4),tail=Buffer.alloc(4);head.writeUInt32BE(bytes.length);tail.writeUInt32BE(crc32(Buffer.concat([name,bytes])));return Buffer.concat([head,name,bytes,tail]);}
function coverPng(){const ihdr=Buffer.alloc(13);ihdr.writeUInt32BE(1,0);ihdr.writeUInt32BE(1,4);ihdr[8]=8;ihdr[9]=6;return Buffer.concat([Buffer.from('89504e470d0a1a0a','hex'),chunk('IHDR',ihdr),chunk('IDAT',zlib.deflateSync(Buffer.from([0,40,120,200,255]))),chunk('IEND',Buffer.alloc(0))]);}
(async()=>{
 const port=Number(process.argv[2]),capture=process.argv[3];let ws,pending;
 let releaseCoverBatch;const coverBatchReady=new Promise(resolve=>releaseCoverBatch=resolve);
 const paths=[];const fixture=coverPng();const server=http.createServer((req,res)=>{paths.push(req.url);const respond=()=>setTimeout(()=>{res.writeHead(200,{'Content-Type':'image/png','Content-Length':fixture.length});res.end(fixture);},100);if(req.url.startsWith('/cover-'))coverBatchReady.then(respond);else respond();});
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
 try{
  for(let i=0;i<30;i++){ws=new WebSocket(`ws://127.0.0.1:${port}/`);if(await new Promise(resolve=>{ws.onopen=()=>resolve(true);ws.onerror=()=>resolve(false);}))break;ws.close();await delay(100);}
  assert.equal(ws.readyState,1);ws.onmessage=event=>{const message=JSON.parse(event.data);if(pending&&message.type===pending.type){const {resolve,timer}=pending;pending=null;clearTimeout(timer);resolve(message);}};
  const send=message=>ws.send(JSON.stringify(message));
  const request=(message,type)=>new Promise((resolve,reject)=>{const timer=setTimeout(()=>{pending=null;reject(Error('host reply timed out'));},5000);pending={type,resolve,timer};send(message);});
  const inspect=()=>request({type:'inspect'},'inspection');
  send({type:'hello',token:'0123456789abcdef0123456789abcdef'});
  send({type:'track',id:'local-unicode',title:'中文标题 🎵',artist:'测试',duration:30000,cover:`http://127.0.0.1:${server.address().port}/cover.png`});
  await request({type:'lyrics',songId:'local-unicode',revision:1,lines:[
   {time:0,duration:5000,text:'你好 🎵',translation:'Hello',words:[{text:'你好 🎵',time:0,duration:3000}]},
   {time:5000,duration:5000,text:'下一行',words:[]}]},'lyrics-ack');
  await request({type:'clock',position:1000,playing:true,valid:true,duration:30000},'host-state');
  let state=await inspect();assert.ok(state.windowWidthPx>=360*(state.dpi/96)-1);assert.ok(state.windowHeightPx>=130*(state.dpi/96)-1);assert.equal(state.backend,'Direct2D/DirectWrite');assert.equal(state.first,'你好 🎵');assert.equal(state.second,'Hello');
  const beforeDuplicate=await inspect();
  const sameLines=[{time:0,duration:5000,text:'你好 🎵',translation:'Hello',words:[{text:'你好 🎵',time:0,duration:3000}]},{time:5000,duration:5000,text:'下一行',words:[]}];
  await request({type:'lyrics',songId:'local-unicode',revision:2,lines:sameLines},'lyrics-ack');
  state=await inspect();assert.equal(state.lineBuildCalls,beforeDuplicate.lineBuildCalls);assert.equal(state.rowStartedMs,beforeDuplicate.rowStartedMs);
  sameLines[0].translation='Updated translation';
  await request({type:'lyrics',songId:'local-unicode',revision:3,lines:sameLines},'lyrics-ack');
  state=await inspect();assert.equal(state.second,'Updated translation');assert.equal(state.rowStartedMs,beforeDuplicate.rowStartedMs);
  await request({type:'test-settings',translation:false,through:true,top:false,fps:60},'test-settings-ack');
  state=await inspect();assert.equal(state.through,true);assert.equal(state.top,false);assert.equal(state.fps,60);assert.equal(state.second,'下一行');assert.ok(state.intervalMs<=17);assert.equal(state.error,'');
  send({type:'unlock'});state=await inspect();assert.equal(state.through,false);
  const rejected=await request({type:'lyrics',songId:'old-track',lines:[{time:0,text:'wrong'}]},'lyrics-ack');assert.equal(rejected.accepted,false);assert.equal((await inspect()).first,'你好 🎵');
  await request({type:'lyrics',songId:'local-unicode',revision:2,lines:[null,{time:0,text:'有效歌词',words:[null,{time:0,duration:5000,text:'有效歌词'}]}]},'lyrics-ack');
  for(let i=0;i<50;i++){state=await inspect();if(state.coverLoaded)break;await delay(100);}
  assert.equal(state.coverLoaded,true,'WinHTTP + WIC cover decoding succeeds');assert.equal(state.first,'有效歌词');assert.equal(state.lyricCount,1);
  const beforeCovers=paths.length;
  for(let i=0;i<6;i++)send({type:'track',id:'local-unicode',title:'中文标题 🎵',artist:'测试',duration:30000,cover:`http://127.0.0.1:${server.address().port}/cover-${i}.png`});
  // Drain the input batch before releasing HTTP: coalescing must not depend on repaint speed.
  await inspect();releaseCoverBatch();
  for(let i=0;i<50;i++){state=await inspect();if(state.coverLoaded&&paths.at(-1)==='/cover-5.png')break;await delay(100);}
  assert.equal(state.coverLoaded,true);assert.equal(paths.at(-1),'/cover-5.png');assert.ok(paths.length-beforeCovers<=2,'rapid cover changes keep one worker and coalesce pending requests');
  await request({type:'clock',position:1200,playing:false,valid:true},'host-state');assert.equal((await inspect()).frameRunning,false);
  const captureState=async name=>{send({type:'capture'});await inspect();for(let i=0;i<50&&!fs.existsSync(capture);i++)await delay(100);assert.equal(fs.readFileSync(capture).readUInt32BE(0),0x89504e47);fs.renameSync(capture,path.join(path.dirname(capture),name+'.png'));};
  await request({type:'test-settings',hover:true,width:360,height:110},'test-settings-ack');
  state=await inspect();assert.equal(state.iconBackend,'vector');assert.equal(state.coverTopPx,(state.windowHeightPx-state.coverSidePx)/2);assert.ok(Number.isInteger(state.coverRadiusPx));
  await captureState('controls-paused-min');
  await request({type:'clock',position:1200,playing:true,valid:true},'host-state');await captureState('controls-playing-min');
  await request({type:'test-settings',hover:true,width:698/(state.dpi/96),height:254/(state.dpi/96)},'test-settings-ack');
  state=await inspect();assert.equal(state.coverTopPx,(state.windowHeightPx-state.coverSidePx)/2);await captureState('controls-resized');assert.ok(state.barRightPx-state.barLeftPx>100);assert.ok(state.nextPx>state.playPx&&state.playPx>state.previousPx);assert.ok(Math.abs(state.nextPx-(state.windowWidthPx-24*(state.dpi/96)))<1); 
  console.log(JSON.stringify({pass:true,backend:state.backend,hardware:state.hardware,unicode:true,cover:true,translation:true,through:true,top:true,fps60:true,staleLyricsRejected:true}));
 }finally{releaseCoverBatch();if(ws?.readyState===1){ws.send(JSON.stringify({type:'exit'}));await delay(150);ws.close();}await new Promise(resolve=>server.close(resolve));}
})().catch(error=>{console.error(error);process.exitCode=1;});
