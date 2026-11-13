const {test}=require('node:test'),assert=require('node:assert/strict');
const {fixture}=require('./bridge-fixture.cjs');
test('play button retains pause and resume behavior from current playback state',async()=>{
 const f=fixture();try{await f.open();f.command('play');f.emitPlay('Paused');f.command('play');assert.deepEqual(f.controls,['pause','play']);}finally{f.dispose();}
});
