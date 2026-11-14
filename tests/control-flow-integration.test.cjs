const {test}=require('node:test'),assert=require('node:assert/strict');
const {fixture}=require('./bridge-fixture.cjs');
const emit=(f,name,...args)=>{for(const fn of [...f.nativeEvents.get('audioplayer.'+name)??[]])fn(...args);};

test('control flow bundle: first seek starts normally and bootstrap progress cannot confirm the target',async()=>{
 const f=fixture({actualPlayer:true,coldPlayer:true});try{
  await f.open();f.setSeek(ms=>f.emitNativeSeek(ms/1000,'seek','local-a'));
  f.seek(50000);assert.deepEqual(f.controls,['play']);assert.equal(f.nativeInitializations(),1);assert.equal(f.diagnostics().seekPending,true);
  emit(f,'PlayProgress','local-a',0);assert.equal(f.diagnostics().seekPending,true,'initial playback position is not seek confirmation');
  await Promise.resolve();assert.equal(f.diagnostics().seekPending,false);assert.equal(f.diagnostics().positionMs,50000);
  emit(f,'PlayProgress','local-a',51);assert.equal(f.diagnostics().positionMs,51000);assert.equal(f.diagnostics().valid,true);
 }finally{f.dispose();}
});

test('control flow bundle: toggle uses the original state and loaded paused seeks stay paused',async()=>{
 const f=fixture({actualPlayer:true});try{
  await f.open();emit(f,'PlayState','local-a','pause',2);assert.equal(f.diagnostics().playing,false);
  f.command('play');assert.deepEqual(f.controls,['pause'],'the original model decides while the widget has a stale pause flag');
  f.setSeek(ms=>f.emitNativeSeek(ms/1000,'seek','local-a'));f.seek(30000);
  assert.deepEqual(f.controls,['pause']);assert.equal(f.diagnostics().playing,false);assert.equal(f.diagnostics().positionMs,30000);
 }finally{f.dispose();}
});

test('control flow bundle: close cancels a startup follow-up before delayed initialization finishes',async()=>{
 const f=fixture({actualPlayer:true,coldPlayer:true,deferStartup:true});try{
  await f.open();f.seek(50000);assert.equal(f.diagnostics().seekPending,true);assert.deepEqual(f.seeks,[]);
  f.config().children.find(c=>c.textContent==='关闭小窗').onclick();f.completeNativeStartup();await Promise.resolve();
  assert.deepEqual(f.seeks,[50000],'only the original queued startup position is applied; no later MiniLyrics seek');
 }finally{f.dispose();}
});
