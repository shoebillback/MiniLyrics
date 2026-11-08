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
  const api={finite, normalizeLines, metadata};
  if(typeof module!=='undefined') module.exports=api; else root.MiniLyricsCore=api;
})(typeof window!=='undefined'?window:globalThis);
