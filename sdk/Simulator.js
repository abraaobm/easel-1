/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Modified 2026-10-17: reserve the final flash sector during sample imports.
 * Modified 2026-10-24: expose Easel FM layer (easel_* commands). */
(() => {
  'use strict';
  function coreRuntime() {
    const sameBytes=(a,b)=>{
      if(!a||a.length!==b.length)return false;
      for(let i=0;i<b.length;i++)if(a[i]!==b[i])return false;
      return true;
    };
    const label=bytes=>{
      let text='';for(const byte of bytes){if(!byte)break;text+=String.fromCharCode(byte);}return text;
    };
    return class FM1Core {
      constructor(wasm, flash, snapshot) {
        this.c = new WebAssembly.Instance(new WebAssembly.Module(wasm), {}).exports;
        const c = this.c; c._initialize();
        if (snapshot) {
          if (snapshot.byteLength % 65536 || snapshot.byteLength < c.memory.buffer.byteLength || snapshot.byteLength > 33554432) throw Error('仿真状态无效');
          c.memory.grow((snapshot.byteLength - c.memory.buffer.byteLength) / 65536);
          new Uint8Array(c.memory.buffer).set(new Uint8Array(snapshot));
        } else {
          const saved = flash?.length === 536576;
          if (saved) new Uint8Array(c.memory.buffer,c.sim_flash(),536576).set(flash);
          c.sim_boot(saved ? 0 : 1);
        }
        this.lastFrame = this.lastFlash = -1;
      }
      command(message) {
        const c = this.c; let result = 0, data;
        switch(message.type) {
          case 'control': c.sim_control(message.kind,message.index,message.value); break;
          case 'release': c.sim_release(); break;
          case 'panic': c.sim_panic(); break;
          case 'tapTempo': c.sim_tap_tempo(); break;
          case 'select': result=c.sim_select(message.track,message.source); break;
          
          case 'easel_enter':   result=c.sim_easel_enter(message.track); break;
          case 'easel_leave':   c.sim_easel_leave(message.track); break;
          case 'easel_adjust':  result=c.sim_easel_adjust(message.track,message.page,message.knob,message.delta); break;
          case 'easel_read':
            c.sim_easel_read(message.track);
            data=new Uint8Array(c.memory.buffer,c.sim_easel_read_buf(),4).slice();
            break;
          case 'easel_set_page': c.sim_easel_set_page(message.track,message.page); break;
          case 'easel_page':     data=c.sim_easel_page(message.track); break;

          case 'open': result=c.sim_open(message.page); if(result)throw Error('当前音源不支持此编辑页。'); break;
          case 'preset': result=c.sim_preset(message.index); break;
          case 'step': result=c.sim_step(message.index,message.toggle?1:0); break;
          case 'lcd': result=c.sim_lcd(message.context,message.action,message.index,message.owner,message.delta); break;
          case 'midi':
            if (!message.data || message.data.length>1024) throw Error('MIDI 数据无效');
            new Uint8Array(c.memory.buffer,c.sim_file(),message.data.length).set(message.data);
            result=c.sim_midi(message.data.length); break;
          case 'export':
            result=message.timbre?c.sim_timbre_export():message.patch?c.sim_patch_export():c.sim_export();
            if (!result) data=new Uint8Array(c.memory.buffer,c.sim_file(),message.timbre?223:message.patch?156:4264).slice();
            break;
          case 'snapshot':
            result=c.sim_export();
            if(!result)data={bytes:new Uint8Array(c.memory.buffer,c.sim_file(),4264).slice(),revision:this.projectRevision()};
            break;
          case 'factory':
            result=c.sim_factory_project(message.index);
            if(!result)data=new Uint8Array(c.memory.buffer,c.sim_file(),4264).slice();
            break;
          case 'factories':
            data=[];
            for(let i=0;i<64;i++){
              const at=c.sim_factory_name(i);if(!at)break;
              const bytes=new Uint8Array(c.memory.buffer,at,13);let name='';
              for(const byte of bytes){if(!byte)break;name+=String.fromCharCode(byte);}
              data.push(name);
            }
            break;
          case 'import':
            if (!message.data || (message.timbre?message.data.length!==223:message.patch?message.data.length!==156:![4048,4064,4264].includes(message.data.length))) throw Error('文件长度无效');
            new Uint8Array(c.memory.buffer,c.sim_file(),message.data.length).set(message.data);
            result=message.timbre?c.sim_timbre_import():message.patch?c.sim_patch_import():c.sim_import(message.data.length); break;
          case 'library-read':
            result=c.sim_library_read(message.bank);if(!result)data=new Uint8Array(c.memory.buffer,c.sim_file(),4096).slice();break;
          case 'library-write':
            if(!(message.data instanceof Uint8Array)||message.data.length!==4096)throw Error('音色库长度无效');
            new Uint8Array(c.memory.buffer,c.sim_file(),4096).set(message.data);result=c.sim_library_write(message.bank);break;
          case 'clear': c.sim_clear(); break;
          case 'samples': {
            if (!(message.data instanceof Uint8Array) || message.data.length<512 || message.data.length>446464) throw Error('采样库长度无效');
            const flash=new Uint8Array(c.memory.buffer,c.sim_flash()+69632,466944),previous=flash.slice();
            flash.fill(255,0,446464);flash.set(message.data);
            result=c.sim_sample_bank(message.data.length);
            if(result){
              new Uint8Array(c.memory.buffer,c.sim_flash()+69632,466944).set(previous);
              c.sim_sample_bank(previous.length);
            }
            break;
          }
          case 'tick': for(let i=0;i<Math.min(32,message.blocks>>>0);i++)c.sim_render();break;
          default: throw Error('未知仿真操作');
        }
        if(result)throw Error(result===-8?'内存不足，当前项目保持不变':'当前音源或参数不支持此操作');
        return data;
      }
      projectRevision(){return this.c.sim_edit_revision()+':'+this.c.sim_project_revision();}
      frame(force=false) {
        const c=this.c,message={type:'frame',state:new Uint8Array(c.memory.buffer,c.sim_state(),225).slice()};
        const name=new Uint8Array(c.memory.buffer,c.sim_project_name(),12),position=c.sim_preset_position();
        const targets=c.sim_lcd_targets(),count=c.sim_lcd_count();
        const labels=new Uint8Array(c.memory.buffer,c.sim_knobs(),128);
        const bytes=new Uint8Array(c.memory.buffer,targets,count*64),words=new Int32Array(c.memory.buffer,targets,count*16);
        const first=c.sim_step_first(),context=c.sim_lcd_context(),mask=c.sim_lcd_mask();
        if(!this.screen||this.screen.first!==first||this.screen.context!==context||this.screen.mask!==mask||
            !sameBytes(this.labels,labels)||!sameBytes(this.targets,bytes)){
          this.labels=labels.slice();this.targets=bytes.slice();
          this.screen={first,context,mask,
            knobs:Array.from({length:4},(_,i)=>[0,1].map(field=>label(labels.subarray(i*32+field*16,i*32+field*16+16)))),
            targets:Array.from({length:count},(_,i)=>({
              rect:Array.from(words.subarray(i*16,i*16+4)),action:words[i*16+4],index:words[i*16+5],owner:words[i*16+6],
              label:label(bytes.subarray(i*64+32,i*64+48)),value:label(bytes.subarray(i*64+48,i*64+64))
            }))};
        }
        message.screen=this.screen;
        message.workspace={name:String.fromCharCode(...name).trim(),slot:position&255,factory:(position&768)===768,dirty:!!c.sim_project_dirty(),revision:this.projectRevision()};
        message.countIn=c.sim_count_in();
        const revision=c.sim_frame_revision();
        if(force||revision!==this.lastFrame){
          const changed=c.sim_frame_changes(),bands=force||this.lastFrame<0?32767:changed;
          const source=new Uint16Array(c.memory.buffer,c.sim_pixels(),57600);
          if(bands===32767)message.pixels=source.slice();
          else {
            let count=0;for(let bits=bands;bits;bits&=bits-1)count++;
            message.pixels=new Uint16Array(count*3840);
            for(let band=0,at=0;band<15;){
              if(!(bands&(1<<band))){band++;continue;}
              const first=band;while(band<15&&(bands&(1<<band)))band++;
              const run=source.subarray(first*3840,band*3840);message.pixels.set(run,at);at+=run.length;
            }
          }
          message.pixelBands=bands;this.lastFrame=revision;
        }
        const flash=c.sim_flash_revision();
        if(flash!==this.lastFlash){message.flash=new Uint8Array(c.memory.buffer,c.sim_flash(),536576).slice();this.lastFlash=flash;}
        return message;
      }
    };
  }
  function audioThread(createCore) {
    const Core=createCore();
    class FM1Processor extends AudioWorkletProcessor {
      constructor(options) {
        super(); const config=options.processorOptions;
        this.core=new Core(config.wasm,config.flash,config.snapshot);this.c=this.core.c;
        this.blockFrames=this.c.sim_block_frames();this.position=this.blockFrames;this.fraction=0;this.ratio=44100/sampleRate;this.count=0;
        this.a=new Float32Array(2);this.b=new Float32Array(2);this.resetAudio();
        this.port.onmessage=e=>this.command(e.data);this.publish(true);this.port.postMessage({type:'ready'});
      }
      render() {
        this.audioAt=this.c.sim_render();this.position=0;
        if(!this.buffer||this.buffer.buffer!==this.c.memory.buffer)
          this.buffer=new Float32Array(this.c.memory.buffer,this.audioAt,this.blockFrames*2);
      }
      next(target) {
        if(this.position===this.blockFrames)this.render();
        const at=this.position++*2;target[0]=this.buffer[at];target[1]=this.buffer[at+1];
      }
      resetAudio(){this.position=this.blockFrames;this.fraction=0;this.next(this.a);this.next(this.b);}
      command(message) {
        try {
          const data=this.core.command(message);
          if(['panic','import','clear'].includes(message.type))this.resetAudio();
          if(message.id)this.port.postMessage({id:message.id,data},ArrayBuffer.isView(data)?[data.buffer]:[]);
          this.publish();
        }catch(error){this.port.postMessage({id:message.id,error:error.message});}
      }
      publish(force=false){const message=this.core.frame(force);this.port.postMessage(message,Object.values(message).filter(v=>ArrayBuffer.isView(v)).map(v=>v.buffer));}
      process(inputs,outputs) {
        const out=outputs[0];if(!out?.[0])return true;
        const left=out[0],right=out[1],length=left.length;
        if(this.buffer.buffer!==this.c.memory.buffer)
          this.buffer=new Float32Array(this.c.memory.buffer,this.audioAt,this.blockFrames*2);
        if(this.ratio===1&&length>=2) {
          left[0]=this.a[0];left[1]=this.b[0];
          if(right){right[0]=this.a[1];right[1]=this.b[1];}
          for(let i=2;i<length;) {
            if(this.position===this.blockFrames)this.render();
            const count=Math.min(this.blockFrames-this.position,length-i),start=this.position*2;
            if(right)for(let j=0;j<count;j++){left[i+j]=this.buffer[start+j*2];right[i+j]=this.buffer[start+j*2+1];}
            else for(let j=0;j<count;j++)left[i+j]=this.buffer[start+j*2];
            this.position+=count;i+=count;
          }
          this.next(this.a);this.next(this.b);
        } else for(let i=0;i<length;i++) {
          left[i]=this.a[0]+(this.b[0]-this.a[0])*this.fraction;
          if(right)right[i]=this.a[1]+(this.b[1]-this.a[1])*this.fraction;
          this.fraction+=this.ratio;
          while(this.fraction>=1){this.fraction--;const spare=this.a;this.a=this.b;this.b=spare;this.next(this.b);}
        }
        this.count+=length;if(this.count>=sampleRate/20){this.count%=sampleRate/20;this.publish();}return true;
      }
    }
    registerProcessor('fm1-engine',FM1Processor);
  }
  globalThis.FM1SimulatorCore=coreRuntime();
  globalThis.FM1SimulatorAudioSource='('+audioThread.toString()+')('+coreRuntime.toString()+');';

  if (typeof document === 'undefined') return;
  const $ = id=>document.getElementById(id), sources=['DX7','303','808','PSG','PLUCK','SAMPLER'];
  const storageKey='fm1.simulator.flash.v4',pending=new Map();
  let context,node,gain,analyser,meterData,state,workspace,preview,wasm,ready=false,starting=false,audioOn=false,handoff=false,sequence=0,selectedFile,fileRevision=0,voices=[],midiInput,midiAccess,midiConnecting=false,presetBusy=false;
  const waiting=[],wakingKeys=new Map();let startTask,wakingPlay,muted=false;
  const say=text=>{FM1I18n.text($('sim-status'),text);};
  const image=$('sim-screen').getContext('2d'),pixels=image.createImageData(240,240);
  const rgb5=new Uint8ClampedArray(32),rgb6=new Uint8ClampedArray(64);
  for(let i=0;i<32;i++)rgb5[i]=i*255/31;
  for(let i=0;i<64;i++)rgb6[i]=i*255/63;
  for(let i=3;i<pixels.data.length;i+=4)pixels.data[i]=255;
  function encode(bytes){let text='';for(let i=0;i<bytes.length;i+=16384)text+=String.fromCharCode(...bytes.subarray(i,i+16384));return btoa(text);}
  function decode(text){return Uint8Array.from(atob(text),c=>c.charCodeAt(0));}
  function save(bytes,name){const url=URL.createObjectURL(new Blob([bytes],{type:'application/octet-stream'})),a=document.createElement('a');a.href=url;a.download=name;document.body.append(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(url),1000);}
  function availability() {
    for(const element of ui.controls)element.disabled=!ready;
    $('sim-start').disabled=starting||!ready;
    $('sim-play-control').disabled=starting||!ready;
    $('sim-midi').disabled=!ready||midiConnecting||!navigator.requestMIDIAccess;
    FM1I18n.text($('sim-start'),starting?'正在开启声音…':muted?'取消静音':'静音');
    $('sim-start').setAttribute('aria-pressed',String(muted));
    $('sim-patch-export').disabled=!ready||!state||state[8+state[0]*20]!==0;
    $('sim-file-apply').disabled=!ready||!selectedFile||(selectedFile.patch&&(!state||state[8+state[0]*20]!==0));
    $('sim-placeholder').hidden=ready;
    FM1I18n.text($('sim-engine-state'),ready?'面板已就绪':'正在载入');
    FM1I18n.text($('sim-audio-state'),audioOn?'声音已开启':muted?'静音编辑':'点琴键即可发声');
    $('sim-engine-state').hidden=ready;$('sim-audio-state').hidden=!ready;
    for(const id of ['sim-preset-prev','sim-preset-next','sim-factory','sim-factory-load'])$(id).disabled=!ready||presetBusy;
    $('sim-preset-prev').disabled||=!!workspace?.factory&&workspace.slot===0;
    $('sim-preset-next').disabled||=!!workspace?.factory&&workspace.slot===15;
    $('sim-factory-load').disabled||=$('sim-factory').value==='';
    $('sim-factory-export').disabled=!ready||$('sim-factory').value==='';
    for(const id of ['sim-save-device','sim-read-device'])$(id).disabled=!ready||syncDevice.busy||!navigator.requestMIDIAccess;
  }
  let syncDevice={mode:'offline',epoch:0,busy:false},deviceSnapshot,deviceSnapshotKind;
  let comparedRevision,comparisonMatches=false,syncTimer,syncChecking=false,syncGeneration=0,syncText='';
  function clearDeviceSnapshot(){
    deviceSnapshot=undefined;comparedRevision=undefined;comparisonMatches=false;++syncGeneration;
    clearTimeout(syncTimer);syncTimer=undefined;renderSync();
  }
  function rememberDeviceSnapshot(bytes,kind){
    deviceSnapshot=FM1Web.tunedProject(bytes).slice();deviceSnapshotKind=kind;
    comparedRevision=undefined;comparisonMatches=false;++syncGeneration;renderSync();
  }
  async function compareDeviceSnapshot(){
    syncTimer=undefined;if(syncChecking||!deviceSnapshot||!ready)return;
    syncChecking=true;const generation=syncGeneration,expected=deviceSnapshot;
    try{
      const snapshot=await request('snapshot');
      if(generation!==syncGeneration)return;
      comparedRevision=snapshot.revision;
      comparisonMatches=snapshot.bytes.length===expected.length&&snapshot.bytes.every((v,i)=>v===expected[i]);
    }catch(_){comparisonMatches=false;}
    finally{syncChecking=false;renderSync();}
  }
  function renderSync(){
    let title='网页试听 · 未连接设备',detail='',style='preview';
    if(syncDevice.mode==='update')title='设备已连接用于更新 · 网页试听独立';
    if(syncDevice.mode==='library'){
      const changed=deviceSnapshot&&(!comparisonMatches||comparedRevision!==workspace?.revision);
      title=!deviceSnapshot?'设备已连接 · 请读取项目':changed?'网页修改未写入设备':deviceSnapshotKind==='save'?'已保存到设备 · 校验通过':'已读取设备项目';
      style=!deviceSnapshot?'preview':changed?'pending':'verified';
      detail='网页修改需手动保存；实机更改后请重新读取。';
      if(deviceSnapshot&&ready&&comparedRevision!==workspace?.revision&&!syncTimer&&!syncChecking)
        syncTimer=setTimeout(compareDeviceSnapshot,120);
    }
    if(syncDevice.connection){
      title=syncDevice.connection.failed?'设备连接失败':'正在连接设备…';
      detail=syncDevice.connection.message;style='pending';
    }
    const text=title+'|'+detail+'|'+style;
    if(text===syncText)return;syncText=text;
    $('device-sync-status').dataset.state=style;
    FM1I18n.text($('device-sync-title'),title);FM1I18n.text($('device-sync-detail'),detail);
  }
  document.addEventListener('fm1-device-state',event=>{
    const changed=event.detail.epoch!==syncDevice.epoch;syncDevice=event.detail;
    if(changed)clearDeviceSnapshot();else renderSync();availability();
  });
  document.addEventListener('fm1-device-project-change',clearDeviceSnapshot);
  document.addEventListener('fm1-device-project-saved',event=>rememberDeviceSnapshot(event.detail.bytes,'save'));
  function dispatch(message) {
    if(handoff){waiting.push(message);return;}
    if(node){node.port.postMessage(message);return;}
    if(!preview)return;
    try {const data=preview.command(message);if(message.id)receive({id:message.id,data});receive(preview.frame());}
    catch(error){receive({id:message.id,error:error.message});}
  }
  function send(type,args={}){if(ready)dispatch({type,...args});}
  function request(type,args={}) {
    if(!ready)return Promise.reject(Error('仿真尚未载入'));
    if(['select','import','clear','open','step'].includes(type))release();
    if(type==='preset'){wakingPlay=undefined;wakingKeys.clear();}
    const id=++sequence;return new Promise((resolve,reject)=>{
      const timer=setTimeout(()=>{pending.delete(id);reject(Error('仿真操作超时'));},7000);
      pending.set(id,{resolve,reject,timer});dispatch({type,id,...args});
    });
  }
  globalThis.FM1SamplePreview = data => request('samples', {data});
  async function select(track,source) {
    try {await request('select',{track,source});}
    catch(error){
      if(state){$('sim-track').value=state[0];$('sim-source').value=state[8+state[0]*20];}
      throw error;
    }
  }

  function receive(message) {
    if(message.id){const item=pending.get(message.id);if(item){clearTimeout(item.timer);pending.delete(message.id);message.error?item.reject(Error(message.error)):item.resolve(message.data);}return;}
    if(message.error){say(message.error);return;}
    if(message.type!=='frame')return;
    const before=state;state=message.state;
    const contextChanged=!before||before[1]!==state[1]||before[0]!==state[0]||before[8+before[0]*20]!==state[8+state[0]*20];
    const fm=state[8+state[0]*20]===0;
    if(before&&contextChanged){
      wakingPlay=undefined;wakingKeys.clear();
      const pageTips={
        0:'总览：SELECT 选轨，K3（STEP）选步；PRESETS 换预设，ALGORITHM／K2 换音源。',
        1:'SEQ：ALGORITHM 翻底部八页，PRESETS 选步，SELECT 选轨，OCT−／OCT+ 换步进页；SYNC / LOOP 页设时钟，TRACK ROLE 页设角色与输入通道，长按 SEQ 复制与更多。',
        2:'FM 详细编辑：黑键选择算子／音高／全局，SELECT 翻参数页，K1–K4 调节；长按 OP 键开关算子。',
        3:fm?'FM 总包络：K1 起音，K2 衰减，K3 保持，K4 释放；各算子包络在 EDIT 详细编辑页调节。':'非 FM 包络：K1–K4 调屏幕列出的真实参数，SELECT 选轨；鼓／采样用 PRESETS 或白键选声部，重新触发试听。',
        4:fm?'FM LFO：SELECT 翻参数页，K1–K4 调节；LFO 返回演奏。':'轨道 LFO：K1 速度，K2 深度，K3 音量／声像，K4 波形；深度 0 为关闭，SELECT 选轨。参数随项目和音色保存。',
        5:'音色编辑：K1–K4 调当前四项；短按 EDIT 打开的常用页用 SELECT 选轨，长按后的细调页用 SELECT 翻页。PRESETS 选音色／鼓声部。',
        6:'混音：白键 1–8 静音、9–16 独奏，同轨互斥；SELECT 选轨，K1–K4 调当前四轨组音量。',
        7:'蓝牙：K1 总开关，K2 音频，K3 BLE 角色，K4 时钟；PRESETS 选屏幕下方操作，SAVE 执行，清除配对需再次确认。无线连接请在实机操作。',
        8:'保存项目：SELECT／PRESETS 选 USER 槽，再按 SAVE 保存；覆盖其他已有槽需确认，成功后自动返回。',
        9:'FM 音色命名：SELECT 选字符，K1 改字符；EDIT 返回编辑，HOME 返回轨道。',
        10:'效果：PRESETS 切 TRACK／SHARED／DELAY。DELAY 页 K1 开关 SYNC、K2 调分频或毫秒、K3 调 BPM、K4 调反馈；连按 SEQ 或 TAP 打拍。',
        11:'音源预览：ALGORITHM／K2／SELECT／PRESETS 选择，SAVE 确认，HOME 取消；确认前保留当前音色。',
        12:'复制与粘贴：SELECT 选操作，SAVE 执行；复制后自动选中 PASTE。PRESETS／K2 选目标步，OCT−／OCT+ 选目标页，再按一次 SAVE 粘贴。',
        15:'FM 常用参数：K1 算法、K2 反馈、K3 移调、K4 音量；PRESETS 换当前轨音色。短按 EDIT 返回，长按进细调。',
        17:'ARP：短按开关，长按调参数；ALGORITHM 翻页，第一页调分频／方向／音域／门长，第二页调 Swing／Hold。SELECT 选轨，开关和参数随项目保存，不保存按住的音符。',
        16:'预设列表：PRESETS／SELECT 直接载入，PLAY 载入播放，SAVE 打开项目保存；K1 选 USER／FACTORY。切换直接生效，不提示保存。'
      };
      if(pageTips[state[1]])say(pageTips[state[1]]);
    }
    if(contextChanged){
      for(const button of document.querySelectorAll('[data-sim-page]'))button.setAttribute('aria-pressed',String(Number(button.dataset.simPage)===state[1]||(Number(button.dataset.simPage)===15&&state[1]===5)));
      const presetUse={0:'换整套预设',1:'选步进',3:fm?'开关 FM 包络':[2,5].includes(state[8+state[0]*20])?'选择鼓／采样声部':'此页不使用',4:'此页不使用',5:'换当前轨音色／鼓声部',8:'选保存槽位',10:'切换轨道发送／共用细调',12:'选步进',13:'选步进',15:'换当前轨音色',16:'选整套预设',17:'此页不使用'};
      FM1I18n.text($('sim-context'),'面板 PRESETS 旋钮：'+(presetUse[state[1]]||'以屏幕下方标签为准')+'。网页“内置预设”始终切换整套项目。');
    }
    updateScreenControls(message.screen,contextChanged);
    if(message.pixels)for(let band=0,source=0;band<15;){
      if(!(message.pixelBands&(1<<band))){band++;continue;}
      const first=band;while(band<15&&(message.pixelBands&(1<<band)))band++;
      for(let i=first*3840;i<band*3840;i++){
        const value=message.pixels[source++],at=i*4;
        pixels.data[at]=rgb5[value>>11];pixels.data[at+1]=rgb6[(value>>5)&63];pixels.data[at+2]=rgb5[value&31];
      }
      image.putImageData(pixels,0,0,0,first*16,240,(band-first)*16);
    }
    const selectionChanged=!before||before[0]!==state[0]||before[8+state[0]*20]!==state[8+state[0]*20];
    const oldWorkspace=workspace;
    if(message.workspace){
      workspace=message.workspace;
      if(!oldWorkspace||workspace.name!==oldWorkspace.name)$('sim-current-project').textContent=workspace.name||'UNTITLED';
      if(!oldWorkspace||workspace.dirty!==oldWorkspace.dirty)FM1I18n.text($('sim-project-note'),workspace.dirty?'工作区有修改':'工作区未修改');
      if(!oldWorkspace||workspace.slot!==oldWorkspace.slot||workspace.factory!==oldWorkspace.factory)$('sim-factory').value=workspace.factory?String(workspace.slot):'';
      renderSync();
    }
    const count=Math.min(4,Math.ceil(message.countIn/4));
    const recording=state[16]&&state[17]!==1?'等待外部 MIDI 时钟；在 SEQ 的 SYNC / LOOP 页可切回 INT。':state[4]?
      count?'预备拍：'+count+'；下一小节开始录音。':state[14]?'实时录音：弹琴录入，REC 结束录音。':'步进录入：每次弹琴写入编辑步，REC 结束录入。':state[14]?'正在播放；绿色是播放位置，选中框是编辑位置。':'已停止；REC 开始步进录入，PLAY 播放。';
    $('sim-record-state').hidden=!(state[4]||(state[16]&&state[17]!==1));
    if($('sim-record-state').dataset.message!==recording){$('sim-record-state').dataset.message=recording;FM1I18n.text($('sim-record-state'),recording);}
    if(!before||before[14]!==state[14]){
      FM1I18n.text($('sim-play-control'),state[14]?'暂停':'播放');
      $('sim-play-control').setAttribute('aria-pressed',String(!!state[14]));
    }
    if(selectionChanged){$('sim-track').value=state[0];$('sim-source').value=state[8+state[0]*20];}
    for(let t=0;t<8;t++){
      const at=8+t*20,{button,source,level,dots}=ui.tracks[t];
      if(!before||before[0]!==state[0])button.classList.toggle('selected',state[0]===t);
      if(!before||before[at]!==state[at])FM1I18n.text(source,sources[state[at]]);
      if(!before||before[at+3]!==state[at+3])level.style.setProperty('--level',(state[at+3]/127*100)+'%');
      for(let byte=0;byte<8;byte++){
        const mask=at+12+byte,changed=before?before[mask]^state[mask]:255;
        if(changed)for(let bit=0;bit<8;bit++)if(changed&(1<<bit))dots[byte*8+bit].classList.toggle('on',!!(state[mask]&(1<<bit)));
      }
      if(!before||before[209+t]!==state[209+t])
        for(let i=0;i<64;i++)dots[i].classList.toggle('outside',i>=state[209+t]);
      if(!before||before[at+6]!==state[at+6]||before[at+7]!==state[at+7]){
        if(before&&before[at+6]&&before[at+7]<64)dots[before[at+7]].classList.remove('play');
        if(state[at+6]&&state[at+7]<64)dots[state[at+7]].classList.add('play');
      }
      if(!before||before[at+4]!==state[at+4]||before[at+5]!==state[at+5]||before[209+t]!==state[209+t]||before[217+t]!==state[217+t])FM1I18n.attr(button,'title','轨道 '+(t+1)+' · '+state[209+t]+' 步'+(state[217+t]!==state[209+t]?' · 下轮 '+state[217+t]+' 步':'')+(state[at+4]?' · 静音':'')+(state[at+5]?' · 独奏':''));
    }
    for(const [button,at]of ui.leds)if(!before||before[at]!==state[at])button.classList.toggle('lit',!!state[at]);
    if(message.flash){try{localStorage.setItem(storageKey,encode(message.flash));$('sim-storage-warning').hidden=true;}catch(_){$('sim-storage-warning').hidden=false;}}
    if(selectionChanged||!oldWorkspace||workspace?.slot!==oldWorkspace.slot||workspace?.factory!==oldWorkspace.factory)availability();
  }
  let lcdKey='',lcdScreen,lcdEditing,lcdItems=new Map(),lcdReturnFocus;
  const lcdDialog=$('sim-lcd-dialog');
  const lcdId=item=>[item.action,item.index,item.owner].join(':');
  function lcdAdjust(item,delta){
    if(!item||!ready)return;
    send('lcd',{context:lcdScreen.context,action:item.action,index:item.index,owner:item.owner,delta});
  }
  function lcdWheel(button,getItem){
    let amount=0,last=0,direction=0,context;
    const reset=()=>{amount=last=direction=0;};
    button.addEventListener('pointerleave',reset);
    button.addEventListener('wheel',event=>{
      const item=getItem();
      if(button.disabled||!item||!event.deltaY||event.ctrlKey||event.metaKey||Math.abs(event.deltaX)>Math.abs(event.deltaY))return;
      event.preventDefault();event.stopPropagation();
      const now=performance.now(),sign=Math.sign(event.deltaY);
      const nextContext=lcdScreen.context+':'+lcdId(item);
      if(nextContext!==context){reset();context=nextContext;}
      if(sign!==direction||now-last>250){amount=0;direction=sign;}
      last=now;amount+=Math.abs(event.deltaY);
      if(event.deltaMode||amount>=40){amount=0;lcdAdjust(item,-sign);}
    },{passive:false});
  }
  function lcdValue(){
    const item=lcdItems.get(lcdEditing);
    if(!item)return;
    if($('sim-lcd-label').textContent!==item.label)$('sim-lcd-label').textContent=item.label;
    if($('sim-lcd-value').textContent!==item.value)$('sim-lcd-value').textContent=item.value;
  }
  function editLcd(id,button){
    release();lcdEditing=id;lcdReturnFocus=button;lcdValue();lcdDialog.showModal();$('sim-lcd-plus').focus();
  }
  function closeLcd(){lcdDialog.close();if(lcdReturnFocus?.isConnected)lcdReturnFocus.focus();}
  $('sim-lcd-close').onclick=closeLcd;
  lcdDialog.addEventListener('cancel',event=>{event.preventDefault();closeLcd();});
  $('sim-lcd-minus').onclick=()=>lcdAdjust(lcdItems.get(lcdEditing),-1);
  $('sim-lcd-plus').onclick=()=>lcdAdjust(lcdItems.get(lcdEditing),1);
  lcdWheel($('sim-lcd-value'),()=>lcdDialog.open&&lcdItems.get(lcdEditing));
  lcdDialog.addEventListener('keydown',event=>{
    if(['ArrowLeft','ArrowDown','ArrowRight','ArrowUp'].includes(event.code)){
      event.preventDefault();event.stopPropagation();lcdAdjust(lcdItems.get(lcdEditing),['ArrowLeft','ArrowDown'].includes(event.code)?-1:1);
    }
  });
  function updateScreenControls(screen,contextChanged){
    if(!screen)return;
    const changed=contextChanged||lcdScreen?.context!==screen.context;
    lcdScreen=screen;
    if(changed&&lcdDialog.open)closeLcd();
    const items=screen.targets.slice();
    for(let col=0;col<4;col++)if(screen.mask&(1<<col)){
      const [label,value]=screen.knobs[col];
      items.push({rect:[6+59*col,195,54,32],action:0,index:col,owner:
