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
