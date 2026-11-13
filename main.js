(function(){
const MINILYRICS_VERSION="0.6.33";
/* MiniLyrics, GPL-3.0-or-later. Pure normalization shared by plugin and tests. */
(function (root) {
  'use strict';
  const finite = (x, fallback = 0) => Number.isFinite(Number(x)) ? Number(x) : fallback;
  function normalizeLines(input) {
    if (!Array.isArray(input)) return [];
    return input.slice(0, 5000).filter(l => l && typeof l==='object').map(l => {
      const time = Math.max(0, finite(l.time ?? l.startTime ?? l.start));
      const text = String(l.originalLyric ?? l.lyric ?? l.text ?? l.content ?? '');
      const words = (Array.isArray(l.dynamicLyric) ? l.dynamicLyric : []).slice(0, 500).filter(w => w && typeof w==='object').map(w => {
        const t = finite(w.time);
        return {text: String(w.word ?? w.text ?? ''), time: t >= time - 1000 ? t : time + t,
          duration: Math.max(1, finite(w.duration, 1))};
      });
      return {time, duration: Math.max(0, finite(l.duration)), text, isInterlude: !!l.isInterlude || text === '',
        translation: String(l.translatedLyric ?? l.translation ?? ''), words};
    }).sort((a,b) => a.time-b.time);
  }
  function metadata(info) {
    if(!info || typeof info!=='object') return null;
    const ncmId=finite(info.ncmId);
    const id=String(info.trackId??(ncmId>0?ncmId:''));
    if(!id)return null;
    return {id,ncmId:ncmId>0?String(ncmId):'',title:String(info.songName??''),
      artist:String(info.authorName??''),cover:String(info.cover?.url??''),
      duration:Math.max(0,finite(info.duration))};
  }
  // Reconcile a stale provider pause flag only after sustained, real-time progress.
  // A seek sample cannot establish motion, and every explicit state event starts fresh.
  function createPlaybackState(){
    let reported=null,output=false,inferred=false,point=null,run=null,lastMotionAt=-Infinity;
    const parse=value=>{
      if(value===true||value===false)return value;
      if(value==null)return null;
      const text=String(value).toLowerCase();
      return text==='playing'?true:['paused','stopped','ended'].includes(text)?false:null;
    };
    function reset(){reported=null;output=false;inferred=false;point=null;run=null;lastMotionAt=-Infinity;}
    function report(value,force=false){
      const next=parse(value);
      if(next!==null&&(force||next!==reported)){
        reported=next;output=next;inferred=false;point=null;run=null;lastMotionAt=-Infinity;
      }
      return output;
    }
    function sample(position,now,status,discontinuous=false){
      report(status);
      const next={position,at:now};
      if(reported===true){point=next;run=null;output=true;return output;}
      if(discontinuous){point=next;run=null;}
      else if(!point)point=next;
      else if(position!==point.position){
        const delta=position-point.position,elapsed=now-point.at;
        const continuous=elapsed>=100&&elapsed<=2500&&delta>0&&delta<=1500
          &&Math.abs(delta-elapsed)<=Math.max(250,elapsed*.25);
        if(continuous){
          if(!run)run={at:point.at,steps:0};
          run.steps++;
          if(run.steps>=2&&now-run.at>=800)inferred=true;
          if(inferred)lastMotionAt=now;
        }else run=null;
        point=next;
      }
      if(inferred&&now-lastMotionAt>=2000){inferred=false;run=null;}
      output=inferred;return output;
    }
    return {sample,event:value=>report(value,true),reset,resetProgress:()=>{point=null;run=null;},
      current:()=>output,snapshot:()=>({reportedPlaying:reported,playing:output,inferredFromProgress:inferred,
        progressSamples:run?.steps??0})};
  }
  const api={finite, normalizeLines, metadata, createPlaybackState};
  if(typeof module!=='undefined') module.exports=api; else root.MiniLyricsCore=api;
})(typeof window!=='undefined'?window:globalThis);

/* MiniLyrics — GPL-3.0-or-later. */
(() => {
  'use strict';
  window.__miniLyrics?.dispose();
  const C=window.MiniLyricsCore,listeners=[],rnpCache=new Map(),playback=C.createPlaybackState();
  const STALE_MS=4500,SEEK_TIMEOUT_MS=2500,SEEK_TOLERANCE_MS=1500;
  let socket=null,timer=null,stopped=false,session=0,busy=false;
  let windowSession=null,reconnectTimer=null;
  let infLink=null,detachInfLink=null,song={id:'',ncmId:'',duration:0},lines=[];
  let position=0,anchor=performance.now(),playing=false,clockValid=false,offset=0;
  let infPosition=null,infPlaying=null,infAdvancedAt=0,hostState=null;
  let lastMeta='',lastLyrics='',lyricRevision=0,lastRnp=null,lastAppliedRnp=null;
  let pendingSeek=null,seekTimer=null,lastSeekId=0;
  let status='小窗未启动',statusView=null,lastRnpStyle='';
  const normalizeFontSize=value=>Number.isFinite(Number(value))&&value!==''?Math.max(14,Math.min(36,Math.round(Number(value)))):21;
  let fontSize=21;try{fontSize=normalizeFontSize(plugin.getConfig?.('fontSize',21));}catch{}
  function sendFont(){send({type:'font',size:fontSize});}
  function changeFontSize(value){fontSize=normalizeFontSize(value);try{plugin.setConfig?.('fontSize',fontSize);}catch(e){console.warn('[MiniLyrics] 保存字号失败',e);}sendFont();}
  function normalizeBackground(value){
    const v=value&&typeof value==='object'?value:{},mode=Number(v.mode),speed=Number(v.speed),fps=Number(v.fps);
    return {mode:Number.isInteger(mode)&&mode>=0&&mode<=3?mode:1,animated:typeof v.animated==='boolean'?v.animated:true,
      speed:[.5,1,2].includes(speed)?speed:1,fps:[10,15,30,45,60].includes(fps)?fps:60};
  }
  let background;try{
    background=normalizeBackground(plugin.getConfig?.('background',{}));
    if(plugin.getConfig?.('backgroundRenderer','')!=='lyricify-v1'){
      background.mode=1;background.animated=true;
      plugin.setConfig?.('background',{...background});plugin.setConfig?.('backgroundRenderer','lyricify-v1');
    }
  }catch{background=normalizeBackground();}
  function sendBackground(){send({type:'background',...background});}
  function changeBackground(patch){background=normalizeBackground({...background,...patch});try{plugin.setConfig?.('background',{...background});}catch(e){console.warn('[MiniLyrics] 背景设置保存失败',e);}sendBackground();}
  const listen=(target,name,fn)=>{target.addEventListener(name,fn);listeners.push([target,name,fn]);};
  const send=m=>{if(socket?.readyState!==1)return;try{socket.send(JSON.stringify(m));}catch(e){console.debug('[MiniLyrics] 小窗消息发送失败',e);socket?.close();}};
  const live=()=>position+(playing&&clockValid?Math.max(0,performance.now()-anchor):0);
  function providerId(d){
    if(d?.songID!=null||d?.songId!=null)return String(d.songID??d.songId);
    return String(d?.hash??'').match(/^(.*)-\d+$/)?.[1]??'';
  }
  function aliases(){return new Set([song.id,song.ncmId].filter(x=>x&&x!=='0'));}
  function cacheRnp(d){
    const id=providerId(d);if(!id||!Array.isArray(d?.lyrics))return;
    rnpCache.delete(id);rnpCache.set(id,d);
    while(rnpCache.size>4)rnpCache.delete(rnpCache.keys().next().value);
  }
  function setLyrics(d,force=false){
    if(d===lastAppliedRnp&&!force)return;
    if(!aliases().has(providerId(d))||!Array.isArray(d?.lyrics))return;
    const normalized=C.normalizeLines(d.lyrics),signature=JSON.stringify(normalized);lastAppliedRnp=d;
    if(signature===lastLyrics)return;
    lines=normalized;lastLyrics=signature;
    send({type:'lyrics',songId:song.id,revision:++lyricRevision,lines});refreshStatus();
  }
  function recoverLyrics(update){
    const snapshot=window.currentLyrics;
    if(snapshot!==lastRnp){lastRnp=snapshot;cacheRnp(snapshot);}
    if(update&&Array.isArray(update.lyrics)&&aliases().has(providerId(update))){setLyrics(update,true);return;}
    if(aliases().has(providerId(snapshot))){setLyrics(snapshot);return;}
    for(const [id,d]of [...rnpCache].reverse())if(aliases().has(id)){setLyrics(d);return;}
  }
  function sendRnpStyle(force=false){
    const classes=document.body?.classList,names=['smooth','sharp','lazy','easeout'];let curve=0;
    for(let i=1;i<names.length;i++)if(classes?.contains('rnp-lyric-animation-timing-'+names[i]))curve=i;
    let stagger=true;try{const value=window.localStorage?.getItem('refined-now-playing-lyric-stagger');stagger=value!=='false'&&value!=='0';}catch{}
    const signature=curve+':'+stagger;if(force||signature!==lastRnpStyle){lastRnpStyle=signature;send({type:'style',curve,stagger});}
  }
  function clearSeek(){clearTimeout(seekTimer);seekTimer=null;pendingSeek=null;}
  function syncSong(){
    let info;try{info=infLink?.getCurrentSong?.();}catch{return false;}
    const next=C.metadata(info);if(!next)return false;
    if(next.id!==song.id){
      clearSeek();lastSeekId=0;playback.reset();
      position=0;anchor=performance.now();clockValid=false;
      infPosition=null;infPlaying=null;lines=[];lastLyrics='[]';lastAppliedRnp=null;hostState=null;
    }
    song=next;const signature=JSON.stringify(song);
    if(signature!==lastMeta){lastMeta=signature;send({type:'track',...song});}
    return true;
  }
  function sendClock(seekState=''){
    send({type:'clock',songId:song.id,position:live(),playing,valid:clockValid,
      duration:song.duration,offset,seekRequest:lastSeekId,seekState});refreshStatus();
  }
  function infTimelineFresh(){
    return !!infLink&&infPosition!==null&&(infPlaying===false||performance.now()-infAdvancedAt<STALE_MS);
  }
  function acceptTimeline(t,api,playbackStatus){
    if(api!==infLink||!song.id||t?.currentTime==null||!Number.isFinite(Number(t.currentTime)))return false;
    const next=Math.max(0,Number(t.currentTime)),now=performance.now();
    const state=playbackStatus===undefined?api.getPlaybackStatus?.():playbackStatus;
    const nextPlaying=playback.sample(next,now,state,!!pendingSeek);
    let seekState=pendingSeek?'pending':'';
    if(pendingSeek){
      const expected=pendingSeek.target+(nextPlaying?Math.max(0,now-pendingSeek.started):0);
      // Actual position is authoritative, including an external seek that supersedes ours.
      if(Math.abs(next-expected)<=250||Math.abs(next-live())>SEEK_TOLERANCE_MS){clearSeek();seekState='confirmed';}
    }
    if(next===infPosition&&nextPlaying===infPlaying&&seekState!=='confirmed')return false;
    infPosition=next;infPlaying=nextPlaying;infAdvancedAt=now;
    position=next;anchor=now;playing=nextPlaying;clockValid=true;status='已连接';
    if(Number(t.totalTime)>0)song.duration=Number(t.totalTime);
    sendClock(seekState);return true;
  }
  function sample(update,playbackStatus){
    if(infLink)syncSong();recoverLyrics(update);
    if(!infLink)return false;
    try{return acceptTimeline(infLink.getTimeline(),infLink,playbackStatus);}catch(e){console.debug('[MiniLyrics] InfLink 时间轴暂不可用',e);return false;}
  }
  function failSeek(own,reason){
    if(pendingSeek!==own)return;
    clearSeek();playback.resetProgress();
    try{
      const t=infLink?.getTimeline?.();
      if(t?.currentTime!=null&&Number.isFinite(Number(t.currentTime))){
        position=Math.max(0,Number(t.currentTime));anchor=performance.now();
        infPosition=position;infAdvancedAt=anchor;clockValid=true;
      }else{position=own.previous;anchor=performance.now();clockValid=false;}
    }catch{position=own.previous;anchor=performance.now();clockValid=false;}
    status=reason;sendClock('failed');
  }
  function seek(ms,requestId,requestSong){
    syncSong();recoverLyrics();if(requestSong&&requestSong!==song.id)return;
    const target=Math.max(0,Math.min(song.duration||Infinity,Number(ms)));
    if(!Number.isFinite(target))return;
    lastSeekId=Number(requestId)||lastSeekId+1;
    clearSeek();playback.resetProgress();
    const own={id:lastSeekId,songId:song.id,target,started:performance.now(),previous:live()};pendingSeek=own;
    seekTimer=setTimeout(()=>failSeek(own,'跳转未确认，已恢复进度'),SEEK_TIMEOUT_MS);
    sendClock('pending');
    try{
      if(typeof infLink?.seekTo!=='function')throw Error('InfLink 未就绪');
      const result=infLink.seekTo(target);
      if(result===false)failSeek(own,'InfLink 未接受跳转');
      else if(result?.then)result.then(value=>{if(value===false)failSeek(own,'InfLink 未接受跳转');},
        ()=>failSeek(own,'InfLink 跳转失败'));
    }catch{failSeek(own,'InfLink 跳转失败');}
  }
  function command(m){
    if(m.type!=='command')return;
    if(m.action==='seek')return seek(m.position,m.requestId,m.songId);
    if(m.action==='play'){const action=playing?'pause':'play';if(action==='pause')playback.event('Paused');infLink?.[action]?.();return;}
    if(m.action==='previous'||m.action==='next')infLink?.[m.action]?.();
  }
  function attachInfLink(){
    const candidate=window.InfLinkApi,api=typeof candidate?.getCurrentSong==='function'&&typeof candidate?.getTimeline==='function'?candidate:null;
    if(api===infLink){if(!api)status='等待 InfLink-rs';return;}
    detachInfLink?.();detachInfLink=null;playback.reset();
    const wasPending=pendingSeek;
    infLink=api;
    if(wasPending)failSeek(wasPending,'InfLink 连接变化，跳转已取消');
    position=live();anchor=performance.now();playing=false;clockValid=false;
    infPosition=null;infPlaying=null;
    if(!infLink){status='等待 InfLink-rs';sendClock();return;}
    const onTimeline=e=>{if(infLink!==api)return;syncSong();recoverLyrics();acceptTimeline(e.detail,api);};
    const onSong=()=>{if(infLink===api)sample();};
    const onPlaying=e=>{
      if(infLink!==api)return;
      position=live();anchor=performance.now();playback.event(e.detail);
      if(!sample(undefined,e.detail)){playing=playback.current();sendClock(pendingSeek?'pending':'');}
    };
    const bindings=[['timelineUpdate',onTimeline],['songChange',onSong],['playStateChange',onPlaying]];
    for(const [name,fn]of bindings)api.addEventListener?.(name,fn);
    detachInfLink=()=>{for(const [name,fn]of bindings)api.removeEventListener?.(name,fn);};
    status='已连接';
  }
  function tick(){
    if(stopped||socket?.readyState!==1)return;
    try{
      attachInfLink();sample();sendRnpStyle();
      if(playing&&clockValid&&!pendingSeek&&!infTimelineFresh()){
        position=live();anchor=performance.now();clockValid=false;status='等待 InfLink 播放进度';sendClock();
      }
    }catch(e){console.debug('[MiniLyrics] 状态同步暂不可用',e);}
    finally{
      send({type:'ping'});
      if(!stopped&&socket?.readyState===1){clearTimeout(timer);timer=setTimeout(tick,1000);}
    }
  }
  function resyncPage(){
    if(stopped)return;
    if(windowSession&&socket?.readyState!==1){queueReconnect(windowSession,0);return;}
    try{attachInfLink();sample();}catch(e){console.debug('[MiniLyrics] 恢复同步失败',e);}
  }
  function refreshStatus(){
    if(!statusView)return;const text=status+' · 进度：'+(infLink?'InfLink':'等待 InfLink')+' · '+Math.floor(live()/1000)+' 秒 · RNP 歌词 '+lines.length+' 行 · 小窗确认：'+(hostState?.lyricCount??'待回报');if(statusView.textContent!==text)statusView.textContent=text;
  }
  function connectSocket(port){
    const ws=new WebSocket(`ws://127.0.0.1:${port}/`);
    return new Promise(resolve=>{
      let settled=false;
      const finish=ok=>{if(settled)return;settled=true;clearTimeout(timeout);if(!ok)ws.close();resolve(ok?ws:null);};
      const timeout=setTimeout(()=>finish(false),800);
      ws.onopen=()=>finish(true);ws.onerror=()=>finish(false);ws.onclose=()=>finish(false);
    });
  }
  function retireSocket(ws,token){
    if(!ws)return;
    if(ws.readyState===1){ws.send(JSON.stringify({type:'hello',token}));ws.send(JSON.stringify({type:'exit'}));}
    ws.close();
  }
  async function retireHost(port,token){
    await new Promise(r=>setTimeout(r,200));
    try{retireSocket(await connectSocket(port),token);}catch{}
  }
  const ownsWindow=own=>!stopped&&windowSession===own&&own.session===session;
  function rememberWindow(own){
    try{window.sessionStorage?.setItem('MiniLyrics.window',JSON.stringify({port:own.port,token:own.token}));}catch{}
  }
  function forgetWindow(){try{window.sessionStorage?.removeItem('MiniLyrics.window');}catch{}}
  function releaseConnection(){
    clearTimeout(timer);timer=null;clearSeek();playback.reset();
    position=live();anchor=performance.now();playing=false;clockValid=false;hostState=null;
    detachInfLink?.();detachInfLink=null;infLink=null;infPosition=null;infPlaying=null;
  }
  function windowClosed(own){
    if(!ownsWindow(own))return;
    windowSession=null;session++;busy=false;clearTimeout(reconnectTimer);reconnectTimer=null;forgetWindow();
    const previous=socket;socket=null;releaseConnection();previous?.close();
    status='小窗已关闭';refreshStatus();
  }
  function bindSocket(ws,own){
    if(!ownsWindow(own)){if(stopped)ws.close();else retireSocket(ws,own.token);return;}
    // Recover the actual snapshot before initializing the same host again.
    try{attachInfLink();sample();}catch(e){console.debug('[MiniLyrics] 等待播放状态恢复',e);}
    socket=ws;
    ws.onmessage=e=>{
      if(!ownsWindow(own)||socket!==ws)return;
      try{
        const m=JSON.parse(e.data);
        if(m.type==='lyrics-ack'||m.type==='host-state'||m.type==='background-state'){hostState=m;refreshStatus();}else command(m);
      }catch(err){console.debug('[MiniLyrics]',err);}
    };
    ws.onclose=e=>{
      if(!ownsWindow(own)||socket!==ws)return;
      if(e?.code===1000&&e?.reason==='host-closed'){windowClosed(own);return;}
      socket=null;releaseConnection();status='小窗连接中断，等待恢复';refreshStatus();queueReconnect(own);
    };
    rememberWindow(own);
    send({type:'hello',token:own.token});
    if(song.id){
      send({type:'track',...song});
      if(lastAppliedRnp)send({type:'lyrics',songId:song.id,revision:++lyricRevision,lines});
      if(clockValid)sendClock();
    }
    sendFont();sendRnpStyle(true);sendBackground();
    status='已连接';tick();
  }
  function queueReconnect(own,delay=1000){
    if(!ownsWindow(own)||own.connecting||socket?.readyState===1)return;
    clearTimeout(reconnectTimer);
    reconnectTimer=setTimeout(async()=>{
      reconnectTimer=null;if(!ownsWindow(own)||socket?.readyState===1)return;
      own.connecting=true;let connected=false;
      try{
        const ws=await connectSocket(own.port);
        if(!ownsWindow(own)){if(stopped)ws?.close();else retireSocket(ws,own.token);return;}
        if(ws){bindSocket(ws,own);connected=socket===ws&&ws.readyState===1;}
      }catch(e){console.debug('[MiniLyrics] 等待小窗重新连接',e);}
      finally{own.connecting=false;if(ownsWindow(own)&&!connected)queueReconnect(own,2000);}
    },delay);
  }
  async function open(){
    if(stopped||busy)return;
    if(socket?.readyState===1){send({type:'show'});return;}
    if(windowSession){queueReconnect(windowSession,0);return;}
    busy=true;const ownSession=++session;
    const current=()=>!stopped&&ownSession===session;
    try{
      const bytes=new Uint8Array(24);crypto.getRandomValues(bytes);
      const token=Array.from(bytes,b=>b.toString(16).padStart(2,'0')).join('');
      const pluginPath=String(plugin.pluginPath??loadedPlugins.MiniLyrics?.pluginPath??'').replace(/\//g,'\\').replace(/\\\.\\/g,'\\');
      if(!pluginPath||/["\r\n]/.test(pluginPath))throw new Error('无法确定插件路径');
      const port=42000+Math.floor(Math.random()*6000),own={port,token,session:ownSession,connecting:true,launched:false};
      windowSession=own;
      const ok=await betterncm.app.exec('"'+pluginPath+'\\MiniLyricsHost.exe" --port '+port+' --token '+token,false,false);
      if(!current()){if(ok!==false&&!stopped)await retireHost(port,token);return;}
      if(ok===false)throw new Error('原生窗口启动失败');
      own.launched=true;rememberWindow(own);
      for(let attempt=0;attempt<15;attempt++){
        await new Promise(r=>setTimeout(r,200));
        if(!current()){if(!stopped)await retireHost(port,token);return;}
        const ws=await connectSocket(port);
        if(!current()){if(ws){if(stopped)ws.close();else retireSocket(ws,token);}return;}
        if(!ws)continue;
        own.connecting=false;bindSocket(ws,own);return;
      }
      own.connecting=false;status='小窗等待连接';refreshStatus();queueReconnect(own);
    }catch(e){
      if(current()){
        const own=windowSession;
        if(own?.launched){
          own.connecting=false;status='小窗等待恢复连接';queueReconnect(own);
        }else{windowSession=null;forgetWindow();status=String(e.message??e);}
        refreshStatus();console.error('[MiniLyrics]',e);
      }
    }finally{if(ownSession===session)busy=false;}
  }
  function closeWindow(){
    const own=windowSession,connected=socket?.readyState===1;
    session++;windowSession=null;busy=false;clearTimeout(reconnectTimer);reconnectTimer=null;forgetWindow();
    send({type:'exit'});const previous=socket;socket=null;releaseConnection();previous?.close();
    if(own&&!connected)void retireHost(own.port,own.token);
    status='小窗已关闭';refreshStatus();
  }
  function dispose(){
    // Page reload/unload is temporary bridge loss, not a user close request.
    if(windowSession)rememberWindow(windowSession);
    stopped=true;session++;busy=false;clearTimeout(reconnectTimer);reconnectTimer=null;
    const previous=socket;socket=null;releaseConnection();previous?.close();
    for(const [target,name,fn]of listeners)target.removeEventListener?.(name,fn);
  }
  listen(document,'lyrics-updated',e=>{if(stopped)return;const update=e.detail??window.currentLyrics;cacheRnp(update);attachInfLink();sample(update);});
  // RNP persists this setting before dispatching the event. Share the reader
  // and signature with periodic recovery so the same style is sent only once.
  listen(document,'rnp-lyric-stagger',()=>sendRnpStyle());
  listen(document,'rnp-global-offset',e=>{offset=C.finite(e.detail);sendClock();});
  plugin.onLoad(()=>{
    try{offset=C.finite(window.localStorage?.getItem('refined-now-playing-lyric-offset'));}catch{}
    try{attachInfLink();sample();}catch(e){console.debug('[MiniLyrics] 等待播放信息',e);}
    try{
      const saved=JSON.parse(window.sessionStorage?.getItem('MiniLyrics.window')??'null');
      if(saved&&Number.isInteger(saved.port)&&saved.port>=42000&&saved.port<48000&&/^[0-9a-f]{48}$/.test(saved.token)){
        windowSession={port:saved.port,token:saved.token,session:++session,connecting:false};queueReconnect(windowSession,0);
      }
    }catch{}
  });
  plugin.onConfig(()=>{
    const el=document.createElement('div');el.style.cssText='padding:24px;line-height:1.8;max-width:700px';
    const title=document.createElement('h2');title.textContent='MiniLyrics 音乐小窗';el.append(title);
    for(const [text,fn]of [['打开 / 找回小窗',open],['退出鼠标穿透',()=>send({type:'unlock'})],['重置位置和尺寸',()=>send({type:'reset'})],['关闭小窗',closeWindow]]){
      const b=document.createElement('button');b.textContent=text;b.style.cssText='padding:8px 14px;margin:0 8px 12px 0';b.onclick=fn;el.append(b);}
    const p=document.createElement('p');p.textContent='拖动封面移动，右下角缩放；滚轮回看歌词，点击歌词跳转。右键小窗可调整翻译、置顶和帧率。';el.append(p);
    const fontHeading=document.createElement('h3');fontHeading.textContent='歌词字体';el.append(fontHeading);
    const fontRow=document.createElement('label');fontRow.style.cssText='display:block;margin:10px 0';fontRow.textContent='字号（14–36）';
    const fontInput=document.createElement('input');fontInput.type='number';fontInput.id='miniLyrics-font-size';fontInput.min='14';fontInput.max='36';fontInput.step='1';fontInput.value=String(fontSize);fontInput.style.cssText='width:72px;padding:5px 8px;margin-left:12px';fontInput.onchange=()=>{changeFontSize(fontInput.value);fontInput.value=String(fontSize);};
    const fontReset=document.createElement('button');fontReset.textContent='恢复默认';fontReset.style.cssText='margin-left:12px;padding:5px 10px';fontReset.onclick=()=>{changeFontSize(21);fontInput.value=String(fontSize);};fontRow.append(fontInput,fontReset);el.append(fontRow);
    const fontTip=document.createElement('p');fontTip.textContent='翻译和前后歌词按比例缩放。';el.append(fontTip);
    const heading=document.createElement('h3');heading.textContent='背景';el.append(heading);
    const makeSelect=(id,choices,value)=>{const control=document.createElement('select');control.id=id;control.style.cssText='padding:5px 8px;margin-left:12px';for(const [v,text]of choices){const option=document.createElement('option');option.value=String(v);option.textContent=text;control.append(option);}control.value=String(value);return control;};
    const addRow=(text,control)=>{const row=document.createElement('label');row.style.cssText='display:block;margin:10px 0';const label=document.createElement('span');label.textContent=text;row.append(label,control);el.append(row);};
    const mode=makeSelect('miniLyrics-background-mode',[[0,'现有渐变'],[1,'Lyricify · 标准'],[2,'Lyricify · 明显流动'],[3,'Lyricify · 暗色']],background.mode);
    const animated=document.createElement('input');animated.id='miniLyrics-background-animated';animated.type='checkbox';animated.checked=background.animated;animated.style.marginLeft='12px';
    const speed=makeSelect('miniLyrics-background-speed',[[.5,'慢速'],[1,'标准'],[2,'快速']],background.speed);
    const rate=makeSelect('miniLyrics-background-fps',[[10,'10 FPS'],[15,'15 FPS'],[30,'30 FPS'],[45,'45 FPS'],[60,'60 FPS（默认）']],background.fps);
    const refreshControls=()=>{animated.disabled=background.mode===0;speed.disabled=rate.disabled=background.mode===0||!background.animated;};
    mode.onchange=()=>{changeBackground({mode:Number(mode.value)});refreshControls();};
    animated.onchange=()=>{changeBackground({animated:animated.checked});refreshControls();};
    speed.onchange=()=>changeBackground({speed:Number(speed.value)});rate.onchange=()=>changeBackground({fps:Number(rate.value)});
    addRow('背景样式',mode);addRow('动态流动',animated);addRow('流动速度',speed);addRow('背景帧率',rate);refreshControls();
    const links=document.createElement('p');
    for(const [label,url]of [['源码与许可证','https://github.com/shoebillback/MiniLyrics'],['反馈问题','https://github.com/shoebillback/MiniLyrics/issues']]){
      const link=document.createElement('a');link.textContent=label;link.href=url;link.style.cssText='margin-right:18px';
      link.onclick=event=>{event.preventDefault();betterncm.ncm.openUrl(url);};links.append(link);
    }
    el.append(links);
    const s=document.createElement('p');statusView=s;refreshStatus();el.append(s);return el;
  });
  window.__miniLyrics={open,dispose,getStatus:()=>status,getDiagnostics:()=>({version:MINILYRICS_VERSION,status,songId:song.id,ncmId:song.ncmId,
    progressSource:infLink?'InfLink':'未同步',positionMs:live(),playing,valid:clockValid,lyricCount:lines.length,
    lyricSource:lines.length?'RNP 歌词':'等待 RNP',lyricRevision,rnpSongId:providerId(window.currentLyrics),
    host:hostState,infLinkVersion:infLink?.version??null,infLinkFresh:infTimelineFresh(),
    seekPending:!!pendingSeek,seekRequest:lastSeekId,playbackState:playback.snapshot(),background:{...background}})};
  listen(window,'rnp-lyric-page-opened',resyncPage);
  listen(window,'rnp-lyric-page-closed',resyncPage);
  listen(window,'focus',resyncPage);
  listen(document,'visibilitychange',resyncPage);
  listen(window,'beforeunload',dispose);
})();

})();