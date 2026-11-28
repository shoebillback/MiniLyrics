/* MiniLyrics — GPL-3.0-or-later. */
(() => {
  'use strict';
  window.__miniLyrics?.dispose();
  let playerAdapter=null;
  const C=window.MiniLyricsCore,lyrics=window.MiniLyricsLyrics,listeners=[],playback=C.createPlaybackState();
  const STALE_MS=4500,SEEK_TIMEOUT_MS=2500,SEEK_TOLERANCE_MS=1500;
  const MAX_RECONNECT_FAILURES=10;
  const LYRIC_TIMEOUT_MS=8000,CLIENT_LYRIC_TIMEOUT_MS=1500,LYRIC_RETRY_DELAYS=[1000,2500],LYRIC_CACHE_LIMIT=24;
  const MORE_ICON_SELECTOR='[aria-label="morefunctions"], [aria-label="ellipsis"], [aria-label="更多"]';
  const VOLUME_ICON_SELECTOR='[class*="cmd-icon-volume"], [data-testid*="volume"], [aria-label="音量"]';
  const PLAYBAR_CONTROL_SELECTOR='button, [role="button"], .cmd-button';
  const lyricCache=new Map();
  const emptySong=()=>({id:'',ncmId:'',lyricId:'',lyricUserId:'',title:'',artist:'',cover:'',duration:0});
  let socket=null,timer=null,stopped=false,session=0,busy=false;
  let windowSession=null,reconnectTimer=null;
  let player=null,detachPlayer=null,song=emptySong(),lastSentAt=0;
  const clock={position:0,anchor:performance.now(),playing:false,valid:false,sample:null,playingSince:0};
  let offset=0,hostState=null,confirmedLyricCount=null;
  let songInfo=null,publishedLyrics=null,lyricRevision=0,lyricTask=null;
  const lyricState={mode:'waiting',error:'',transport:''};
  let pendingSeek=null,lastSeekId=0;
  let status='小窗未启动',statusView=null,retryLyricsButton=null,curve=0,stagger=true,wordMode=0;
  let shortcutButton=null,shortcutStyle=null,shortcutObserver=null,shortcutFrame=null,shortcutAnchor=null;
  const lyricStatus={waiting:'等待曲目',loading:'正在获取歌词',timed:'网易云歌词',untimed:'无时间轴歌词',empty:'暂无歌词',instrumental:'纯音乐',unsupported:'暂无在线歌词',error:'歌词获取失败'};
  function saveConfig(key,value){
    try{plugin.setConfig?.(key,value);return true;}
    catch(e){console.warn('[MiniLyrics] 保存设置失败',key,e);return false;}
  }
  const normalizeOffset=value=>Math.max(-10000,Math.min(10000,Math.round(C.finite(value))));
  const normalizeChoice=(value,min,max,fallback=0)=>
    typeof value==='number'&&Number.isInteger(value)&&value>=min&&value<=max?value:fallback;
  try{
    offset=normalizeOffset(plugin.getConfig?.('lyricOffset',0));
    curve=normalizeChoice(plugin.getConfig?.('lyricCurve',0),0,4,0);
    const rawStagger=plugin.getConfig?.('lyricStagger',true);
    stagger=typeof rawStagger==='boolean'?rawStagger:true;
    wordMode=normalizeChoice(plugin.getConfig?.('lyricWordMode',0),0,1,0);
  }catch{}
  function sendStyle(){send({type:'style',curve,stagger,wordMode});}
  const normalizeFontSize=value=>Number.isFinite(Number(value))&&value!==''?Math.max(14,Math.min(36,Math.round(Number(value)))):21;
  let fontSize=21;try{fontSize=normalizeFontSize(plugin.getConfig?.('fontSize',21));}catch{}
  function sendFont(){send({type:'font',size:fontSize});}
  function changeFontSize(value){fontSize=normalizeFontSize(value);saveConfig('fontSize',fontSize);sendFont();}
  function normalizeBackground(value){
    const v=value&&typeof value==='object'?value:{},mode=Number(v.mode),speed=Number(v.speed),fps=Number(v.fps);
    return {mode:Number.isInteger(mode)&&mode>=0&&mode<=3?mode:1,animated:typeof v.animated==='boolean'?v.animated:true,
      speed:[.5,1,2].includes(speed)?speed:1,fps:[10,15,30,45,60].includes(fps)?fps:60};
  }
  let background;try{
    background=normalizeBackground(plugin.getConfig?.('background',{}));
    if(plugin.getConfig?.('backgroundRenderer','')!=='lyricify-v1'){
      background.mode=1;background.animated=true;
      if(saveConfig('background',{...background}))saveConfig('backgroundRenderer','lyricify-v1');
    }
  }catch{background=normalizeBackground();}
  function sendBackground(){send({type:'background',...background});}
  function changeBackground(patch){background=normalizeBackground({...background,...patch});saveConfig('background',{...background});sendBackground();}
  const listen=(target,name,fn)=>{target.addEventListener(name,fn);listeners.push([target,name,fn]);};
  const send=m=>{if(socket?.readyState!==1)return;try{socket.send(JSON.stringify(m,(_,v)=>typeof v==='string'?C.wellFormed(v):v));lastSentAt=performance.now();}catch(e){console.debug('[MiniLyrics] 小窗消息发送失败',e);socket?.close();}};
  const live=()=>clock.position+(clock.playing&&clock.valid?Math.max(0,performance.now()-clock.anchor):0);
  function resetClock(pos=0){
    clock.position=pos;clock.anchor=performance.now();clock.playing=false;clock.valid=false;clock.sample=null;clock.playingSince=0;
  }
  function sendTrack(){send({type:'track',id:song.id,title:song.title,artist:song.artist,cover:song.cover,duration:song.duration});}
  function sendLyrics(){if(publishedLyrics)send({type:'lyrics',songId:song.id,revision:publishedLyrics.revision,mode:publishedLyrics.mode,lines:publishedLyrics.lines});}
  function publishLyrics(result){
    publishedLyrics={mode:result.mode,lines:result.lines,revision:++lyricRevision};
    confirmedLyricCount=null;lyricState.mode=result.mode;
    sendLyrics();refreshStatus();
  }
  function cancelLyrics(){lyricTask?.cancel();lyricTask=null;}
  function resetLyrics(){
    cancelLyrics();
    publishedLyrics=null;confirmedLyricCount=null;
    lyricState.mode='waiting';lyricState.error='';lyricState.transport='';
  }
  function resetWindowSong(){
    resetLyrics();songInfo=null;song=emptySong();
  }
  function cacheLyrics(key,result){
    lyricCache.delete(key);
    lyricCache.set(key,result);
    if(lyricCache.size>LYRIC_CACHE_LIMIT)lyricCache.delete(lyricCache.keys().next().value);
  }
  function createLyricTask(id,userId,key,transport){
    const task={
      timer:null,controller:null,checkedClient:false,
      active:()=>lyricTask===task,
      setTimer(fn,ms){clearTimeout(task.timer);task.timer=setTimeout(fn,ms);},
      cancel(){
        clearTimeout(task.timer);task.timer=null;
        const ctrl=task.controller;task.controller=null;
        ctrl?.abort();
      },
      commit(result,source){
        clearTimeout(task.timer);task.timer=null;task.controller=null;
        if(lyricTask===task)lyricTask=null;
        lyricState.error='';lyricState.transport=source;
        cacheLyrics(key,result);publishLyrics(result);
      },
      tryClientCache(onMiss){
        let lookup;
        if(!userId&&!task.checkedClient){
          task.checkedClient=true;
          try{lookup=player?.getCachedLyrics?.(id);}catch{}
        }
        if(!lookup){onMiss();return;}
        let finished=false;
        function finish(payload){
          if(finished||!task.active())return;
          finished=true;clearTimeout(task.timer);task.timer=null;
          let result;
          if(payload)try{result=lyrics.parseLyrics(payload);}catch{}
          if(result&&result.mode!=='empty'){task.commit(result,'client-cache');return;}
          onMiss();
        }
        task.setTimer(()=>finish(null),CLIENT_LYRIC_TIMEOUT_MS);
        Promise.resolve(lookup).then(finish,()=>finish(null));
      },
      startAttempt(index){
        if(!task.active())return;
        const controller=new AbortController();task.controller=controller;
        const current=()=>task.active()&&task.controller===controller;
        const onFail=(error,timedOut=false)=>{
          if(!current())return;
          clearTimeout(task.timer);task.timer=null;task.controller=null;
          if(timedOut)controller.abort();
          lyricState.error=String(error?.message??error);
          task.tryClientCache(()=>{
            if(!task.active())return;
            const delay=LYRIC_RETRY_DELAYS[index];
            if(delay!==undefined&&error?.retryable!==false&&error?.name!=='AbortError'){
              console.debug('[MiniLyrics] 歌词请求失败，将重试',id,lyricState.error);
              task.setTimer(()=>task.startAttempt(index+1),delay);
              if(!publishedLyrics)publishLyrics({mode:'loading',lines:[]});
              else refreshStatus();
              return;
            }
            lyricTask=null;console.warn('[MiniLyrics] 歌词获取失败',id,lyricState.error);
            publishLyrics({mode:'error',lines:[]});
          });
        };
        task.setTimer(()=>onFail(new Error('歌词请求超时（8 秒）'),true),LYRIC_TIMEOUT_MS);
        lyrics.fetchLyrics(id,{signal:controller.signal,userId}).then(result=>{
          if(!current())return;
          clearTimeout(task.timer);task.timer=null;task.controller=null;
          if(result.mode==='empty')task.tryClientCache(()=>{if(task.active())task.commit(result,transport);});
          else task.commit(result,transport);
        },onFail);
      }
    };
    return task;
  }
  function loadLyrics(clearHost=false){
    if(stopped||!windowSession)return;
    cancelLyrics();
    const id=song.lyricId,userId=song.lyricUserId;
    if(!id){publishLyrics({mode:'unsupported',lines:[]});return;}
    const key=userId?'cloud:'+userId+':'+id:id;
    const transport=userId?'cloud':'direct';
    const cached=lyricCache.get(key);
    if(cached){cacheLyrics(key,cached);lyricState.error='';lyricState.transport='memory';publishLyrics(cached);return;}
    if(clearHost&&publishedLyrics?.mode!=='loading')publishLyrics({mode:'loading',lines:[]});
    else{lyricState.mode='loading';refreshStatus();}
    const task=createLyricTask(id,userId,key,transport);
    lyricTask=task;
    task.startAttempt(0);
  }
  function syncSong(){
    let info;try{info=player?.getCurrentSong?.();}catch{return false;}
    if(info&&info===songInfo)return true;
    const next=C.metadata(info);if(!next)return false;
    songInfo=info;
    if(Object.keys(next).every(key=>next[key]===song[key]))return true;
    const newTrack=next.id!==song.id,newLyrics=newTrack||next.lyricId!==song.lyricId||next.lyricUserId!==song.lyricUserId;
    if(newTrack){
      finishSeek(pendingSeek,'cancelled',{resetStart:true});lastSeekId=0;playback.reset();
      resetClock(0);hostState=null;confirmedLyricCount=null;
    }
    if(newLyrics)resetLyrics();
    song=next;sendTrack();
    if(newLyrics)loadLyrics(!newTrack);
    return true;
  }
  function sendClock(seekState=''){
    send({type:'clock',songId:song.id,position:live(),playing:clock.playing,valid:clock.valid,
      duration:song.duration,offset,seekRequest:lastSeekId,seekState});refreshStatus();
  }
  function playerTimelineFresh(){
    if(!player||!clock.valid||!clock.sample)return false;
    if(clock.sample.playing===false)return true;
    return performance.now()-Math.max(clock.sample.at,clock.playingSince)<STALE_MS;
  }
  function readValidTimeline(api){
    if(!api||api!==player||!song.id)return null;
    let t;try{t=api.getTimeline?.();}catch{return null;}
    if(!t||(t.songId&&t.songId!==song.id)||t.currentTime==null||!Number.isFinite(Number(t.currentTime)))return null;
    return {position:Math.max(0,Number(t.currentTime)),kind:t.source??'progress',duration:Number(t.totalTime)>0?Number(t.totalTime):0,
      sequence:Number.isFinite(t.sequence)?t.sequence:null};
  }
  function finishSeek(own,outcome,{reason='',resetStart=false}={}){
    if(!own||pendingSeek!==own)return false;
    clearTimeout(own.timer);own.timer=null;pendingSeek=null;
    own.adapter?.cancelSeek?.(own.id,outcome==='failed'?true:resetStart);
    if(outcome==='failed'){
      playback.resetProgress();
      const now=performance.now(),recovered=readValidTimeline(player);
      if(recovered){
        if(recovered.duration>0)song.duration=recovered.duration;
        clock.position=recovered.position;clock.anchor=now;clock.valid=recovered.kind!=='restore';
        clock.sample={position:recovered.position,playing:clock.playing,kind:recovered.kind,sequence:recovered.sequence,at:now};
      }else{
        clock.position=own.previous;clock.anchor=now;clock.valid=false;clock.sample=null;
      }
      status=reason+(clock.valid?'，已恢复进度':'，等待实际进度');
      sendClock('failed');
    }
    return true;
  }
  function acceptTimeline(t,api,playbackStatus){
    if(api!==player||!song.id||(t?.songId&&t.songId!==song.id)||t?.currentTime==null||!Number.isFinite(Number(t.currentTime)))return false;
    const next=Math.max(0,Number(t.currentTime)),now=performance.now(),kind=t.source??'progress',actual=kind!=='restore';
    const nextDuration=Number(t.totalTime)>0?Number(t.totalTime):0;
    const durationChanged=nextDuration>0&&nextDuration!==song.duration;
    if(durationChanged)song.duration=nextDuration;
    const state=playbackStatus===undefined?api.getPlaybackStatus?.():playbackStatus;
    const discontinuous=!!pendingSeek||!!t.seek||kind!=='progress';
    const nextPlaying=playback.sample(next,now,state,discontinuous);
    const prev=clock.sample;
    const sampleSequence=Number.isFinite(t.sequence)?t.sequence:null;
    const newSample=sampleSequence!==null&&sampleSequence!==prev?.sequence;
    const alreadyProcessed=sampleSequence!==null&&sampleSequence===prev?.sequence;
    let seekState=pendingSeek?'pending':'';
    if(pendingSeek&&actual&&!alreadyProcessed){
      const elapsed=nextPlaying?Math.max(0,now-pendingSeek.started):0;
      const expected=pendingSeek.target+elapsed;
      // Startup progress after a missed synchronous seek may begin slightly ahead
      // of target; reject bootstrap 0 / stale positions, accept authoritative seek.
      const reached=pendingSeek.startup
        ?next>=pendingSeek.target-250&&next<=expected+SEEK_TOLERANCE_MS
        :(Math.abs(next-expected)<=250||(!!t.seek&&Math.abs(next-pendingSeek.target)<=250));
      const newerSample=pendingSeek.sequence===undefined||t.sequence===undefined||t.sequence>pendingSeek.sequence;
      let staleIndex=-1;
      if(newerSample&&!reached&&pendingSeek.staleTargets?.length){
        staleIndex=pendingSeek.staleTargets.findIndex(item=>{
          const staleExpected=item.target+(nextPlaying?Math.max(0,now-item.started):0);
          return Math.abs(next-item.target)<=250||Math.abs(next-staleExpected)<=250;
        });
      }
      const superseded=staleIndex<0&&(pendingSeek.startup
        ?!!t.seek
        :(!!t.seek||Math.abs(next-live())>SEEK_TOLERANCE_MS));
      if(staleIndex>=0)pendingSeek.staleTargets.splice(staleIndex,1);
      else if(newerSample&&(reached||superseded)){finishSeek(pendingSeek,'confirmed');seekState='confirmed';}
    }
    const snapshotChanged=!prev||newSample||next!==prev.position||nextPlaying!==prev.playing||kind!==prev.kind||seekState==='confirmed';
    if(!snapshotChanged&&!durationChanged)return false;
    if(snapshotChanged){
      if(nextPlaying&&!clock.playing)clock.playingSince=now;
      clock.sample={position:next,playing:nextPlaying,kind,sequence:sampleSequence,at:now};
      clock.position=next;clock.anchor=now;clock.playing=nextPlaying;clock.valid=actual;
    }
    if(socket?.readyState===1)status='已连接';
    sendClock(seekState);return true;
  }
  function sample(playbackStatus){
    if(player)syncSong();
    if(!player)return false;
    try{return acceptTimeline(player.getTimeline(),player,playbackStatus);}catch(e){console.debug('[MiniLyrics] 网易云播放器 时间轴暂不可用',e);return false;}
  }
  function seek(ms,requestId,requestSong){
    syncSong();if(requestSong&&requestSong!==song.id)return;
    const target=Math.max(0,Math.min(song.duration||Infinity,Number(ms)));
    if(!Number.isFinite(target))return;
    lastSeekId=Number(requestId)||lastSeekId+1;
    const prevSeek=pendingSeek,started=performance.now();
    const staleTargets=prevSeek&&prevSeek.songId===song.id
      ?[...prevSeek.staleTargets,{target:prevSeek.target,started:prevSeek.started}]
        .filter(item=>started-item.started<=SEEK_TIMEOUT_MS*2).slice(-4)
      :[];
    finishSeek(pendingSeek,'cancelled');playback.resetProgress();
    const startup=typeof player?.isSeekSessionReady==='function'?!player.isSeekSessionReady():false;
    const own={id:lastSeekId,songId:song.id,adapter:player,timer:null,target,startup,sequence:player?.getTimeline?.()?.sequence,started,previous:live(),staleTargets};
    pendingSeek=own;
    own.timer=setTimeout(()=>finishSeek(own,'failed',{reason:'跳转未确认'}),SEEK_TIMEOUT_MS);
    sendClock('pending');
    try{
      if(typeof player?.seekTo!=='function')throw Error('网易云播放器 未就绪');
      const result=player.seekTo(target,own.id);
      if(result===false)finishSeek(own,'failed',{reason:'网易云播放器 未接受跳转'});
      else if(result?.then)result.then(value=>{if(value===false)finishSeek(own,'failed',{reason:'网易云播放器 未接受跳转'});},
        ()=>finishSeek(own,'failed',{reason:'网易云播放器 跳转失败'}));
    }catch{finishSeek(own,'failed',{reason:'网易云播放器 跳转失败'});}
  }
  function command(m){
    if(m.type!=='command')return;
    if(m.action==='seek')return seek(m.position,m.requestId,m.songId);
    if(m.action==='play'){if(player?.togglePlay)player.togglePlay();else player?.[player.getPlaybackStatus?.()==='Playing'?'pause':'play']?.();return;}
    if(m.action==='previous'||m.action==='next')player?.[m.action]?.();
  }
  function attachPlayer(){
    if(stopped||!windowSession)return;
    playerAdapter??=window.MiniLyricsPlayer.createPlayerAdapter();
    const candidate=(playerAdapter.metadataReady??playerAdapter.ready)?playerAdapter:null;
    const api=typeof candidate?.getCurrentSong==='function'&&typeof candidate?.getTimeline==='function'?candidate:null;
    const connected=socket?.readyState===1;
    if(api===player){if(connected&&!api)status='等待 网易云播放器';return;}
    detachPlayer?.();detachPlayer=null;playback.reset();
    const wasPending=pendingSeek;
    player=api;
    if(wasPending)finishSeek(wasPending,'failed',{reason:'网易云播放器 连接变化，跳转已取消'});
    resetClock(live());
    if(!player){if(connected)status='等待 网易云播放器';sendClock();return;}
    const onTimeline=e=>{if(player!==api)return;syncSong();acceptTimeline(e.detail,api);};
    const onSong=()=>{if(player===api)sample();};
    const onPlaying=e=>{
      if(player!==api)return;
      const now=performance.now();
      clock.position=live();clock.anchor=now;playback.event(e.detail);
      if(!sample(e.detail)){
        const nextPlaying=playback.current();
        if(nextPlaying&&!clock.playing)clock.playingSince=now;
        clock.playing=nextPlaying;
        sendClock(pendingSeek?'pending':'');
      }
    };
    const onControlError=e=>{const own=pendingSeek;if(player===api&&own&&e.detail?.requestId===own.id&&e.detail?.songId===own.songId)finishSeek(own,'failed',{reason:e.detail.reason});};
    const bindings=[['timelineUpdate',onTimeline],['songChange',onSong],['playStateChange',onPlaying],['controlError',onControlError]];
    for(const [name,fn]of bindings)api.addEventListener?.(name,fn);
    detachPlayer=()=>{for(const [name,fn]of bindings)api.removeEventListener?.(name,fn);};
    if(connected)status='已连接';
  }
  function tick(){
    if(stopped||socket?.readyState!==1)return;
    try{
      attachPlayer();sample();
      if(clock.playing&&clock.valid&&!pendingSeek&&!playerTimelineFresh()){
        clock.position=live();clock.anchor=performance.now();clock.valid=false;status='等待 网易云播放器 播放进度';sendClock();
      }
      refreshStatus();
    }catch(e){console.debug('[MiniLyrics] 状态同步暂不可用',e);}
    finally{
      if(socket?.readyState===1&&performance.now()-lastSentAt>=1000)send({type:'ping'});
      if(!stopped&&socket?.readyState===1){clearTimeout(timer);timer=setTimeout(tick,1000);}
    }
  }
  function resyncPage(){
    if(stopped||!windowSession)return;
    if(socket?.readyState!==1){queueReconnect(windowSession,0);return;}
    try{attachPlayer();sample();}catch(e){console.debug('[MiniLyrics] 恢复同步失败',e);}
  }
  function canRetryLyrics(){return !stopped&&!!windowSession&&!!song.lyricId&&(lyricState.mode==='error'||lyricState.mode==='loading');}
  function refreshShortcut(){
    if(!shortcutButton)return;
    const connected=socket?.readyState===1;
    const opening=busy&&!connected;
    const idle=status==='小窗未启动'||status==='小窗已关闭'||status==='小窗未连接，请先打开小窗';
    const label=connected?'关闭 MiniLyrics 小窗':opening?'正在打开 MiniLyrics 小窗…':windowSession?'重新连接 MiniLyrics 小窗':
      idle?'打开 MiniLyrics 小窗':'小窗连接异常，点击重试';
    shortcutButton.title=label;
    shortcutButton.setAttribute('aria-pressed',String(connected));
    shortcutButton.setAttribute('aria-disabled',String(opening));
  }
  function findShortcutAnchor(){
    for(const footer of document.querySelectorAll('footer')){
      const more=footer.querySelector(MORE_ICON_SELECTOR);
      const volume=footer.querySelector(VOLUME_ICON_SELECTOR);
      if(more&&volume&&!more.contains(volume)&&!volume.contains(more)){
        // Insert at the shared controls row, outside any native button or tooltip wrapper.
        let element=more;
        while(!element.parentElement.contains(volume))element=element.parentElement;
        return {element,before:true};
      }
      const element=(more??volume)?.closest(PLAYBAR_CONTROL_SELECTOR);
      if(element&&footer.contains(element))return {element,before:!!more};
    }
    return null;
  }
  function shortcutIsPlaced(anchor){
    return anchor?.element.isConnected&&shortcutButton!==null&&
      (anchor.before?anchor.element.previousElementSibling:anchor.element.nextElementSibling)===shortcutButton;
  }
  function mountShortcut(){
    shortcutFrame=null;
    shortcutAnchor=findShortcutAnchor();
    if(!shortcutAnchor){shortcutButton?.remove();return;}
    if(!shortcutButton){
      shortcutStyle=document.createElement('style');
      shortcutStyle.textContent=`
        #miniLyrics-playbar-toggle{position:relative;display:inline-grid;place-items:center;
          flex:0 0 32px;width:32px;height:32px;box-sizing:border-box;align-self:center;
          padding:0;border:0;border-radius:6px;background:transparent;color:inherit;cursor:pointer}
        #miniLyrics-playbar-toggle::before{content:'';box-sizing:border-box;width:18px;height:15px;
          border:1.5px solid currentColor;border-radius:3px}
        #miniLyrics-playbar-toggle::after{content:'';position:absolute;left:12px;top:13px;
          width:8px;height:1.5px;background:currentColor;box-shadow:0 4px currentColor}
        #miniLyrics-playbar-toggle:hover{background:rgba(128,128,128,.16)}
        #miniLyrics-playbar-toggle:focus-visible{outline:2px solid currentColor;outline-offset:1px}
        #miniLyrics-playbar-toggle[aria-pressed="true"]{color:#e84d5e;background:rgba(232,77,94,.15)}
        #miniLyrics-playbar-toggle[aria-disabled="true"]{opacity:.5;cursor:wait}`;
      document.head.append(shortcutStyle);
      shortcutButton=document.createElement('button');
      shortcutButton.type='button';
      shortcutButton.id='miniLyrics-playbar-toggle';
      shortcutButton.setAttribute('aria-label','MiniLyrics 小窗');
      shortcutButton.onclick=()=>{
        if(busy)return;
        if(socket?.readyState===1)closeWindow();
        else open();
      };
    }
    if(!shortcutIsPlaced(shortcutAnchor)){
      const {element,before}=shortcutAnchor;
      if(before)element.before(shortcutButton);
      else element.after(shortcutButton);
    }
    refreshShortcut();
  }
  function watchShortcut(){
    if(stopped||shortcutObserver||!document.documentElement)return;
    shortcutObserver=new MutationObserver(()=>{
      if(shortcutIsPlaced(shortcutAnchor))return;
      if(shortcutFrame===null)shortcutFrame=window.requestAnimationFrame(mountShortcut);
    });
    shortcutObserver.observe(document.documentElement,{childList:true,subtree:true});
    mountShortcut();
  }
  function refreshStatus(initial=false){
    refreshShortcut();
    if(!statusView||(!initial&&statusView.isConnected===false))return;
    if(retryLyricsButton)retryLyricsButton.disabled=!canRetryLyrics();
    const state=playerAdapter?.getDiagnostics?.()?.streamState,labels={'waiting-player':'播放器未就绪','waiting-subscription':'等待播放器初始化','subscription-lost':'播放同步已中断','waiting-progress':'等待实际播放进度','events-filtered':'收到进度但未接受','progress-stalled':'播放进度已中断','progress-live':'进度正常','progress-paused':'播放已暂停'};
    const count=publishedLyrics?.lines.length??0;
    const text=status+' · 进度：'+(labels[state]??(player?'网易云播放器':'等待 网易云播放器'))+' · '+Math.floor(live()/1000)+' 秒 · '+lyricStatus[lyricState.mode]+' · '+count+' 行 · 小窗确认：'+(confirmedLyricCount??'待回报');if(statusView.textContent!==text)statusView.textContent=text;
    const idle=status==='小窗未启动'||status==='小窗已关闭';
    const statusClass=socket?.readyState===1?'ml-status ml-status-connected':idle?'ml-status':'ml-status ml-status-alert';
    if(statusView.className!==statusClass)statusView.className=statusClass;
  }
  function connectSocket(port,own=null,probeFont=fontSize){
    const ws=new WebSocket(`ws://127.0.0.1:${port}/`);
    return new Promise(resolve=>{
      let settled=false;
      const finish=ok=>{if(settled)return;settled=true;clearTimeout(timeout);if(!ok)ws.close();resolve(ok?ws:null);};
      const timeout=setTimeout(()=>finish(false),800);
      ws.onopen=()=>{
        if(!own||!ownsWindow(own)){finish(true);return;}
        // Only an authenticated host answers a font message, including older hosts.
        ws.onmessage=e=>{
          let reply;try{reply=JSON.parse(e.data);}catch{return;}
          if(reply?.type==='host-state'&&reply.accepted===true)finish(true);
        };
        try{
          ws.send(JSON.stringify({type:'hello',token:own.token}));
          ws.send(JSON.stringify({type:'font',size:probeFont}));
        }catch{finish(false);}
      };
      ws.onerror=()=>finish(false);ws.onclose=()=>finish(false);
    });
  }
  function retireSocket(ws,token){
    if(!ws)return;
    if(ws.readyState===1){ws.send(JSON.stringify({type:'hello',token}));ws.send(JSON.stringify({type:'exit'}));}
    ws.close();
  }
  async function retireHost(port,token){
    for(let i=0;i<6;i++){
      await new Promise(r=>setTimeout(r,200));
      try{const ws=await connectSocket(port);if(ws){retireSocket(ws,token);return;}}catch{}
    }
  }
  function retireWindow(own,ws=null){
    if(own.retired){ws?.close();return;}
    own.retired=true;
    if(ws)retireSocket(ws,own.token);else return retireHost(own.port,own.token);
  }
  const ownsWindow=own=>!stopped&&windowSession===own&&own.session===session;
  function rememberWindow(own){
    try{window.sessionStorage?.setItem('MiniLyrics.window',JSON.stringify({port:own.port,token:own.token}));}catch{}
  }
  function forgetWindow(){try{window.sessionStorage?.removeItem('MiniLyrics.window');}catch{}}
  function releaseConnection(){
    clearTimeout(timer);timer=null;finishSeek(pendingSeek,'cancelled',{resetStart:true});lastSeekId=0;playback.reset();
    resetClock(live());hostState=null;confirmedLyricCount=null;lastSentAt=0;
    if(stopped||!windowSession){
      detachPlayer?.();detachPlayer=null;player=null;playerAdapter?.dispose();playerAdapter=null;
    }
  }
  function windowClosed(own,nextStatus='小窗已关闭'){
    if(!ownsWindow(own))return;
    windowSession=null;session++;busy=false;clearTimeout(reconnectTimer);reconnectTimer=null;forgetWindow();
    resetWindowSong();
    const previous=socket;socket=null;releaseConnection();previous?.close();
    status=nextStatus;refreshStatus();
  }
  function bindSocket(ws,own,probeFont){
    // Recover the actual snapshot before initializing the same host again.
    try{attachPlayer();sample();}catch(e){console.debug('[MiniLyrics] 等待播放状态恢复',e);}
    socket=ws;
    ws.onmessage=e=>{
      if(!ownsWindow(own)||socket!==ws)return;
      try{
        const m=JSON.parse(e.data);
        if(m.type==='lyrics-ack'){
          if(m.accepted===true&&m.songId===song.id&&publishedLyrics&&m.revision===publishedLyrics.revision){
            confirmedLyricCount=Number.isFinite(Number(m.lyricCount))?Number(m.lyricCount):publishedLyrics.lines.length;
            hostState=m;
          }
          refreshStatus();
        }else if(m.type==='host-state'||m.type==='background-state'){hostState=m;refreshStatus();}
        else command(m);
      }catch(err){console.debug('[MiniLyrics]',err);}
    };
    ws.onclose=e=>{
      if(!ownsWindow(own)||socket!==ws)return;
      if(e?.code===1000&&e?.reason==='host-closed'){windowClosed(own);return;}
      socket=null;releaseConnection();status='小窗连接中断，等待恢复';refreshStatus();queueReconnect(own);
    };
    own.failures=0;
    rememberWindow(own);
    if(song.id){
      sendTrack();
      if(publishedLyrics)sendLyrics();
      sendClock();
    }
    if(fontSize!==probeFont)sendFont();
    sendStyle();sendBackground();
    status='已连接';tick();
  }
  function connectWindow(own){
    if(!own.connection){
      own.connection=(async()=>{
        const probeFont=fontSize;
        let ws;
        try{
          ws=await connectSocket(own.port,own,probeFont);
          if(!ownsWindow(own)){if(stopped)ws?.close();else retireWindow(own,ws);return false;}
          if(!ws)return false;
          bindSocket(ws,own,probeFont);
          return socket===ws&&ws.readyState===1;
        }catch(e){
          if(ws&&socket===ws){socket=null;releaseConnection();}
          ws?.close();
          console.debug('[MiniLyrics] 等待小窗重新连接',e);
          return false;
        }
      })().finally(()=>{own.connection=null;});
    }
    return own.connection;
  }
  function queueReconnect(own,delay=1000){
    if(!ownsWindow(own)||own.connection||socket?.readyState===1)return;
    clearTimeout(reconnectTimer);
    reconnectTimer=setTimeout(async()=>{
      reconnectTimer=null;if(!ownsWindow(own)||busy||socket?.readyState===1)return;
      if(await connectWindow(own)||!ownsWindow(own)||busy)return;
      if(++own.failures>=MAX_RECONNECT_FAILURES)windowClosed(own,'小窗连接失败，请重新打开');
      else queueReconnect(own,2000);
    },delay);
  }
  async function open(){
    if(stopped||busy)return;
    if(socket?.readyState===1){send({type:'show'});return;}
    busy=true;refreshShortcut();let ownSession=session;
    const current=()=>!stopped&&ownSession===session;
    try{
      const previous=windowSession;
      if(previous){
        clearTimeout(reconnectTimer);reconnectTimer=null;
        const recovered=await connectWindow(previous);
        if(!ownsWindow(previous))return;
        if(recovered){send({type:'show'});return;}
        windowClosed(previous,'正在重新打开小窗');
        busy=true;refreshShortcut();
      }
      ownSession=++session;
      const bytes=new Uint8Array(24);crypto.getRandomValues(bytes);
      const token=Array.from(bytes,b=>b.toString(16).padStart(2,'0')).join('');
      const pluginPath=String(plugin.pluginPath??loadedPlugins.MiniLyrics?.pluginPath??'').replace(/\//g,'\\').replace(/\\\.\\/g,'\\');
      if(!pluginPath||/["\r\n]/.test(pluginPath))throw new Error('无法确定插件路径');
      let port=42000+Math.floor(Math.random()*6000);
      if(port===previous?.port)port=42000+(port-42000+1)%6000;
      const own={port,token,session:ownSession,connection:null,failures:0,launched:false};
      windowSession=own;
      const ok=await betterncm.app.exec('"'+pluginPath+'\\MiniLyricsHost.exe" --port '+port+' --token '+token,false,false);
      if(!current()){if(ok!==false&&!stopped)await retireWindow(own);return;}
      if(ok===false)throw new Error('原生窗口启动失败');
      own.launched=true;rememberWindow(own);
      for(let attempt=0;attempt<15;attempt++){
        await new Promise(r=>setTimeout(r,200));
        if(!current()){if(!stopped)await retireWindow(own);return;}
        if(await connectWindow(own)||!current())return;
      }
      status='小窗等待连接';refreshStatus();queueReconnect(own);
    }catch(e){
      if(current()){
        const own=windowSession;
        if(own?.launched){
          status='小窗等待恢复连接';queueReconnect(own);
        }else{windowSession=null;forgetWindow();status=String(e.message??e);}
        refreshStatus();console.error('[MiniLyrics]',e);
      }
    }finally{if(ownSession===session){busy=false;refreshShortcut();}}
  }
  function closeWindow(){
    const own=windowSession,connected=socket?.readyState===1;
    session++;windowSession=null;busy=false;clearTimeout(reconnectTimer);reconnectTimer=null;forgetWindow();
    resetWindowSong();
    send({type:'exit'});const previous=socket;socket=null;releaseConnection();previous?.close();
    if(own?.launched&&!connected&&!own.connection)retireWindow(own);
    status='小窗已关闭';refreshStatus();
  }
  function dispose(){
    // Page reload/unload is temporary bridge loss, not a user close request.
    if(windowSession)rememberWindow(windowSession);
    stopped=true;session++;busy=false;cancelLyrics();clearTimeout(reconnectTimer);reconnectTimer=null;
    shortcutObserver?.disconnect();shortcutObserver=null;
    if(shortcutFrame!==null)window.cancelAnimationFrame(shortcutFrame);
    shortcutFrame=null;shortcutAnchor=null;
    shortcutButton?.remove();shortcutButton=null;shortcutStyle?.remove();shortcutStyle=null;
    const previous=socket;socket=null;releaseConnection();previous?.close();
    for(const [target,name,fn]of listeners)target.removeEventListener?.(name,fn);
  }
  plugin.onLoad(()=>{
    try{
      const saved=JSON.parse(window.sessionStorage?.getItem('MiniLyrics.window')??'null');
      if(saved&&Number.isInteger(saved.port)&&saved.port>=42000&&saved.port<48000&&/^[0-9a-f]{48}$/.test(saved.token)){
        windowSession={port:saved.port,token:saved.token,session:++session,connection:null,failures:0,launched:true};queueReconnect(windowSession,0);
      }
    }catch{}
    watchShortcut();
  });
  plugin.onConfig(()=>{
    const create=(tag,className,text)=>{
      const element=document.createElement(tag);
      if(className)element.className=className;
      if(text!==undefined)element.textContent=text;
      return element;
    };
    const el=create('div');
    const setTheme=value=>{
      const next='ml-config '+(value?'ml-dark':'ml-light');
      if(el.className!==next)el.className=next;
    };
    let dark=window.matchMedia?.('(prefers-color-scheme: dark)').matches??false;
    try{
      const theme=window.localStorage?.getItem('currentTheme');
      if(theme)dark=/^dark/i.test(theme);
    }catch{}
    setTheme(dark);
    const colorChannels=value=>value.match(/^rgba?\(([^)]+)\)$/i)?.[1].split(',').map(Number);
    const luminance=rgb=>[.2126,.7152,.0722].reduce((sum,weight,index)=>{
      const channel=rgb[index]/255;
      return sum+weight*(channel<=.04045?channel/12.92:((channel+.055)/1.055)**2.4);
    },0);
    let observer=null;
    const attachThemeObserver=()=>{
      if(observer||typeof MutationObserver!=='function'||!el.isConnected)return;
      observer=new MutationObserver(()=>{
        if(!el.isConnected){observer.disconnect();observer=null;return;}
        syncTheme();
      });
      for(const node of [document.documentElement,document.body]){
        if(node)observer.observe(node,{attributes:true,attributeFilter:['class','style']});
      }
    };
    const syncTheme=(attempt=0)=>{
      if(!el.isConnected){
        if(observer){observer.disconnect();observer=null;}
        if(attempt<8)window.requestAnimationFrame?.(()=>syncTheme(attempt+1));
        return;
      }
      attachThemeObserver();
      if(typeof window.getComputedStyle!=='function')return;
      // Skin names may not describe the active palette. Read the mounted panel,
      // including translucent backgrounds layered over its ancestors.
      const text=colorChannels(window.getComputedStyle(el).color);
      const surface=[0,0,0];let uncovered=1;
      for(let parent=el.parentElement;parent&&uncovered>.01;parent=parent.parentElement){
        const rgb=colorChannels(window.getComputedStyle(parent).backgroundColor);
        if(!rgb)continue;
        const alpha=rgb[3]??1;
        const weight=alpha*uncovered;
        for(let i=0;i<3;i++)surface[i]+=rgb[i]*weight;
        uncovered*=1-alpha;
      }
      const foreground=text?luminance(text):null;
      const backdrop=uncovered<=.01?luminance(surface):null;
      if(backdrop!==null&&(foreground===null||Math.abs(backdrop-foreground)>.18))setTheme(backdrop<.23);
      else if(foreground!==null)setTheme(foreground>.35);
    };
    window.requestAnimationFrame?.(()=>syncTheme());
    const style=create('style');
    style.textContent=`
      .ml-config{--ml-text:#1c1d22;--ml-muted:#5b606b;--ml-card:rgba(0,0,0,.035);
        --ml-card-border:rgba(0,0,0,.09);--ml-divider:rgba(0,0,0,.07);
        --ml-control:#fff;--ml-control-hover:#f2f3f6;--ml-control-border:rgba(0,0,0,.15);
        --ml-seg-bg:rgba(0,0,0,.06);--ml-seg-active:#fff;--ml-seg-active-shadow:0 1px 2px rgba(0,0,0,.12);
        --ml-track:rgba(0,0,0,.14);--ml-switch-off:rgba(0,0,0,.22);--ml-thumb:#fff;
        --ml-option-bg:#fff;--ml-option-fg:#1c1d22;--ml-option-hover:#f2f3f6;
        --ml-accent:#d93842;--ml-accent-hover:#c22d36;--ml-accent-ring:rgba(217,56,66,.35);--ml-on-accent:#fff;
        --ml-state:#828794;--ml-connected:#209362;--ml-connected-ring:rgba(32,147,98,.22);
        --ml-alert:#c67617;--ml-alert-ring:rgba(198,118,23,.22);
        color-scheme:light;color:var(--ml-text);background:transparent;box-sizing:border-box;max-width:660px;
        padding:6px clamp(8px,2vw,16px) 16px;font:inherit;font-size:13px;line-height:1.5}
      .ml-config.ml-dark{--ml-text:#f2f3f7;--ml-muted:#a1a6b3;--ml-card:rgba(255,255,255,.048);
        --ml-card-border:rgba(255,255,255,.095);--ml-divider:rgba(255,255,255,.07);
        --ml-control:rgba(255,255,255,.075);--ml-control-hover:rgba(255,255,255,.135);--ml-control-border:rgba(255,255,255,.15);
        --ml-seg-bg:rgba(0,0,0,.28);--ml-seg-active:rgba(255,255,255,.16);--ml-seg-active-shadow:0 1px 2px rgba(0,0,0,.28);
        --ml-track:rgba(255,255,255,.16);--ml-switch-off:rgba(255,255,255,.22);--ml-thumb:#fff;
        --ml-option-bg:#25262b;--ml-option-fg:#f3f4f6;--ml-option-hover:#353740;
        --ml-accent:#e5434c;--ml-accent-hover:#f2545d;--ml-accent-ring:rgba(229,67,76,.45);--ml-on-accent:#fff;
        --ml-state:#8e93a0;--ml-connected:#3ecf8e;--ml-connected-ring:rgba(62,207,142,.25);
        --ml-alert:#f0a84c;--ml-alert-ring:rgba(240,168,76,.25);
        color-scheme:dark}
      .ml-config .ml-head{display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin:0 0 12px}
      .ml-config h2{margin:0;color:var(--ml-text);font-size:18px;line-height:1.3;font-weight:700;letter-spacing:-.01em}
      .ml-config .ml-badge{display:inline-flex;align-items:center;padding:2px 8px;border-radius:999px;
        background:var(--ml-card);border:1px solid var(--ml-card-border);color:var(--ml-muted);
        font-size:11px;font-weight:600;line-height:1.4;font-variant-numeric:tabular-nums}
      .ml-config .ml-actions,.ml-config .ml-links{display:flex;align-items:center;flex-wrap:wrap;gap:7px}
      .ml-config .ml-section{padding:4px 14px;background:var(--ml-card);border:1px solid var(--ml-card-border);border-radius:12px}
      .ml-config .ml-section+.ml-section{margin-top:12px}
      .ml-config h3{display:flex;align-items:center;gap:8px;margin:0;padding:10px 0 7px;color:var(--ml-text);
        font-size:13px;font-weight:700;letter-spacing:.02em}
      .ml-config h3::before{content:'';width:3px;height:12px;border-radius:999px;background:var(--ml-accent);flex:none}
      .ml-config .ml-row{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:8px 14px;
        padding:10px 0;border-top:1px solid var(--ml-divider)}
      .ml-config .ml-caption{flex:1 1 130px;min-width:0;overflow-wrap:anywhere}
      .ml-config .ml-label{display:block;color:var(--ml-text);font-size:13.5px;font-weight:500}
      .ml-config .ml-hint{display:block;margin-top:2px;color:var(--ml-muted);font-size:12px;line-height:1.4}
      .ml-config .ml-controls{display:inline-flex;align-items:center;flex-wrap:wrap;gap:8px;max-width:100%}
      .ml-config button,.ml-config input[type=number],.ml-config select{box-sizing:border-box;margin:0;
        font:inherit;line-height:1.35;color:var(--ml-text);background:var(--ml-control);
        border:1px solid var(--ml-control-border);
        transition:background-color .15s ease,border-color .15s ease,color .15s ease,box-shadow .15s ease}
      .ml-config .ml-btn{min-height:31px;border-radius:999px;padding:5px 12px;font-size:12.5px;font-weight:500;
        white-space:nowrap;cursor:pointer}
      .ml-config .ml-btn:hover:not(:disabled){background:var(--ml-control-hover)}
      .ml-config .ml-btn:active:not(:disabled){transform:translateY(.5px)}
      .ml-config button.ml-primary{background:var(--ml-accent);border-color:var(--ml-accent);color:var(--ml-on-accent);
        padding-inline:15px;font-weight:600;box-shadow:0 2px 8px var(--ml-accent-ring)}
      .ml-config button.ml-primary:hover:not(:disabled){background:var(--ml-accent-hover);border-color:var(--ml-accent-hover)}
      .ml-config input[type=number]{height:30px;border-radius:8px;padding:3px 6px;font-size:13px;
        text-align:center;font-variant-numeric:tabular-nums;-moz-appearance:textfield}
      .ml-config input[type=number]::-webkit-inner-spin-button,
      .ml-config input[type=number]::-webkit-outer-spin-button{-webkit-appearance:none;margin:0}
      .ml-config input[type=number]:hover,.ml-config input[type=number]:focus{border-color:var(--ml-accent)}
      .ml-config .ml-num-font{width:48px}
      .ml-config .ml-range{appearance:none;width:108px;height:5px;border-radius:999px;background:var(--ml-track);
        outline:none;cursor:pointer;margin:0;border:none;padding:0}
      .ml-config .ml-range::-webkit-slider-thumb{appearance:none;width:15px;height:15px;border-radius:50%;
        background:var(--ml-accent);border:2px solid #fff;box-shadow:0 1px 4px rgba(0,0,0,.3);cursor:pointer;
        transition:transform .12s ease}
      .ml-config .ml-range::-webkit-slider-thumb:hover{transform:scale(1.12)}
      .ml-config .ml-stepper{display:inline-flex;align-items:center;height:30px;border:1px solid var(--ml-control-border);
        border-radius:8px;background:var(--ml-control);overflow:hidden}
      .ml-config .ml-stepper input[type=number]{width:66px;height:100%;border:none;border-inline:1px solid var(--ml-divider);
        border-radius:0;background:transparent}
      .ml-config .ml-stepper input[type=number]:focus{outline:none;background:var(--ml-control-hover)}
      .ml-config .ml-step-btn{width:28px;height:100%;padding:0;border:none;border-radius:0;background:transparent;
        color:var(--ml-muted);font-size:15px;font-weight:500;display:inline-flex;align-items:center;justify-content:center;cursor:pointer}
      .ml-config .ml-step-btn:hover:not(:disabled){background:var(--ml-control-hover);color:var(--ml-text)}
      .ml-config .ml-sr-select{display:none}
      .ml-config .ml-seg{display:inline-flex;align-items:center;flex-wrap:wrap;max-width:100%;padding:3px;gap:2px;
        border-radius:9px;background:var(--ml-seg-bg);border:1px solid var(--ml-divider)}
      .ml-config .ml-seg-btn{border:none;border-radius:6px;padding:4px 10px;min-height:24px;font-size:12.5px;font-weight:500;
        background:transparent;color:var(--ml-muted);white-space:nowrap;cursor:pointer}
      .ml-config .ml-seg-btn:hover:not(:disabled){color:var(--ml-text)}
      .ml-config .ml-seg-btn.ml-seg-active{background:var(--ml-seg-active);color:var(--ml-text);font-weight:600;
        box-shadow:var(--ml-seg-active-shadow)}
      .ml-config .ml-seg-disabled{opacity:.5}
      .ml-config .ml-seg-disabled .ml-seg-btn:disabled{opacity:1}
      .ml-config select.ml-select{appearance:none;min-height:30px;min-width:176px;max-width:100%;border-radius:8px;
        padding:4px 30px 4px 10px;font-size:12.5px;cursor:pointer;
        background-color:var(--ml-option-bg);color:var(--ml-option-fg);
        background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='10' height='6' viewBox='0 0 10 6'%3E%3Cpath fill='none' stroke='%23888' stroke-width='1.5' stroke-linecap='round' stroke-linejoin='round' d='M1 1l4 4 4-4'/%3E%3C/svg%3E");
        background-repeat:no-repeat;background-position:right 10px center}
      .ml-config select.ml-select:hover:not(:disabled){background-color:var(--ml-option-hover)}
      .ml-config select option,.ml-config select optgroup{background-color:var(--ml-option-bg);color:var(--ml-option-fg)}
      @supports(appearance:base-select){
        .ml-config select.ml-select,.ml-config select.ml-select::picker(select){appearance:base-select}
        .ml-config select.ml-select{display:inline-flex;align-items:center;justify-content:space-between;gap:8px;
          padding-inline:10px;background-image:none}
        .ml-config select.ml-select::picker-icon{color:var(--ml-muted);transition:transform .15s ease}
        .ml-config select.ml-select:open::picker-icon{transform:rotate(180deg)}
        .ml-config select.ml-select::picker(select){margin-top:4px;padding:4px;border:1px solid var(--ml-control-border);
          border-radius:10px;background-color:var(--ml-option-bg);color:var(--ml-option-fg);box-shadow:0 8px 24px rgba(0,0,0,.28)}
        .ml-config select.ml-select option{padding:6px 10px;border-radius:6px}
        .ml-config select.ml-select option:hover,.ml-config select.ml-select option:focus,.ml-config select.ml-select option:checked{
          background-color:var(--ml-option-hover);color:var(--ml-option-fg)}
      }
      .ml-config :focus-visible{outline:2px solid var(--ml-accent);outline-offset:2px}
      .ml-config :disabled{opacity:.5;cursor:default}
      .ml-config .ml-switch{appearance:none;box-sizing:border-box;display:inline-flex;align-items:center;margin:0;
        width:38px;height:22px;border:1px solid transparent;border-radius:999px;background:var(--ml-switch-off);
        padding:2px;cursor:pointer;transition:background-color .16s ease,opacity .16s ease;flex:none}
      .ml-config .ml-switch::before{content:'';width:16px;height:16px;border-radius:50%;background:var(--ml-thumb);
        box-shadow:0 1px 3px rgba(0,0,0,.28);transform:translateX(0);transition:transform .16s ease}
      .ml-config .ml-switch:checked{background:var(--ml-accent)}
      .ml-config .ml-switch:checked::before{transform:translateX(16px)}
      @media(forced-colors:active){.ml-config .ml-switch{appearance:auto}.ml-config .ml-switch::before{content:none}}
      .ml-config .ml-status{position:relative;margin:10px 0 14px;padding:8px 12px 8px 26px;
        background:var(--ml-card);border:1px solid var(--ml-card-border);border-radius:10px;
        color:var(--ml-muted);font-size:12px;line-height:1.45;overflow-wrap:anywhere}
      .ml-config .ml-status::before{content:'';position:absolute;left:11px;top:13px;width:7px;height:7px;border-radius:50%;
        background:var(--ml-state)}
      .ml-config .ml-status-connected::before{background:var(--ml-connected);box-shadow:0 0 0 3px var(--ml-connected-ring)}
      .ml-config .ml-status-alert::before{background:var(--ml-alert);box-shadow:0 0 0 3px var(--ml-alert-ring)}
      .ml-config .ml-footer{display:flex;align-items:center;justify-content:space-between;gap:8px 14px;flex-wrap:wrap;
        margin:14px 2px 0}
      .ml-config .ml-help,.ml-config .ml-links{color:var(--ml-muted);font-size:12px}
      .ml-config .ml-links{gap:12px}
      .ml-config .ml-links a{color:var(--ml-muted);text-decoration:none;border-bottom:1px solid var(--ml-divider);padding-bottom:1px;
        transition:color .15s ease,border-color .15s ease}
      .ml-config .ml-links a:hover{color:var(--ml-accent);border-bottom-color:var(--ml-accent)}
      .ml-config .ml-help{margin:0;overflow-wrap:anywhere}
    `;
    el.append(style);
    const button=(text,onClick,id,className='ml-btn')=>{
      const control=create('button',className,text);
      control.type='button';
      if(id)control.id=id;
      control.onclick=onClick;
      return control;
    };
    const head=create('div','ml-head');
    head.append(create('h2',null,'MiniLyrics 音乐小窗'));
    if(typeof MINILYRICS_VERSION==='string'&&MINILYRICS_VERSION)head.append(create('span','ml-badge','v'+MINILYRICS_VERSION));
    el.append(head);
    const requireSocket=fn=>()=>{if(socket?.readyState===1)fn();else{status='小窗未连接，请先打开小窗';refreshStatus();}};
    const actions=create('div','ml-actions');
    retryLyricsButton=button('重试获取歌词',()=>{if(canRetryLyrics())loadLyrics(true);},'miniLyrics-retry-lyrics');
    actions.append(button('打开 / 找回小窗',open,'miniLyrics-open','ml-btn ml-primary'),
      button('退出鼠标穿透',requireSocket(()=>send({type:'unlock'}))),
      button('重置位置和尺寸',requireSocket(()=>send({type:'reset'}))),
      button('关闭小窗',closeWindow,'miniLyrics-close'),retryLyricsButton);
    statusView=create('p','ml-status');statusView.id='miniLyrics-status';
    el.append(actions,statusView);

    const makeSelect=(id,choices,value,className='ml-select')=>{
      const control=create('select',className);control.id=id;
      for(const [v,text]of choices){const option=create('option',null,text);option.value=String(v);control.append(option);}
      control.value=String(value);return control;
    };
    const makeSegmented=(select,choices)=>{
      select.className='ml-sr-select';
      const group=create('div','ml-seg');
      group.setAttribute('role','group');
      const items=[];
      for(const [v,text]of choices){
        const item=create('button','ml-seg-btn',text);
        item.type='button';
        item.onclick=()=>{
          if(select.disabled)return;
          const next=String(v);
          if(select.value!==next){select.value=next;select.onchange?.();}
          else sync();
        };
        items.push([String(v),item]);
        group.append(item);
      }
      const sync=()=>{
        const current=String(select.value),off=!!select.disabled;
        group.className=off?'ml-seg ml-seg-disabled':'ml-seg';
        for(const [value,item]of items){
          const active=value===current;
          item.disabled=off;
          item.className=active?'ml-seg-btn ml-seg-active':'ml-seg-btn';
          item.setAttribute('aria-pressed',active?'true':'false');
        }
      };
      sync();
      return {group,sync};
    };
    const makeSwitch=(id,checked)=>{
      const control=create('input','ml-switch');control.id=id;control.type='checkbox';control.checked=checked;return control;
    };
    const makeNumber=(id,min,max,step,value,className)=>{
      const control=create('input',className);control.id=id;control.type='number';
      control.min=String(min);control.max=String(max);control.step=String(step);control.value=String(value);return control;
    };
    const addRow=(section,text,items,{labelFor,hint}={})=>{
      const row=create('div','ml-row'),caption=create('div','ml-caption'),label=create('label','ml-label',text);
      if(labelFor)label.htmlFor=labelFor;
      caption.append(label);
      const list=Array.isArray(items)?items:[items];
      if(hint){
        const description=create('span','ml-hint',hint);
        if(labelFor)description.id=labelFor+'-hint';
        caption.append(description);
        if(description.id&&list[0]?.setAttribute)list[0].setAttribute('aria-describedby',description.id);
      }
      const controls=create('div','ml-controls');
      controls.append(...list);
      row.append(caption,controls);section.append(row);
    };
    const lyricsSection=create('section','ml-section'),backgroundSection=create('section','ml-section');
    lyricsSection.append(create('h3',null,'歌词'));
    backgroundSection.append(create('h3',null,'背景'));
    const fontRange=create('input','ml-range');fontRange.type='range';fontRange.min='14';fontRange.max='36';fontRange.step='1';fontRange.value=String(fontSize);
    fontRange.setAttribute('aria-label','歌词字号滑块');
    const fontInput=makeNumber('miniLyrics-font-size',14,36,1,fontSize,'ml-num-font');
    const syncFontUI=()=>{
      fontInput.value=String(fontSize);fontRange.value=String(fontSize);
      const pct=Math.max(0,Math.min(100,(fontSize-14)/22*100));
      fontRange.style.background=`linear-gradient(to right,var(--ml-accent) ${pct}%,var(--ml-track) ${pct}%)`;
    };
    syncFontUI();
    fontRange.oninput=()=>{changeFontSize(fontRange.value);syncFontUI();};
    fontInput.onchange=()=>{changeFontSize(fontInput.value);syncFontUI();};
    const fontReset=button('恢复默认',()=>{changeFontSize(21);syncFontUI();});
    const offsetInput=makeNumber('miniLyrics-lyric-offset',-10000,10000,50,offset);
    function changeOffset(value){offset=normalizeOffset(value);offsetInput.value=String(offset);saveConfig('lyricOffset',offset);sendClock(pendingSeek?'pending':'');}
    offsetInput.onchange=()=>changeOffset(offsetInput.value);
    const offsetMinus=button('−',()=>changeOffset(offset-50),null,'ml-step-btn');
    offsetMinus.setAttribute('aria-label','减少 50 毫秒');offsetMinus.title='-50 ms';
    const offsetPlus=button('+',()=>changeOffset(offset+50),null,'ml-step-btn');
    offsetPlus.setAttribute('aria-label','增加 50 毫秒');offsetPlus.title='+50 ms';
    const offsetStepper=create('div','ml-stepper');offsetStepper.append(offsetMinus,offsetInput,offsetPlus);
    const offsetReset=button('归零',()=>changeOffset(0),'miniLyrics-reset-lyric-offset');
    const curveChoices=[[0,'平滑'],[1,'利落'],[2,'缓慢'],[3,'缓出'],[4,'弹簧']];
    const curveInput=makeSelect('miniLyrics-lyric-curve',curveChoices,curve);
    const curveSeg=makeSegmented(curveInput,curveChoices);
    curveInput.onchange=()=>{const value=Number(curveInput.value);if(Number.isInteger(value)&&value>=0&&value<=4&&value!==curve){curve=value;saveConfig('lyricCurve',curve);sendStyle();}curveSeg.sync();};
    const staggerInput=makeSwitch('miniLyrics-lyric-stagger',stagger);
    staggerInput.onchange=()=>{if(stagger!==staggerInput.checked){stagger=staggerInput.checked;saveConfig('lyricStagger',stagger);sendStyle();}};
    const wordModeChoices=[[0,'整词淡入'],[1,'渐变扫光']];
    const wordModeInput=makeSelect('miniLyrics-lyric-word-mode',wordModeChoices,wordMode);
    const wordModeSeg=makeSegmented(wordModeInput,wordModeChoices);
    wordModeInput.onchange=()=>{const value=Number(wordModeInput.value);if((value===0||value===1)&&value!==wordMode){wordMode=value;saveConfig('lyricWordMode',wordMode);sendStyle();}wordModeSeg.sync();};
    addRow(lyricsSection,'歌词字号',[fontRange,fontInput,fontReset],{labelFor:fontInput.id,hint:'翻译和前后歌词按比例缩放'});
    addRow(lyricsSection,'歌词偏移（ms）',[offsetStepper,offsetReset],{labelFor:offsetInput.id,hint:'正值提前，负值延后'});
    addRow(lyricsSection,'换行动画',[curveInput,curveSeg.group],{labelFor:curveInput.id});
    addRow(lyricsSection,'错峰滚动',staggerInput,{labelFor:staggerInput.id});
    addRow(lyricsSection,'逐字高亮',[wordModeInput,wordModeSeg.group],{labelFor:wordModeInput.id});
    const mode=makeSelect('miniLyrics-background-mode',[[0,'现有渐变'],[1,'Lyricify · 标准'],[2,'Lyricify · 明显流动'],[3,'Lyricify · 暗色']],background.mode);
    const animated=makeSwitch('miniLyrics-background-animated',background.animated);
    const speedChoices=[[.5,'慢速'],[1,'标准'],[2,'快速']];
    const speed=makeSelect('miniLyrics-background-speed',speedChoices,background.speed);
    const speedSeg=makeSegmented(speed,speedChoices);
    const rate=makeSelect('miniLyrics-background-fps',[[10,'10 FPS'],[15,'15 FPS'],[30,'30 FPS'],[45,'45 FPS'],[60,'60 FPS（默认）']],background.fps);
    const rateSeg=makeSegmented(rate,[[10,'10'],[15,'15'],[30,'30'],[45,'45'],[60,'60']]);
    const refreshControls=()=>{animated.disabled=background.mode===0;speed.disabled=rate.disabled=background.mode===0||!background.animated;speedSeg.sync();rateSeg.sync();};
    mode.onchange=()=>{changeBackground({mode:Number(mode.value)});refreshControls();};
    animated.onchange=()=>{changeBackground({animated:animated.checked});refreshControls();};
    speed.onchange=()=>{changeBackground({speed:Number(speed.value)});speedSeg.sync();};
    rate.onchange=()=>{changeBackground({fps:Number(rate.value)});rateSeg.sync();};
    addRow(backgroundSection,'背景样式',mode,{labelFor:mode.id});
    addRow(backgroundSection,'动态流动',animated,{labelFor:animated.id});
    addRow(backgroundSection,'流动速度',[speed,speedSeg.group],{labelFor:speed.id});
    addRow(backgroundSection,'背景帧率',[rate,rateSeg.group],{labelFor:rate.id,hint:'FPS · 仅动态流动时生效'});
    refreshControls();
    el.append(lyricsSection,backgroundSection);
    const footer=create('footer','ml-footer'),links=create('div','ml-links');
    footer.append(create('p','ml-help','拖动封面移动小窗，右下角缩放。滚轮回看歌词，点击歌词跳转。右键小窗可调整翻译、置顶和帧率。'),links);
    for(const [label,url]of [['源码与许可证','https://github.com/shoebillback/MiniLyrics'],['反馈问题','https://github.com/shoebillback/MiniLyrics/issues']]){
      const link=create('a',null,label);link.href=url;
      link.onclick=event=>{event.preventDefault();betterncm.ncm.openUrl(url);};links.append(link);
    }
    el.append(footer);refreshStatus(true);return el;
  });
  window.__miniLyrics={open,dispose,getStatus:()=>status,getDiagnostics:()=>({version:MINILYRICS_VERSION,status,songId:song.id,ncmId:song.ncmId,
    progressSource:player?'网易云播放器':'未同步',positionMs:live(),playing:clock.playing,valid:clock.valid,lyricCount:publishedLyrics?.lines.length??0,
    lyricMode:lyricState.mode,lyricError:lyricState.error,lyricTransport:lyricState.transport,lyricSource:lyricStatus[lyricState.mode],lyricRevision,lyricId:song.lyricId??'',offset,curve,stagger,wordMode,
    host:hostState,playerVersion:player?.version??null,playerFresh:playerTimelineFresh(),playerDiagnostics:playerAdapter?.getDiagnostics?.()??null,
    seekPending:!!pendingSeek,seekRequest:lastSeekId,playbackState:playback.snapshot(),fontSize,background:{...background}})};
  listen(window,'focus',resyncPage);
  listen(document,'visibilitychange',resyncPage);
  listen(window,'beforeunload',dispose);
})();
