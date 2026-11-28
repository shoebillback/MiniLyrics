/* MiniLyrics — GPL-3.0-or-later. NetEase lyric transport and parsing. */
(function(root){
  'use strict';
  const core=typeof module!=='undefined'&&module.exports?require('./core.js'):root.MiniLyricsCore;
  const maxLines=5000,maxWords=500;
  const lrcTime=/^\[(\d{1,5}):([0-5]?\d)(?:[.:](\d+))?\]/;
  const lrcInfo=/^\[(?:ar|al|ti|au|by|re|ve|length|offset)\s*:/i;
  const yrcLine=/^\[(\d{1,10}),(\d{1,10})\](.*)$/;
  const instrumental=/^(?:纯音乐\s*[,，。]?\s*请欣赏|纯音乐\s*[,，。]?\s*无歌词)[。！!]?$/;
  const lrcCredit=/^(?:作[词詞曲]|[词詞]曲|编曲|編曲|原唱|制作人|製作人)\s*[:：]\s*\S/;

  function isNeteaseCredit(row){
    if(!row.startsWith('{'))return false;
    try{const credit=JSON.parse(row);return Number.isFinite(credit.t)&&Array.isArray(credit.c);}
    catch{return false;}
  }

  const cjkTail=/[\p{Script=Han}\p{Script=Hiragana}\p{Script=Katakana}\p{Script=Hangul}ー]/u;

  function markLongNotes(words){
    let trailing=null,lastVocal=null;
    for(const word of words){
      const text=word.text.trim();
      if(/[\p{L}\p{N}]/u.test(text)){
        trailing=word;lastVocal=word;
        if(word.duration>=1000){
          const length=Array.from(text.replace(/\s/gu,'')).length;
          if(length>=2&&length<=7)word.emphasize=true;
        }
      }
      const phraseEnd=/[,.!?，。！？、；：;:…—~～]$/u.test(text)||(/\s$/u.test(word.text)&&!/[a-z]/i.test(text));
      if(phraseEnd){
        if(trailing?.duration>=1000)trailing.longNote=true;
        trailing=null;
      }
    }
    if(trailing?.duration>=1000)trailing.longNote=true;
    if(lastVocal&&lastVocal.duration>=380&&cjkTail.test(lastVocal.text))lastVocal.longNote=true;
    if(lastVocal?.longNote)lastVocal.lineTail=true;
  }

  function parseLrc(raw){
    const offset=Number(raw.match(/^\s*\[offset:\s*([+-]?\d{1,10})\s*\]/im)?.[1]??0);
    const lines=[],plain=[];
    for(let row of raw.split(/\r?\n/)){
      row=row.trim();
      if(!row||lrcInfo.test(row)||isNeteaseCredit(row))continue;
      const times=[];
      for(let tag;(tag=lrcTime.exec(row))&&times.length<maxLines;){
        const time=(Number(tag[1])*60+Number(tag[2]))*1000+Number((tag[3]??'').slice(0,3).padEnd(3,'0'));
        times.push(Math.max(0,time-offset));row=row.slice(tag[0].length).trimStart();
      }
      if(!times.length){if(plain.length<maxLines)plain.push({text:row});continue;}
      for(const time of times){if(lines.length>=maxLines)break;lines.push({time,text:row});}
      if(lines.length>=maxLines)break;
    }
    return lines.length?{untimed:false,lines:lines.sort((a,b)=>a.time-b.time)}:{untimed:!!plain.length,lines:plain};
  }

  function parseYrc(raw){
    const lines=[];
    for(const input of raw.split(/\r?\n/)){
      if(lines.length>=maxLines)break;
      const header=yrcLine.exec(input.trimStart());
      if(!header)continue;
      const [,start,duration,body]=header;
      const text=body.trimStart();
      if(!text){lines.push({time:Number(start),duration:Number(duration),text:''});continue;}
      const tags=[];
      for(const tag of text.matchAll(/\((\d{1,10}),(\d{1,10}),\d{1,10}\)/g)){
        tags.push(tag);
        if(tags.length>maxWords)break;
      }
      if(!tags.length||tags[0].index!==0||tags.length>maxWords)continue;
      const words=tags.map((tag,i)=>({time:Number(tag[1]),duration:Number(tag[2]),
        text:text.slice(tag.index+tag[0].length,tags[i+1]?.index??text.length)}));
      markLongNotes(words);
      lines.push({time:Number(start),duration:Number(duration),words,text:words.map(word=>word.text).join('')});
    }
    return lines.sort((a,b)=>a.time-b.time);
  }

  // Prefer strong text matches, then more pairs, then less total time drift.
  // Translation pairs all have the same quality since their text differs.
  function alignPairs(left,right,tolerance,quality){
    if(left[left.length-1].time<right[0].time-tolerance||right[right.length-1].time<left[0].time-tolerance)return [];
    const stride=right.length+1;
    const decisions=new Uint8Array((left.length+1)*stride);
    const score=new Float64Array(stride);
    const maxPairs=Math.min(left.length,right.length);
    const matchValue=tolerance*maxPairs+1;
    const strongBonus=matchValue*(maxPairs+1);
    for(let i=1;i<=left.length;i++){
      let diagonal=0;
      for(let j=1;j<=right.length;j++){
        const above=score[j],distance=Math.abs(left[i-1].time-right[j-1].time);
        let best=above,step=1;
        if(score[j-1]>best){best=score[j-1];step=2;}
        if(distance<=tolerance){
          const strength=quality(left[i-1],right[j-1],distance);
          const matched=diagonal+matchValue-distance+strength*strongBonus;
          if(strength>=0&&matched>=best){best=matched;step=3;}
        }
        decisions[i*stride+j]=step;diagonal=above;score[j]=best;
      }
    }
    const pairs=[];
    for(let i=left.length,j=right.length;i>0&&j>0;){
      const step=decisions[i*stride+j];
      if(step===3){pairs.push([--i,--j]);}
      else if(step===1)i--;
      else j--;
    }
    return pairs.reverse();
  }

  function matchCluster(left,right,tolerance,quality,onMatch){
    if((left.length+1)*(right.length+1)<=250000){
      for(const [a,b] of alignPairs(left,right,tolerance,quality))onMatch(left[a],right[b]);
      return;
    }
    // Dense clusters use overlapping windows. Commit the earlier prefix
    // while keeping the rest as context for the next window.
    const prefix=32;
    for(let i=0,j=0;i<left.length&&j<right.length;){
      const a=left.slice(i,i+2*prefix),b=right.slice(j,j+2*prefix);
      const aCut=Math.min(prefix,a.length),bCut=Math.min(prefix,b.length);
      const advanceLeft=a[aCut-1].time<=b[bCut-1].time;
      let nextI=i+(advanceLeft?aCut:0),nextJ=j+(advanceLeft?0:bCut);
      for(const [x,y] of alignPairs(a,b,tolerance,quality)){
        if(advanceLeft?x>=aCut:y>=bCut)break;
        onMatch(a[x],b[y]);
        nextI=Math.max(nextI,i+x+1);nextJ=Math.max(nextJ,j+y+1);
      }
      i=nextI;j=nextJ;
    }
  }

  function matchTimedLines(left,right,tolerance,quality,onMatch){
    let startLeft=0,startRight=0,i=0,j=0,previous=-Infinity;
    while(i<left.length||j<right.length){
      const leftTime=left[i]?.time??Infinity,rightTime=right[j]?.time??Infinity;
      const time=Math.min(leftTime,rightTime);
      if(time-previous>tolerance){
        if(i>startLeft&&j>startRight)matchCluster(left.slice(startLeft,i),right.slice(startRight,j),tolerance,quality,onMatch);
        startLeft=i;startRight=j;
      }
      if(leftTime<=rightTime)i++;
      else j++;
      previous=time;
    }
    if(i>startLeft&&j>startRight)matchCluster(left.slice(startLeft),right.slice(startRight),tolerance,quality,onMatch);
  }

  function oneEditApart(a,b){
    if(Math.min(a.length,b.length)<4||Math.abs(a.length-b.length)>1)return false;
    if(a.length>b.length)[a,b]=[b,a];
    let i=0;
    while(i<a.length&&a[i]===b[i])i++;
    return a.slice(i+(a.length===b.length?1:0))===b.slice(i+1);
  }

  function mergeTimedLyrics(lrc,yrc){
    const firstVocal=yrc.find(line=>line.words&&line.text.trim()).time;
    lrc=lrc.filter(line=>line.time>=firstVocal||!lrcCredit.test(line.text));
    const matchKey=line=>({line,time:line.time,text:line.text.trim(),
      key:line.text.normalize('NFKC').toLowerCase().replace(/[\s\p{P}]/gu,'')});
    const covered=new Set();
    matchTimedLines(lrc.map(matchKey),yrc.map(matchKey),1000,(a,b,distance)=>{
      if(a.key===b.key&&(a.key!==''||distance===0&&a.text===b.text))return 1;
      if(a.text&&b.text&&(distance===0||distance<=120&&oneEditApart(a.key,b.key)))return 0;
      return -1;
    },a=>covered.add(a.line));
    return [...yrc,...lrc.filter(line=>!covered.has(line))].sort((a,b)=>a.time-b.time);
  }

  function addTranslations(lines,translation){
    const original=lines.filter(line=>line.text.trim());
    const translated=translation.filter(line=>line.text.trim());
    // A lower-priority entry at an already translated timestamp must stay
    // there. Keep those original rows in the alignment for other nearby entries.
    const translatedTimes=new Set(original.filter(line=>line.translation).map(line=>line.time));
    matchTimedLines(original,translated,500,(a,b)=>translatedTimes.has(b.time)&&a.time!==b.time?-1:0,(line,match)=>{
      if(!line.translation)line.translation=match.text;
    });
  }

  function addInterludes(lines){
    const withInterludes=[];
    let slots=Math.max(0,maxLines-lines.length);
    for(let i=0;i<lines.length;i++){
      const line=lines[i],next=lines[i+1];
      withInterludes.push(line);
      if(!slots||!line.words?.length||!line.text.trim()||!(line.duration>0)||!next?.text.trim())continue;
      let start=line.time+line.duration;
      for(const word of line.words){
        if(!word.text.trim())continue;
        start=Math.max(start,word.time+Math.max(0,word.duration));
      }
      const duration=next.time-start;
      if(duration>=4000){
        withInterludes.push({time:start,duration,text:'',isInterlude:true});
        slots--;
      }
    }
    return withInterludes;
  }

  function requestError(message,retryable){
    return Object.assign(new Error(message),{retryable});
  }

  function parseLyrics(payload,{cloud=false}={}){
    if(payload?.data===-400)throw requestError('网易云歌词返回异常（data: -400）',true);
    const raw=key=>{
      const value=payload?.[key];
      return typeof value==='string'?value:typeof value?.lyric==='string'?value.lyric:undefined;
    };
    const original=raw('lrc'),dynamic=raw('yrc');
    const hasCloudLyrics=cloud&&payload?.code==null&&(original!==undefined||dynamic!==undefined||payload?.nolyric===true);
    if(payload?.code!==200&&!hasCloudLyrics)throw requestError('网易云歌词返回异常（code: '+String(payload?.code??'缺失')+'）',true);
    if(payload.nolyric===true)return {mode:'instrumental',lines:[]};
    const lrc=parseLrc(original??''),yrc=parseYrc(dynamic??'');
    const hasWords=yrc.some(line=>line.words&&line.text.trim());
    let {lines,untimed}=hasWords?{lines:mergeTimedLyrics(lrc.untimed?[]:lrc.lines,yrc),untimed:false}:lrc;
    const textLines=lines.filter(line=>line.text.trim());
    if(textLines.length===1&&instrumental.test(textLines[0].text.trim()))return {mode:'instrumental',lines:[]};
    if(!textLines.length||(payload.uncollected===true&&textLines.length===1&&textLines[0].text.trim()==='暂无歌词'))return {mode:'empty',lines:[]};

    if(!untimed){
      for(const field of hasWords?['ytlrc','ttlrc','tlyric']:['tlyric','ytlrc','ttlrc']){
        const text=raw(field);if(!text)continue;
        const translation=parseLrc(text);
        addTranslations(lines,translation.untimed?parseYrc(text):translation.lines);
      }
      if(hasWords)lines=addInterludes(lines);
      for(let i=0;i<lines.length;i++){
        const line=lines[i],next=lines[i+1];
        if(!line.text.trim()){line.text='';line.isInterlude=true;}
        if(!line.duration&&next)line.duration=next.time-line.time;
      }
    }
    return {mode:untimed?'untimed':'timed',lines:core.normalizeLines(lines,{unsynced:untimed})};
  }

  async function fetchLyrics(id,{signal,userId=''}={}){
    const cloud=!!userId;
    if(cloud?(!id||id==='0'||!core.validId(userId)):!core.validId(id))throw requestError('无可查询的网易云歌曲 ID',false);
    const domain=root.APP_CONF?.domain;
    const origin=typeof domain==='string'&&/^https:\/\/(?:[a-z\d-]+\.)?music\.163\.com\/?$/i.test(domain)?domain.replace(/\/$/,''):'https://music.163.com';
    const route=cloud?'/api/cloud/lyric/get?songId='+encodeURIComponent(id)+'&userId='+userId+'&lv=-1&kv=-1':
      '/api/song/lyric/v1?tv=0&lv=0&rv=0&kv=0&yv=0&ytv=0&yrv=0&cp=false&id='+id;
    const response=await root.fetch(origin+route,{signal});
    if(!response.ok){
      const detail=response.statusText?' '+response.statusText:'';
      throw requestError('网易云歌词请求失败（HTTP '+response.status+detail+'）',response.status>=500&&response.status<600);
    }
    return parseLyrics(await response.json(),{cloud});
  }

  const api={fetchLyrics,parseLyrics};
  if(typeof module!=='undefined'&&module.exports)module.exports=api;
  else root.MiniLyricsLyrics=api;
})(typeof window!=='undefined'?window:globalThis);
