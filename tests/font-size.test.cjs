const {test}=require('node:test'),assert=require('node:assert/strict');
const {fixture}=require('./bridge-fixture.cjs');
function controls(f){const all=[];function walk(n){all.push(n);for(const c of n.children??[])walk(c)}walk(f.config());return {input:all.find(n=>n.id==='miniLyrics-font-size'),reset:all.find(n=>n.tagName==='button'&&n.textContent==='恢复默认')};}
test('font defaults and stored selection reach the host when opened',async()=>{
 for(const size of [21,14,36,28]){const f=fixture({config:{fontSize:size}});try{assert.equal(controls(f).input.value,String(size));await f.open();assert.equal(f.sent.find(m=>m.type==='font').size,size);}finally{f.dispose()}}
});
test('font edits save immediately, clamp to supported bounds, and reset',async()=>{
 const f=fixture();try{const c=controls(f);c.input.value='28';c.input.onchange();assert.equal(f.savedConfig.fontSize,28);await f.open();assert.equal(f.sent.find(m=>m.type==='font').size,28);
 for(const [value,expected] of [['99',36],['1',14],['23.7',24],['',21],['oops',21]]){c.input.value=value;c.input.onchange();assert.equal(f.savedConfig.fontSize,expected);assert.equal(f.sent.at(-1).size,expected)}
 c.reset.onclick();assert.equal(f.savedConfig.fontSize,21);assert.equal(c.input.value,'21');
 }finally{f.dispose()}
});
test('font preference survives plugin reload and reconnection',async()=>{
 const f=fixture({config:{fontSize:30}});try{await f.open();f.closeSocket();await f.open();assert.equal(f.sent.filter(m=>m.type==='font').at(-1).size,30)}finally{f.dispose()}
 const restored=fixture({config:{fontSize:30}});try{assert.equal(controls(restored).input.value,'30')}finally{restored.dispose()}
});
