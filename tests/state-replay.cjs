// Local fixtures only; no running music player is accessed.
const http=require('node:http'),fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict'),{spawn}=require('node:child_process');
const {fixture}=require('./bridge-fixture.cjs');
(async()=>{
 const dir=process.argv[3];fs.mkdirSync(dir,{recursive:true});const marker=path.join(dir,'superseded-requested');if(fs.existsSync(marker))fs.unlinkSync(marker);
 const f=fixture();let messages;try{await f.open();const n=f.sent.length;f.emitTimeline(60000);f.advance(100);f.emitTimeline(20000);messages=f.sent.slice(n).filter(m=>m.type==='seek'||m.type==='clock');}finally{f.dispose();}
 const replay=path.join(dir,'replay.json');fs.writeFileSync(replay,JSON.stringify({messages}));
 if(process.argv[4]==='motion'){const child=spawn(process.argv[2],[replay],{stdio:'inherit',windowsHide:true});const code=await new Promise(r=>child.once('exit',r));assert.equal(code,0);return;}
 const bytes=Buffer.alloc(54+16*16*4);bytes.write('BM');bytes.writeUInt32LE(bytes.length,2);bytes.writeUInt32LE(54,10);bytes.writeUInt32LE(40,14);bytes.writeInt32LE(16,18);bytes.writeInt32LE(-16,22);bytes.writeUInt16LE(1,26);bytes.writeUInt16LE(32,28);bytes.fill(255,54);
 const counts={};const server=http.createServer((req,res)=>{const n=counts[req.url]=(counts[req.url]??0)+1;
  if(req.url==='/superseded.bmp'){res.writeHead(503);res.end();fs.writeFileSync(marker,'ready');return;}
  if(req.url==='/always-fail.bmp'||req.url==='/retry.bmp'&&n===1){res.writeHead(503);res.end();return;}
  res.writeHead(200,{'Content-Type':'image/bmp','Content-Length':bytes.length});res.end(bytes);
 });await new Promise(r=>server.listen(0,'127.0.0.1',r));
 try{const child=spawn(process.argv[2],[replay,`http://127.0.0.1:${server.address().port}`,dir],{stdio:'inherit',windowsHide:true});
  const code=await new Promise((resolve,reject)=>{const timer=setTimeout(()=>{child.kill();reject(Error('state replay timed out'));},20000);child.once('error',e=>{clearTimeout(timer);reject(e);});child.once('exit',code=>{clearTimeout(timer);resolve(code);});});
  assert.equal(code,0);assert.equal(counts['/retry.bmp'],2);assert.equal(counts['/superseded.bmp'],1);assert.equal(counts['/new.bmp'],1);assert.equal(counts['/always-fail.bmp'],3);
  fs.writeFileSync(path.join(dir,'result.json'),JSON.stringify({pass:true,counts},null,2));console.log('PASS loopback request counts:',JSON.stringify(counts));
 }finally{await new Promise(r=>server.close(r));}
})().catch(e=>{console.error(e);process.exitCode=1;});
