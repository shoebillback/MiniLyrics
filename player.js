/* MiniLyrics — GPL-3.0-or-later. NCM 3.x playback boundary; no shared function patches. */
(function(root){
  'use strict';
  function createPlayerAdapter(env=root){
    let store=null,bridge=null,nativeAdapter=null,unsubscribe=null,disposed=false,starting=false,retry=null;
    let song=null,status='Paused',timeline=null,sequence=0,lastStoreState=null;
    const events=new Map(),bindings=new Map(),eventNames=['PlayProgress','Seek','PlayState'];
    const now=()=>env.performance?.now?.()??Date.now();
    let lastAcceptedProgressAt=null,lastAcceptedSeekAt=null,bridgeSource=null;
    let audioPlayerAvailable=false,canQueueLoadingSeek=false,startTarget=null,resumeTrack=null,startFlushScheduled=false,controlSerial=0,lastControl=null;
    const sessionReady=p=>!!p?.playId&&!p.restoreResource&&!p.isLoadingFirst;
    function controlState(){
      const p=store?.getState()?.playing;
      return {phase:!p?'unavailable':p.isLoadingFirst?'loading':p.restoreResource?'restoring':p.playId?'loaded':'idle',
        hasPlayId:!!p?.playId,audioPlayerAvailable,canQueueLoadingSeek,pendingTargetMs:startTarget?.target??null,
        lastCommand:lastControl?{...lastControl}:null};
    }
    function cancelSeek(requestId,resetStart=false){
      if(requestId==null||startTarget?.requestId===requestId)startTarget=null;
      if(resetStart)resumeTrack=null;
    }
    function scheduleStartupSeek(){
      if(disposed||!startTarget||startFlushScheduled||!sessionReady(store?.getState()?.playing))return;
      startFlushScheduled=true;
      // Finish the current native/store event fanout before issuing a seek.
      // This is an ordering boundary, not a timed guess about readiness.
      Promise.resolve().then(()=>{
        startFlushScheduled=false;const own=startTarget,p=store?.getState()?.playing;
        if(disposed||!own||!sessionReady(p))return;
        if(String(p.resourceTrackId)!==own.trackId){startTarget=null;return;}
        startTarget=null;resumeTrack=null;
        lastControl={action:'seek',route:'loaded-after-start',targetMs:own.target,at:now()};
        try{dispatch('playing/setPlayingPosition',{duration:own.target/1000});}
        catch(e){lastControl.error='seek-dispatch-failed';emit('controlError',{requestId:own.requestId,songId:own.trackId,reason:'播放器跳转失败'});}
      });
    }
    const nativeStats=Object.fromEntries(eventNames.map(name=>[name,{received:0,accepted:0,filtered:0,lastReceivedAt:null,lastAcceptedAt:null,lastSongId:null,lastOutcome:null,lastFilterReason:null}]));
    function receive(name,id){
      const event=nativeStats[name];event.received++;event.lastReceivedAt=now();event.lastSongId=id==null?null:String(id).slice(0,256);event.lastOutcome='received';return event;
    }
    function outcome(event,reason=null){
      event.lastOutcome=reason?'filtered':'accepted';event.lastFilterReason=reason;
      if(reason)event.filtered++;else{event.accepted++;event.lastAcceptedAt=now();}
    }
    function diagnostics(){
      const subscriptions=Object.fromEntries(eventNames.map(name=>{
        const own=bindings.get(name),entry=nativeAdapter?.registerCallMap?.['audioplayer.on'+name];
        const active=!!own&&(entry===own||Array.isArray(entry)&&entry.includes(own));
        return [name,active?'subscribed':own?'lost':'waiting'];
      }));
      const canControl=!!store&&!disposed,progress=nativeStats.PlayProgress;
      const streamState=!canControl?'waiting-player':subscriptions.PlayProgress==='lost'?'subscription-lost':subscriptions.PlayProgress!=='subscribed'?'waiting-subscription':
        progress.lastOutcome==='filtered'?'events-filtered':lastAcceptedProgressAt===null?'waiting-progress':status==='Playing'&&now()-lastAcceptedProgressAt>=4500?'progress-stalled':status==='Playing'?'progress-live':'progress-paused';
      return {canControl,bridgeSource,originalInitialization:'unverified',subscriptions,streamState,
        hasActualProgress:lastAcceptedProgressAt!==null,lastReceivedProgressAt:progress.lastReceivedAt,lastAcceptedProgressAt,lastAcceptedSeekAt,
        progressAgeMs:lastAcceptedProgressAt===null?null:Math.max(0,now()-lastAcceptedProgressAt),
        timelineSource:timeline?.source??null,control:controlState(),events:Object.fromEntries(eventNames.map(name=>[name,{...nativeStats[name]}]))};
    }
    const emit=(name,detail)=>{for(const fn of [...events.get(name)??[]])try{fn({detail});}catch(e){env.console?.warn('[MiniLyrics] 播放订阅处理失败',e);}};
    const readSong=()=>{
      const state=store.getState(),p=state?.playing;if(!p?.resourceTrackId)return null;
      const voice=state['page:vinylPage']?.currentVoice;
      if(p.resourceType==='voice'&&(!voice||String(voice.id)!==String(p.onlineResourceId)))return null;
      const v=p.resourceType==='voice'?voice:null;
      const online=v?.id??p.onlineResourceId??p.resourceTrackId;
      return {trackId:String(p.resourceTrackId),ncmId:/^\d+$/.test(String(online))?Number(online):0,
        songName:v?.name??p.resourceName??'',authorName:(v?.track?.artists??p.resourceArtists??[]).map(a=>a.name).join(' / '),
        cover:{url:v?.coverUrl??p.resourceCoverUrl??''},duration:Math.max(0,Number(v?.duration??p.curTrack?.duration)||0)};
    };
    function sync(){
      if(disposed||!store)return;
      bindEvents();
      const next=readSong();if(!next)return;
      const changed=next.trackId!==song?.trackId,meta=JSON.stringify(next)!==JSON.stringify(song);
      if(changed){timeline=null;sequence++;lastStoreState=null;lastAcceptedProgressAt=null;lastAcceptedSeekAt=null;cancelSeek(null,true);}
      song=next;
      // Redux uses 2 for playing; native PlayState uses a different enum.
      const state=store.getState()?.playing?.playingState;
      const known=[-1,0,1,2].includes(state),nextStatus=state===2?'Playing':'Paused';
      const stateChanged=known&&(changed||state!==lastStoreState);
      if(stateChanged){status=nextStatus;lastStoreState=state;}
      if(timeline)timeline={...timeline,totalTime:song.duration};
      if(meta)emit('songChange',song);
      if(stateChanged)emit('playStateChange',status);
      if(sessionReady(store.getState()?.playing))resumeTrack=null;
      scheduleStartupSeek();
    }
    function matches(id){
      if(!song||String(store?.getState()?.playing?.resourceTrackId)!==song.trackId||id==null||id==='')return false;
      const value=String(id).toLowerCase();
      return [song.trackId,song.ncmId>0?String(song.ncmId):''].filter(Boolean).some(key=>{
        key=key.toLowerCase();return value===key||value.startsWith(key+'_')||value.startsWith(key+'|');
      });
    }
    function progress(id,seconds,seek=false,source=seek?'seek':'progress',code=0){
      const name=source==='progress'?'PlayProgress':source==='seek'?'Seek':null,event=name?receive(name,id):null;
      sync();
      const reason=!song?'no-song':!matches(id)?'song-mismatch':seek&&code!==0?'seek-failed':typeof seconds!=='number'||!Number.isFinite(seconds)||seconds<0?'invalid-time':null;
      if(event)outcome(event,reason);if(reason)return;
      if(source==='progress')lastAcceptedProgressAt=now();else if(source==='seek')lastAcceptedSeekAt=now();
      timeline={songId:song.trackId,currentTime:seconds*1000,totalTime:song.duration,sequence:++sequence,seek,source,receivedAt:event?.lastReceivedAt??null};
      if(source!=='restore'&&startTarget?.trackId===song.trackId&&Math.abs(timeline.currentTime-startTarget.target)<=250)startTarget=null;
      emit('timelineUpdate',timeline);
    }
    const isolate=(fn,name)=>(...args)=>{if(disposed)return;try{return fn(...args);}catch(e){if(name)outcome(nativeStats[name],'callback-error');env.console?.warn('[MiniLyrics] 播放回调失败，已隔离',e);}};
    function bind(name,fn){
      const entry=nativeAdapter?.registerCallMap?.['audioplayer.on'+name],current=bindings.get(name);
      if(current&&Array.isArray(entry)&&entry.includes(current))return;
      // APPEND reserves an absent event and can reject the player's later first
      // registration. Only join an existing callback; never create an empty slot.
      const existing=Array.isArray(entry)?entry.some(cb=>typeof cb==='function'&&cb!==current):typeof entry==='function'&&entry!==current;
      if(!existing)return;
      const callback=current??isolate(fn,name);
      bridge.appendRegisterCall(name,'audioplayer',callback);bindings.set(name,callback);
    }
    function bindEvents(){
      if(disposed||!bridge)return;
      bind('PlayProgress',(id,seconds)=>progress(id,seconds));
      bind('Seek',(id,request,code,seconds)=>progress(id,seconds,true,'seek',code));
      bind('PlayState',(id,request,value)=>{
        const event=receive('PlayState',id);sync();
        const reason=!matches(id)?'song-mismatch':![0,1,2,4].includes(value)?'invalid-state':null;
        outcome(event,reason);if(reason)return;
        status=value===1?'Playing':'Paused';emit('playStateChange',status);
      });
    }
    function release(){
      unsubscribe?.();unsubscribe=null;
      for(const [name,fn]of bindings)try{bridge.removeRegisterCall(name,'audioplayer',fn);}catch(e){env.console?.warn('[MiniLyrics] 清理播放订阅失败',e);}
      bindings.clear();store=null;bridge=null;nativeAdapter=null;
    }
    function find(require,predicate){
      for(const module of Object.values(require.c??{})){const e=module?.exports;for(const candidate of [e,e?.default])if(candidate&&predicate(candidate))return candidate;}
      return null;
    }
    function initialize(require){
      if(disposed)return;
      const dva=find(require,e=>e.a?.app?._store?.getState&&e.a.app._store.subscribe&&e.a.app._store.dispatch);
      // Like InfLink's V3 adapter, resolve AudioPlayer before subscribing through Bridge.
      audioPlayerAvailable=!!find(require,e=>typeof e.AudioPlayer?.subscribePlayStatus==='function');
      const model=dva?.a.app._models?.find(m=>m.namespace==='playing'),reducers=model?.reducers;
      canQueueLoadingSeek=typeof (reducers?.['playing/onLoadingSeekUpdate']??reducers?.onLoadingSeekUpdate)==='function';
      const native=find(require,e=>typeof e.Bridge?.appendRegisterCall==='function'&&typeof e.Bridge?.removeRegisterCall==='function')?.Bridge;
      const selected=native??env.legacyNativeCmder,adapter=selected?._Adapter??selected?._envAdapter;
      if(!dva||!adapter?.registerCallMap||typeof selected?.appendRegisterCall!=='function'||typeof selected?.removeRegisterCall!=='function')return;
      store=dva.a.app._store;bridge=selected;nativeAdapter=adapter;bridgeSource=native?'internal':'legacy';
      try{
        sync();unsubscribe=store.subscribe(isolate(sync));
        // Event binding is retried by sync/polling after native initialization.
        // Restore only a matching startup snapshot, never overwrite a newer event.
        const storage=find(require,e=>typeof e.b?.lastPlaying?.get==='function')?.b;
        const initial=sequence,id=song?.trackId;
        Promise.resolve(storage?.lastPlaying.get()).then(saved=>{
          if(disposed||sequence!==initial||song?.trackId!==id||String(saved?.trackId)!==id)return;
          progress(id,saved.current,false,'restore');
        }).catch(()=>{});
      }catch(e){release();throw e;}
    }
    function start(){
      if(disposed||store||starting)return;
      starting=true;
      try{
        const chunks=env.webpackJsonp;
        if(chunks?.push){
          const key='minilyrics_player_'+Date.now()+'_'+Math.random().toString(36).slice(2);
          const modules={[key]:(module,exports,require)=>initialize(require)};
          chunks.push(chunks[0]&&Array.isArray(chunks[0])?[[key],modules,[[key]]]:[[key],modules]);
        }
      }catch(e){env.console?.debug('[MiniLyrics] 等待播放器就绪',e);}
      finally{starting=false;if(!disposed&&!store)retry=env.setTimeout(start,1000);}
    }
    function dispatch(type,payload){
      if(disposed||!store)return false;
      store.dispatch({type,payload});
      // A synchronous first seek may initialize native callbacks without a store
      // notification. Join them now; a confirmation already missed is not replayed.
      bindEvents();
      return true;
    }
    function seekTo(ms,requestId){
      if(disposed||!store||!Number.isFinite(ms)||ms<0)return false;
      sync();const p=store.getState()?.playing,trackId=String(p?.resourceTrackId??'');
      if(!trackId||trackId!==song?.trackId)return false;
      if(sessionReady(p)){
        cancelSeek();lastControl={action:'seek',route:'loaded',targetMs:ms,at:now()};
        return dispatch('playing/setPlayingPosition',{duration:ms/1000});
      }
      // The live NCM effects queue positions in restoreResource/current or
      // loadingSeekDuration. Never send an idle session's missing playId to seek.
      const needsLoadingQueue=!p.restoreResource&&!p.isLoadingFirst;
      const clearZeroFallback=!!p.restoreResource&&ms===0&&Number(p.loadingSeekDuration)>0;
      if((!p.isLoadingFirst&&!p.curPlaying)||((needsLoadingQueue||clearZeroFallback)&&!canQueueLoadingSeek)){
        lastControl={action:'seek',route:'unavailable',targetMs:ms,at:now()};return false;
      }
      const own={requestId:requestId??++controlSerial,trackId,target:ms};startTarget=own;
      lastControl={action:'seek',route:'queued-start',targetMs:ms,at:now()};
      try{
        if(p.restoreResource||p.isLoadingFirst)dispatch('playing/setPlayingPosition',{duration:ms/1000});
        else dispatch('playing/onLoadingSeekUpdate',{loadingSeekDuration:ms/1000});
        // nativePlay uses restore.current || loadingSeekDuration: zero must
        // also clear an older loading target rather than fall back to it.
        if(clearZeroFallback)dispatch('playing/onLoadingSeekUpdate',{loadingSeekDuration:0});
        if(!p.isLoadingFirst&&resumeTrack!==trackId){resumeTrack=trackId;dispatch('playing/resume',{triggerScene:'desktopLyric'});}
        scheduleStartupSeek();return true;
      }catch(e){cancelSeek(own.requestId,true);throw e;}
    }
    function playbackControl(type,payload){
      cancelSeek(null,true);lastControl={action:type,route:'original-model',at:now()};return dispatch(type,payload);
    }
    const api={version:'ncm3-v3',get ready(){return !!store&&!disposed;},getDiagnostics:diagnostics,
      getCurrentSong:()=>{sync();return song;},getPlaybackStatus:()=>status,getTimeline:()=>{bindEvents();scheduleStartupSeek();return timeline;},
      addEventListener:(name,fn)=>{if(!events.has(name))events.set(name,new Set());events.get(name).add(fn);},
      removeEventListener:(name,fn)=>events.get(name)?.delete(fn),
      play:()=>playbackControl('playing/resume',{triggerScene:'desktopLyric'}),pause:()=>playbackControl('playing/pause',{triggerScene:'desktopLyric'}),
      togglePlay:()=>playbackControl('playing/switchResumeOrPause',{triggerScene:'desktopLyric'}),
      next:()=>playbackControl('playingList/jump2Track',{flag:1,type:'call',triggerScene:'hotKey'}),
      previous:()=>playbackControl('playingList/jump2Track',{flag:-1,type:'call',triggerScene:'hotKey'}),
      seekTo,cancelSeek,
      dispose:()=>{cancelSeek(null,true);disposed=true;env.clearTimeout(retry);release();events.clear();timeline=null;song=null;}};
    start();return api;
  }
  if(typeof module!=='undefined')module.exports={createPlayerAdapter};else root.MiniLyricsPlayer={createPlayerAdapter};
})(typeof window!=='undefined'?window:globalThis);
