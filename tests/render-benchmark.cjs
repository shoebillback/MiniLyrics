const assert=require('node:assert/strict');
const fs=require('node:fs');
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
(async()=>{
 const port=Number(process.argv[2]),output=process.argv[3],capture=process.argv[4],baseline=process.argv[5]==='baseline';
 let ws,pending;
 for(let i=0;i<30;i++){
  ws=new WebSocket(`ws://127.0.0.1:${port}/`);
  if(await new Promise(resolve=>{ws.onopen=()=>resolve(true);ws.onerror=()=>resolve(false);}))break;
  ws.close();await delay(100);
 }
 assert.equal(ws.readyState,1,'test host must be reachable');
 ws.onmessage=event=>{const message=JSON.parse(event.data);if(message.type==='inspection'&&pending){const resolve=pending;pending=null;resolve(message);}};
 const send=message=>ws.send(JSON.stringify(message));
 const inspect=()=>new Promise((resolve,reject)=>{const timer=setTimeout(()=>{pending=null;reject(Error('native inspection timed out'));},3000);pending=message=>{clearTimeout(timer);resolve(message);};send({type:'inspect'});});
 const clock=(position,playing=true)=>send({type:'clock',position,playing,valid:true,duration:90000});
 const lyrics=lines=>send({type:'lyrics',songId:'benchmark',lines});
 try{
  send({type:'hello',token:'0123456789abcdef0123456789abcdef'});
  send({type:'track',id:'benchmark',title:'Render test',artist:'Isolated fixture',duration:90000});
  lyrics([]);clock(0);await delay(200);const empty=await inspect();
  if(!baseline)assert.equal(empty.frameRunning,false,'empty unhovered widget must not run render timer');

  lyrics([{time:0,duration:60000,text:'Stable row',translation:'Translation',words:[]}]);
  clock(1000);await delay(550);const beforeCorrections=await inspect();
  for(let i=1;i<=6;i++){clock(1000+i*1000);await delay(35);}
  const afterCorrections=await inspect();
  const builds=afterCorrections.lineBuildCalls-beforeCorrections.lineBuildCalls;
  if(!baseline)assert.equal(builds,0,'same-row clock corrections must reuse text nodes');

  lyrics([{time:0,duration:2000,text:'Long scrolling lyric '.repeat(20),words:[]}]);
  clock(20000);await delay(550);const beforeScroll=await inspect();
  await delay(2000);const afterScroll=await inspect();
  const scrollCalls=afterScroll.renderCalls-beforeScroll.renderCalls;
  if(!baseline)assert.ok(scrollCalls<=15,'completed scrolling must return to idle refresh');

  lyrics([{time:0,duration:60000,text:'Paused fixture',translation:'Translation',words:[]}]);
  clock(1000,false);await delay(100);const beforePause=await inspect();
  await delay(400);const afterPause=await inspect();
  assert.equal(afterPause.frameRunning,false);assert.equal(afterPause.renderCalls,beforePause.renderCalls);

  lyrics([{time:1000,duration:3000,text:'Future word',words:[{text:'Future word',time:1000,duration:1000}]}]);
  clock(0);await delay(1150);const future=await inspect();
  assert.equal(future.index,0,'scheduled future row becomes active');assert.ok(future.intervalMs<=34,'active word uses animation refresh');
  clock(1100,false);send({type:'capture'});
  for(let i=0;i<50&&!fs.existsSync(capture);i++)await delay(100);
  assert.equal(fs.readFileSync(capture).readUInt32BE(0),0x89504e47,'Native screenshot is a valid PNG');
  const result={baseline,emptyTimerRunning:empty.frameRunning,sameRowRebuilds:builds,
   completedScrollRenderCalls:scrollCalls,measurementMs:2000,completedScrollIntervalMs:afterScroll.intervalMs,
   pausedRenderCalls:afterPause.renderCalls-beforePause.renderCalls,futureWordIndex:future.index,activeWordIntervalMs:future.intervalMs};
  fs.writeFileSync(output,JSON.stringify(result,null,2));console.log(JSON.stringify(result));
 }finally{send({type:'exit'});await delay(200);ws.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
