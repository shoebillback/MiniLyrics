// Local generated artwork only; no player or external network source.
const http=require('node:http'),{spawn}=require('node:child_process');
function artwork(side){
 const bytes=Buffer.alloc(54+side*side*4);bytes.write('BM');bytes.writeUInt32LE(bytes.length,2);bytes.writeUInt32LE(54,10);bytes.writeUInt32LE(40,14);bytes.writeInt32LE(side,18);bytes.writeInt32LE(-side,22);bytes.writeUInt16LE(1,26);bytes.writeUInt16LE(32,28);
 for(let y=0;y<side;y++)for(let x=0;x<side;x++){const i=54+(y*side+x)*4;bytes[i]=x%256;bytes[i+1]=y%256;bytes[i+2]=(x+y)%256;bytes[i+3]=255;}return bytes;
}
(async()=>{
 const images={'/large.bmp':artwork(1024),'/small.bmp':artwork(64)};
 let requests=0;const server=http.createServer((req,res)=>{requests++;const bytes=images[req.url];if(!bytes){res.writeHead(404);res.end();return;}res.writeHead(200,{'Content-Type':'image/bmp','Content-Length':bytes.length});res.end(bytes);});
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
 try{
  const child=spawn(process.argv[2],[`http://127.0.0.1:${server.address().port}`,process.argv[3]],{stdio:'inherit',windowsHide:true});
  const code=await new Promise((resolve,reject)=>{const timer=setTimeout(()=>{child.kill();reject(Error('cover decode timed out'));},20000);child.once('error',e=>{clearTimeout(timer);reject(e);});child.once('exit',value=>{clearTimeout(timer);resolve(value);});});
  if(code!==0)throw Error('cover regression failed: '+code);
 }finally{console.log('Local cover requests:',requests);await new Promise(resolve=>server.close(resolve));}
})().catch(e=>{console.error(e);process.exitCode=1;});
