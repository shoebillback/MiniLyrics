const {test}=require('node:test'),assert=require('node:assert/strict');
const {fixture}=require('./bridge-fixture.cjs');
const plain=value=>JSON.parse(JSON.stringify(value));
function controls(f){const all=[];const walk=n=>{all.push(n);for(const c of n.children??[])walk(c);};walk(f.config());return Object.fromEntries(all.filter(n=>n.id).map(n=>[n.id.replace('miniLyrics-background-',''),n]));}
test('background defaults enable the native Lyricify background and are sent on connection',async()=>{
 const f=fixture();try{await f.open();assert.deepEqual(f.sent.find(m=>m.type==='background'),{type:'background',mode:1,animated:true,speed:1,fps:30});const c=controls(f);assert.deepEqual(c.fps.children.map(n=>n.value),['10','15','30','45','60']);assert.equal(c.mode.children.length,4);assert.equal(c.animated.disabled,false);assert.equal(c.speed.disabled,false);}finally{f.dispose();}
});
test('configuration page saves all four background controls before opening the widget',async()=>{
 const f=fixture();try{const c=controls(f);c.mode.value='2';c.mode.onchange();assert.equal(c.animated.disabled,false);assert.equal(c.speed.disabled,false);c.speed.value='2';c.speed.onchange();c.fps.value='45';c.fps.onchange();assert.equal(f.savedConfig.background.fps,45);c.fps.value='60';c.fps.onchange();c.animated.checked=false;c.animated.onchange();assert.equal(c.speed.disabled,true);assert.equal(c.fps.disabled,true);
  assert.deepEqual(f.savedConfig.background,{mode:2,animated:false,speed:2,fps:60});assert.deepEqual(plain(f.diagnostics().background),f.savedConfig.background);assert.equal(f.sent.length,0);await f.open();assert.deepEqual(f.sent.find(m=>m.type==='background'),{type:'background',...f.savedConfig.background});
 }finally{f.dispose();}
});
test('live background changes apply immediately and survive reconnect and reloading configuration',async()=>{
 const f=fixture({config:{background:{mode:1,animated:true,speed:.5,fps:10}}});let saved;
 try{await f.open();const c=controls(f);assert.equal(c.speed.value,'0.5');c.mode.value='3';c.mode.onchange();assert.deepEqual(f.sent.at(-1),{type:'background',mode:3,animated:true,speed:.5,fps:10});f.hostMessage({type:'background-state',background:{mode:3},lyricCount:2});assert.equal(f.diagnostics().host.background.mode,3);
  f.closeSocket();await f.open();assert.deepEqual(f.sent.filter(m=>m.type==='background').at(-1),{type:'background',mode:3,animated:true,speed:.5,fps:10});saved=plain(f.savedConfig);
 }finally{f.dispose();}
 const restored=fixture({config:saved});try{const c=controls(restored);assert.equal(c.mode.value,'3');assert.equal(c.speed.value,'0.5');assert.equal(c.fps.value,'10');assert.equal(c.animated.checked,true);}finally{restored.dispose();}
});
test('malformed background settings fall back to supported choices',()=>{
 const f=fixture({config:{background:{mode:99,animated:'false',speed:Infinity,fps:1}}});try{assert.deepEqual(plain(f.diagnostics().background),{mode:1,animated:true,speed:1,fps:30});}finally{f.dispose();}
});
test('upgrade selects Lyricify once and preserves subsequent user choices',()=>{
 const old=fixture({config:{background:{mode:0,animated:false,speed:2,fps:10}}});let saved;
 try{assert.deepEqual(plain(old.diagnostics().background),{mode:1,animated:true,speed:2,fps:10});const c=controls(old);c.mode.value='0';c.mode.onchange();saved=plain(old.savedConfig);}finally{old.dispose();}
 const restored=fixture({config:saved});try{assert.equal(restored.diagnostics().background.mode,0);assert.equal(restored.savedConfig.backgroundRenderer,'lyricify-v1');}finally{restored.dispose();}
});
