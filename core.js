/* MiniLyrics, GPL-3.0-or-later. Pure normalization shared by plugin and tests. */
(function (root) {
  'use strict';
  const finite = (x, fallback = 0) => Number.isFinite(Number(x)) ? Number(x) : fallback;
  const wellFormed = value => {
    const s = String(value);
    return typeof s.toWellFormed === 'function'
      ? s.toWellFormed()
      : s.replace(/[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/g, '\uFFFD');
  };
  function normalizeLines(input, options = {}) {
    if (!Array.isArray(input)) return [];
    const lines = input.slice(0, 5000).filter(l => l && typeof l==='object').map(l => {
      const time = Math.max(0, finite(l.time ?? l.startTime ?? l.start));
      let text = wellFormed(l.originalLyric ?? l.lyric ?? l.text ?? l.content ?? '');
      const words = (Array.isArray(l.dynamicLyric) ? l.dynamicLyric : []).slice(0, 500).filter(w => w && typeof w==='object').map(w => {
        const t = finite(w.time);
        return {text: wellFormed(w.word ?? w.text ?? ''), time: options.wordTiming === 'relative' ? time + t : t,
          duration: Math.max(1, finite(w.duration, 1))};
      });
      if (!text) text = words.map(w => w.text).join('');
      const translation = wellFormed(l.translatedLyric ?? l.translation ?? '');
      return {time, duration: Math.max(0, finite(l.duration)), text, isInterlude: !!l.isInterlude || (!text && !translation),
        translation, words};
    });
    return options.unsynced ? lines : lines.sort((a,b) => a.time-b.time);
  }
  function metadata(info) {
    if(!info || typeof info!=='object') return null;
    const ncmId=finite(info.ncmId);
    const id=wellFormed(info.trackId??(ncmId>0?ncmId:''));
    if(!id)return null;
    return {id,ncmId:ncmId>0?String(ncmId):'',title:wellFormed(info.songName??''),
      artist:wellFormed(info.authorName??''),cover:wellFormed(info.cover?.url??''),
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
    return {sample,event:value=>report(value,true),reset,resetProgress:()=>{point=null;run=null;inferred=false;lastMotionAt=-Infinity;output=reported===true;},
      current:()=>output,snapshot:()=>({reportedPlaying:reported,playing:output,inferredFromProgress:inferred,
        progressSamples:run?.steps??0})};
  }
  // Minimal LRC reader used only as a fallback when RNP exposes no lyrics
  // (e.g. NetEase sitting in its tray/mini player). Metadata tags are ignored.
  function parseLrc(text){
    const out=[],seen=new Set(),head=/^\[(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?\]/;
    for(const raw of String(text??'').split(/\r?\n/)){
      let rest=raw,times=[],m;
      while((m=head.exec(rest))){
        times.push(((+m[1])*60+(+m[2]))*1000+(m[3]?Number((m[3]+'000').slice(0,3)):0));
        rest=rest.slice(m[0].length);
      }
      const body=rest.trim();
      if(!times.length||!body)continue;
      for(const time of times){
        const key=time+'|'+body;
        if(seen.has(key))continue;
        seen.add(key);out.push({time,text:body});
      }
    }
    return out.sort((a,b)=>a.time-b.time);
  }
  // Merge the original and translated LRC by their timestamps.
  function mergeLyrics(text,translatedText){
    const translated=new Map();
    for(const line of parseLrc(translatedText)){
      const key=Math.round(line.time/10);
      if(!translated.get(key))translated.set(key,line.text);
    }
    return parseLrc(text).map(line=>({time:line.time,text:line.text,translation:translated.get(Math.round(line.time/10))||''}));
  }
  const api={finite, wellFormed, normalizeLines, metadata, createPlaybackState, parseLrc, mergeLyrics};
  if(typeof module!=='undefined') module.exports=api; else root.MiniLyricsCore=api;
})(typeof window!=='undefined'?window:globalThis);
