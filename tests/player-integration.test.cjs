const {test}=require('node:test'),assert=require('node:assert/strict');
const {fixture}=require('./bridge-fixture.cjs');
test('real adapter drives the widget without InfLink; stale events, seeks and unload are isolated',async()=>{
 const f=fixture({actualPlayer:true});
 const emit=(name,...args)=>{for(const fn of [...f.nativeEvents.get('audioplayer.'+name)??[]])fn(...args);};
 try{
  assert.equal(f.context.InfLinkApi,undefined);await f.open();assert.ok(f.diagnostics().positionMs>=10000&&f.diagnostics().positionMs<11000);assert.equal(f.diagnostics().songId,'local-a');
  emit('PlayState','old-song','end',4);assert.equal(f.diagnostics().playing,true);
  emit('PlayState','local-a','pause',2);assert.equal(f.diagnostics().playing,false);
  f.seek(50000);assert.equal(f.diagnostics().positionMs,10000);assert.equal(f.seeks.at(-1),50000);
  emit('Seek','local-a','seek',0,50);assert.equal(f.diagnostics().positionMs,50000);assert.equal(f.diagnostics().seekPending,false);
  f.setInfo({...f.info(),trackId:'local-b',ncmId:456});emit('PlayProgress','local-a',70);assert.equal(f.diagnostics().songId,'local-b');assert.equal(f.diagnostics().valid,false);
  emit('PlayProgress','local-b',3);assert.equal(f.diagnostics().positionMs,3000);
  const foreign=()=>{};f.nativeEvents.get('audioplayer.PlayProgress').add(foreign);f.dispose();assert.deepEqual([...f.nativeEvents.get('audioplayer.PlayProgress')],[foreign]);
 }finally{f.dispose();}
});

// The bundled plugin must leave native event names free until the first seek
// initializes playback, then acquire progress through its normal sampling loop.
test('fresh player can seek from the widget before original playback starts',async()=>{
 const f=fixture({actualPlayer:true,coldPlayer:true});
 const emit=(name,...args)=>{for(const fn of [...f.nativeEvents.get('audioplayer.'+name)??[]])fn(...args);};
 try{
  await f.open();assert.equal(f.nativeEvents.size,0);assert.equal(f.diagnostics().playing,false);
  f.seek(50000);assert.deepEqual(f.seeks,[50000]);assert.equal(f.nativeInitializations(),1);
  f.advance(1000);emit('Seek','local-a','seek',0,50);emit('PlayState','local-a','play',1);
  assert.equal(f.diagnostics().seekPending,false);assert.equal(f.diagnostics().positionMs,50000);
  emit('PlayProgress','local-a',51);assert.equal(f.diagnostics().positionMs,51000);assert.equal(f.diagnostics().valid,true);
  f.config().children.find(c=>c.textContent==='关闭小窗').onclick();
  await f.open();emit('PlayProgress','local-a',52);assert.equal(f.diagnostics().positionMs,52000);
  assert.equal(f.nativeInitializations(),1);
 }finally{f.dispose();}
});

const emitNative=(f,name,...args)=>{for(const fn of [...f.nativeEvents.get('audioplayer.'+name)??[]])fn(...args);};

test('widget recovers immediately from a missed synchronous first seek without a state notification',async()=>{
 const f=fixture({actualPlayer:true,coldPlayer:true});try{
  await f.open();assert.equal(f.diagnostics().valid,false);assert.equal(f.diagnostics().playerFresh,false);
  f.setSeek(target=>f.emitNativeSeek(target/1000,'early','local-a'));f.seek(50000);
  assert.equal(f.diagnostics().seekPending,true);assert.equal(f.diagnostics().playerDiagnostics.lastAcceptedSeekAt,null);
  emitNative(f,'PlayProgress','local-a',51);
  assert.equal(f.diagnostics().seekPending,false);assert.equal(f.diagnostics().positionMs,51000);assert.equal(f.diagnostics().valid,true);
  assert.equal(f.diagnostics().playerDiagnostics.hasActualProgress,true);assert.deepEqual(f.controls,['play']);
 }finally{f.dispose();}
});

test('a missed acknowledgement with no later authoritative progress times out honestly',async()=>{
 const f=fixture({actualPlayer:true,coldPlayer:true});try{
  await f.open();f.setSeek(target=>f.emitNativeSeek(target/1000,'early','local-a'));f.seek(50000);f.advance(2600);
  const d=f.diagnostics();assert.equal(d.seekPending,false);assert.equal(d.valid,false);assert.equal(d.playerDiagnostics.hasActualProgress,false);
  assert.equal(f.sent.some(m=>m.type==='clock'&&m.seekState==='confirmed'),false);assert.match(d.status,/跳转未确认/);assert.match(d.status,/等待实际进度/);
 }finally{f.dispose();}
});

test('restored position cannot confirm a pending seek; equal actual progress can validate it later',async()=>{
 const f=fixture();try{
  await f.open();f.seek(50000);const notify=f.apiEvents.get('timelineUpdate');
  notify({detail:{songId:'local-a',currentTime:50000,totalTime:90000,sequence:1,source:'restore'}});
  assert.equal(f.diagnostics().seekPending,true);assert.equal(f.diagnostics().valid,false);
  notify({detail:{songId:'local-a',currentTime:50000,totalTime:90000,sequence:2,source:'progress'}});
  assert.equal(f.diagnostics().seekPending,false);assert.equal(f.diagnostics().valid,true);
 }finally{f.dispose();}
});

test('settings distinguish initialization, filtered progress and a stalled stream',async()=>{
 const f=fixture({actualPlayer:true,coldPlayer:true});try{
  await f.open();const status=f.config().children.at(-1);assert.match(status.textContent,/等待播放器初始化/);
  f.seek(50000);f.advance(1000);assert.match(status.textContent,/等待实际播放进度/);
  emitNative(f,'PlayProgress','old-song',50);f.advance(1000);assert.match(status.textContent,/收到进度但未接受/);
  emitNative(f,'PlayState','local-a','play',1);emitNative(f,'PlayProgress','local-a',51);assert.match(status.textContent,/进度正常/);
  f.advance(5000);assert.match(status.textContent,/播放进度已中断/);assert.equal(f.diagnostics().playerDiagnostics.progressAgeMs,5000);
 }finally{f.dispose();}
});
