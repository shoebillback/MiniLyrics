const assert=require('node:assert/strict');
const {test}=require('node:test');
const {fixture}=require('./bridge-fixture.cjs');

test('playbar shortcut mounts between volume and more and follows the shared window session',async()=>{
  let volume,more,notify,frame,observing=false,shortcut;
  const controls=[];
  const head={append:style=>{style.isConnected=true;}};
  const row={contains:element=>controls.includes(element.parentElement)};
  const footer={
    contains:element=>controls.includes(element),
    querySelector(selector){
      if(selector.includes('morefunctions'))return more?.icon;
      if(selector.includes('cmd-icon-volume'))return volume?.icon;
      throw Error('unexpected selector '+selector);
    }
  };
  function node(){
    return {
      attributes:{},
      get isConnected(){return controls.includes(this);},
      remove(){const i=controls.indexOf(this);if(i!==-1)controls.splice(i,1);},
      setAttribute(name,value){this.attributes[name]=value;}
    };
  }
  function nativeControl(){
    const control=Object.assign(node(),{
      parentElement:row,
      contains(element){return element===this.icon;},
      get previousElementSibling(){return controls[controls.indexOf(this)-1];},
      get nextElementSibling(){return controls[controls.indexOf(this)+1];},
      before(button){button.remove();controls.splice(controls.indexOf(this),0,button);},
      after(button){button.remove();controls.splice(controls.indexOf(this)+1,0,button);}
    });
    control.icon={parentElement:control,contains:()=>false,closest:()=>control};
    return control;
  }
  function setControls({includeMore=true}={}){
    volume=nativeControl();
    more=includeMore?nativeControl():null;
    controls.splice(0,controls.length,volume,...(more?[more]:[]));
    update();
  }
  function update(){notify();const run=frame;frame=null;run?.();}
  const playbar={
    document:{
      documentElement:{},head,
      querySelectorAll(selector){
        assert.equal(selector,'footer');
        return [footer];
      },
      createElement(tag){
        if(tag==='button')return shortcut=node();
        assert.equal(tag,'style');
        return {remove(){this.isConnected=false;}};
      }
    },
    MutationObserver:class{
      constructor(callback){notify=callback;}
      observe(){observing=true;}
      disconnect(){observing=false;}
    },
    requestAnimationFrame(callback){frame=callback;return 1;},
    cancelAnimationFrame(){frame=null;}
  };
  const f=fixture({playbar,delaySockets:true});
  async function connect(){
    await new Promise(setImmediate);
    f.advance(200);
    await new Promise(setImmediate);
    f.sockets.at(-1).connect();
    await new Promise(setImmediate);
  }
  try{
    assert.equal(shortcut,undefined,'wait for the native controls');
    setControls();
    const button=shortcut;
    assert.deepEqual(controls,[volume,button,more]);
    assert.equal(button.title,'打开 MiniLyrics 小窗');

    button.onclick();
    assert.equal(button.attributes['aria-disabled'],'true');
    button.onclick();
    assert.equal(f.launches.length,1);
    await connect();
    assert.equal(button.attributes['aria-pressed'],'true');
    assert.equal(button.title,'关闭 MiniLyrics 小窗');

    setControls();
    update();
    assert.deepEqual(controls,[volume,button,more],'reuse one button when the host rebuilds the playbar');
    assert.equal(button.attributes['aria-pressed'],'true');

    f.closeSocket({code:1000,reason:'host-closed'});
    assert.equal(button.attributes['aria-pressed'],'false');
    assert.equal(button.title,'打开 MiniLyrics 小窗');

    button.onclick();
    await connect();
    button.onclick();
    assert.equal(f.sent.at(-1).type,'exit');
    assert.equal(button.attributes['aria-pressed'],'false');

    volume.remove();more.remove();volume=more=null;
    update();
    assert.equal(button.isConnected,false);
    setControls({includeMore:false});
    assert.deepEqual(controls,[volume,button],'stay beside volume when the more menu is absent');
    setControls();
    assert.deepEqual(controls,[volume,button,more]);
  }finally{f.dispose();}
  assert.equal(observing,false);
  assert.deepEqual(controls,[volume,more]);
});
