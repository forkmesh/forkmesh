#include "DjScripts.h"

namespace forkmesh::djstudio {

QString hostPage()
{
    return QStringLiteral(R"HTML(<!doctype html><html><head><meta charset="utf-8">
<style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:#000}.s{position:absolute;left:0;top:0;width:100%;height:100%;opacity:1}.s>*{position:absolute;inset:0;width:100%;height:100%;border:0}</style>
</head><body><div class="s" id="s0"><div id="p0"></div></div><div class="s" id="s1"><div id="p1"></div></div><div class="s" id="s2"><div id="p2"></div></div><div class="s" id="s3"><div id="p3"></div></div><script>
var N=4,D=[],H=null,HG=null,HL=0.8;
for(var k=0;k<N;k++)D.push({p:null,ready:false,meta:"",id:"",play:false,routed:false,lv:0,w:{rate:1,keylock:0,gain:1,trim:1,eq:[0,0,0],filter:0,fx:[0,0,0],beat:0.5,scratch:null,on:0,gen:0,from:0,seek:0,spin:0,brake:0}});
function post(m){try{console.log("fmdj:"+JSON.stringify(m))}catch(e){}}
function onYouTubeIframeAPIReady(){post({type:"api"})}
function frame(k){try{var f=D[k].p&&D[k].p.getIframe&&D[k].p.getIframe();return f&&f.contentWindow}catch(e){return null}}
function push(k){var f=frame(k);if(f)f.postMessage({fmdj:1,w:D[k].w},"*")}
function vol(k){var d=D[k];if(!d.p||!d.ready)return;try{d.p.unMute();d.p.setVolume(d.routed?100:Math.round(Math.max(0,Math.min(1,d.w.gain))*100))}catch(e){}}
function fmCall(k,name){var d=D[k];if(!d.p||!d.ready||typeof d.p[name]!=="function")return;var a=[].slice.call(arguments,2);
if(name==="playVideo")d.w.on=1;else if(name==="pauseVideo"||name==="stopVideo")d.w.on=0;else if(name==="seekTo"){d.w.from=+a[0]||0;d.w.seek++}
push(k);try{d.p[name].apply(d.p,a)}catch(e){}}
function fmSet(k,o){var w=D[k].w;for(var x in o)w[x]=o[x];push(k);if("gain" in o&&!D[k].routed)vol(k)}
function fmLoad(k,id,start,rate,play){var d=D[k];d.w.rate=rate;d.w.scratch=null;d.w.on=play?1:0;d.w.from=start;d.w.gen++;d.meta="";d.id=id;d.lv=0;
if(d.p&&d.ready){push(k);try{if(play)d.p.loadVideoById({videoId:id,startSeconds:start});else d.p.cueVideoById({videoId:id,startSeconds:start})}catch(e){}vol(k);return}
d.play=!!play;make(k,id,start)}
function fmEject(k){var d=D[k];d.id="";d.w.on=0;d.w.gen++;d.lv=0;if(d.p){try{d.p.destroy()}catch(e){}document.getElementById("s"+k).innerHTML='<div id="p'+k+'"></div>';d.p=null;d.ready=false;d.routed=false}}
function make(k,id,start){var d=D[k];if(d.p){try{d.p.destroy()}catch(e){}document.getElementById("s"+k).innerHTML='<div id="p'+k+'"></div>';d.ready=false;d.routed=false}
d.p=new YT.Player("p"+k,{host:"https://www.youtube-nocookie.com",width:"100%",height:"100%",videoId:id,
playerVars:{playsinline:1,controls:0,disablekb:1,fs:0,rel:0,iv_load_policy:3,cc_load_policy:0,enablejsapi:1,start:Math.floor(start),origin:location.origin},
events:{onReady:function(){d.ready=true;vol(k);push(k);if(d.play){d.play=false;try{if(d.w.from>0)d.p.seekTo(d.w.from,true);d.p.playVideo()}catch(e){}}post({type:"ready",k:k})},
onStateChange:function(e){if(e.data===1)push(k);post({type:"state",k:k,s:e.data})},
onError:function(e){d.w.on=0;push(k);post({type:"error",k:k,code:e.data})}}})}
function fmLayout(r){for(var k=0;k<N;k++){var s=document.getElementById("s"+k).style,x=r[k]||[0,0,0,0,0];s.left=(x[0]*100)+"%";s.top=(x[1]*100)+"%";s.width=(x[2]*100)+"%";s.height=(x[3]*100)+"%";s.opacity=x[4];s.zIndex=x[5]||0;s.visibility=x[4]>0.003&&x[2]>0?"visible":"hidden"}}
function ac(){if(!H){var C=window.AudioContext||window.webkitAudioContext;H=new C();HG=H.createGain();HG.gain.value=HL;HG.connect(H.destination)}if(H.state!=="running")H.resume().catch(function(){});return H}
function fmLevel(l){HL=l;if(HG)HG.gain.value=l}
function env(g,t,a,peak,dur){g.gain.setValueAtTime(0.0001,t);g.gain.exponentialRampToValueAtTime(Math.max(0.0002,peak),t+a);g.gain.exponentialRampToValueAtTime(0.0001,t+dur)}
var NB=null;function noiseBuf(){var c=ac();if(!NB){NB=c.createBuffer(1,c.sampleRate*4,c.sampleRate);var d=NB.getChannelData(0);for(var i=0;i<d.length;i++)d[i]=Math.random()*2-1}return NB}
function tone(type,f0,f1,dur,peak,at,dest){var c=ac(),t=c.currentTime+(at||0),o=c.createOscillator(),g=c.createGain();o.type=type;o.frequency.setValueAtTime(f0,t);o.frequency.exponentialRampToValueAtTime(Math.max(1,f1),t+dur);env(g,t,0.004,peak,dur);o.connect(g);g.connect(dest||HG);o.start(t);o.stop(t+dur+0.05);return o}
function noise(dur,type,freq,peak,at,q,dest){var c=ac(),t=c.currentTime+(at||0),s=c.createBufferSource(),f=c.createBiquadFilter(),g=c.createGain();s.buffer=noiseBuf();f.type=type;f.frequency.setValueAtTime(freq,t);if(q)f.Q.value=q;env(g,t,0.004,peak,dur);s.connect(f);f.connect(g);g.connect(dest||HG);s.start(t,Math.random()*3);s.stop(t+dur+0.05);return f}
function held(type,f,at,dur,peak,dest){var c=ac(),t=c.currentTime+at,o=c.createOscillator(),g=c.createGain();o.type=type;o.frequency.value=f;g.gain.setValueAtTime(0.0001,t);g.gain.exponentialRampToValueAtTime(peak,t+0.015);g.gain.setValueAtTime(peak,t+dur-0.03);g.gain.exponentialRampToValueAtTime(0.0001,t+dur);o.connect(g);g.connect(dest);o.start(t);o.stop(t+dur+0.02);return o}
var PADS={
kick:function(){tone("sine",155,43,0.32,0.8)},tom:function(){tone("sine",190,80,0.36,0.65)},perc:function(){tone("triangle",740,160,0.16,0.36)},riser:function(){tone("sawtooth",160,1300,0.75,0.25)},
stab:function(){[196,247,294].forEach(function(f){tone("sawtooth",f,f*0.98,0.22,0.13)})},snare:function(){noise(0.18,"highpass",1800,0.5)},hat:function(){noise(0.07,"highpass",6000,0.22)},
clap:function(){noise(0.3,"highpass",1000,0.5);[0.045,0.09].forEach(function(d){noise(0.12,"highpass",1000,0.25,d)})},
airhorn:function(){var c=ac(),bp=c.createBiquadFilter(),ws=c.createWaveShaper(),g=c.createGain(),cv=new Float32Array(256);for(var i=0;i<256;i++){var x=i/128-1;cv[i]=Math.tanh(3*x)}ws.curve=cv;bp.type="bandpass";bp.frequency.value=1400;bp.Q.value=0.6;g.gain.value=0.32;bp.connect(ws);ws.connect(g);g.connect(HG);
[[0,0.13],[0.17,0.09],[0.3,0.09],[0.43,0.75]].forEach(function(p){[466,587,698].forEach(function(f){held("sawtooth",f,p[0],p[1],0.3,bp)})})},
siren:function(){var c=ac(),t=c.currentTime,o=c.createOscillator(),l=c.createOscillator(),ld=c.createGain(),f=c.createBiquadFilter(),g=c.createGain();o.type="square";o.frequency.value=900;l.frequency.value=2.2;ld.gain.value=380;l.connect(ld);ld.connect(o.frequency);f.type="lowpass";f.frequency.value=2600;g.gain.setValueAtTime(0.0001,t);g.gain.exponentialRampToValueAtTime(0.22,t+0.05);g.gain.setValueAtTime(0.22,t+2.1);g.gain.exponentialRampToValueAtTime(0.0001,t+2.4);o.connect(f);f.connect(g);g.connect(HG);o.start(t);l.start(t);o.stop(t+2.45);l.stop(t+2.45)},
explosion:function(){var c=ac(),t=c.currentTime,f=noise(2.2,"lowpass",4000,0.9);f.frequency.exponentialRampToValueAtTime(80,t+2.2);tone("sine",70,25,1.3,0.9)},
applause:function(){for(var i=0;i<160;i++){var at=Math.random()*3.2,sw=Math.sin(Math.min(1,at/3.2)*Math.PI);noise(0.015+Math.random()*0.03,"bandpass",1200+Math.random()*2800,(0.05+Math.random()*0.12)*(0.35+sw),at,1.1)}},
laugh:function(){var c=ac();for(var i=0;i<6;i++){var a=c.createBiquadFilter(),b=c.createBiquadFilter(),g=c.createGain();a.type="bandpass";a.frequency.value=850;a.Q.value=5;b.type="bandpass";b.frequency.value=1300;b.Q.value=6;g.gain.value=1.6;a.connect(g);b.connect(g);g.connect(HG);var o=tone("sawtooth",280-i*14,250-i*14,0.13,0.6,i*0.2,a);o.connect(b)}},
rewind:function(){var c=ac(),t=c.currentTime;tone("sawtooth",1400,110,1.3,0.18);var f=noise(1.3,"bandpass",3200,0.35,0,2);f.frequency.exponentialRampToValueAtTime(260,t+1.3)},
laser:function(){tone("square",2400,170,0.24,0.22);tone("square",2400,170,0.24,0.22,0.3)},
scratch:function(){var c=ac(),t=c.currentTime,f=noise(0.6,"bandpass",900,0.5,0,3);for(var i=0;i<8;i++)f.frequency.setValueAtTime(i%2?2200:600,t+i*0.075)}};
function fmPad(name){var f=PADS[name];if(f)try{f()}catch(e){}}
addEventListener("message",function(e){var m=e.data;if(!m||typeof m!=="object"||!m.fmdjFrame)return;for(var k=0;k<N;k++){if(!e.source||frame(k)!==e.source)continue;var d=D[k];
if(m.fmdjFrame==="hello")push(k);
else if(m.fmdjFrame==="level")d.lv=+m.l||0;
else if(m.fmdjFrame==="routed"){if(!d.routed){d.routed=true;vol(k)}post({type:"routed",k:k})}
else if(m.fmdjFrame==="pcm"){if(m.gen===d.w.gen&&d.id)post({type:"pcm",k:k,id:d.id,t:+m.t||0,d:String(m.d||"")})}
else if(m.fmdjFrame==="spun")post({type:"spun",k:k,t:+m.t||0})}});
setInterval(function(){var out=[];for(var k=0;k<N;k++){var d=D[k],p=d.p;if(!p||!d.ready||!p.getCurrentTime)continue;
try{var v=p.getVideoData?p.getVideoData():null;if(v&&v.title&&v.title!==d.meta){d.meta=v.title;post({type:"meta",k:k,title:v.title,author:v.author||""})}
out.push([k,p.getCurrentTime()||0,p.getDuration()||0,p.getPlayerState(),d.lv])}catch(e){}}if(out.length)post({type:"tick",d:out})},100);
</script><script src="https://www.youtube.com/iframe_api"></script></body></html>)HTML");
}

QString frameScript()
{
    return QStringLiteral(R"JS((function(){
if(window.top===window||!/(^|\.)youtube(-nocookie)?\.com$/.test(location.hostname))return;
try{var DP=Document.prototype;["hidden","webkitHidden"].forEach(function(n){Object.defineProperty(DP,n,{configurable:true,get:function(){return false}})});["visibilityState","webkitVisibilityState"].forEach(function(n){Object.defineProperty(DP,n,{configurable:true,get:function(){return "visible"}})})}catch(e){}
["visibilitychange","webkitvisibilitychange"].forEach(function(t){addEventListener(t,function(e){e.stopImmediatePropagation()},true)});
var w={rate:1,keylock:0,gain:1,trim:1,eq:[0,0,0],filter:0,fx:[0,0,0],beat:0.5,scratch:null,on:0,gen:0,from:0,seek:0,spin:0,brake:0};
var ctx=null,G=null,tries=0,braking=0,brakeId=0,spinId=0,PS=11025;
var S={on:0,mode:0,head:0,vel:0,target:0,amp:0,spinV:0,cur:null};
var RB=null,RL=0,ri=0,CH=[],pb=[],pt=-1,pnext=-1,acc=0,cnt=0,pe=0;
function video(){return document.querySelector("video")}
function blob(v){return String(v.currentSrc||v.src||"").indexOf("blob:")===0}
function clamp(x,a,b){x=+x;if(!isFinite(x))x=a;return Math.max(a,Math.min(b,x))}
function impulse(c,sec){var n=Math.floor(c.sampleRate*sec),b=c.createBuffer(2,n,c.sampleRate);for(var ch=0;ch<2;ch++){var d=b.getChannelData(ch);for(var i=0;i<n;i++)d[i]=(Math.random()*2-1)*Math.pow(1-i/n,2.6)}return b}
function b64(a){var u=new Uint8Array(a.buffer),s="";for(var i=0;i<u.length;i+=0x8000)s+=String.fromCharCode.apply(null,u.subarray(i,i+0x8000));return btoa(s)}
function send(){if(pb.length>=PS*0.05){var a=new Int16Array(pb.length);for(var i=0;i<pb.length;i++)a[i]=Math.round(clamp(pb[i],-1,1)*32767);parent.postMessage({fmdjFrame:"pcm",gen:w.gen,t:pt,d:b64(a)},"*")}
if(pt>=0)pt+=pb.length/PS;pb=[]}
function resetCapture(){pb=[];pt=-1;pnext=-1;acc=0;cnt=0;RB=null;RL=0;ri=0;CH=[];S.cur=null}
function capture(e){var v=G&&G.v,i0=e.inputBuffer.getChannelData(0),i1=e.inputBuffer.numberOfChannels>1?e.inputBuffer.getChannelData(1):i0,n=i0.length,sr=ctx.sampleRate;
if(!v||v.paused||v.seeking||S.on||braking||v.readyState<3)return;
var r=v.playbackRate||1,step=r/sr,t0=Math.max(0,v.currentTime-n*step);
if(!RB){RL=Math.floor(sr*30);RB=new Float32Array(RL)}
for(var j=0;j<n;j++)RB[(ri+j)%RL]=(i0[j]+i1[j])*0.5;
CH.push({t:t0,r:r,i:ri,n:n});ri+=n;while(CH.length&&CH[0].i<ri-RL+n)CH.shift();
if(pt<0||Math.abs(t0-pnext)>0.15){send();pt=t0;pe=t0+1/PS;acc=0;cnt=0}
for(var j=0;j<n;j++){acc+=(i0[j]+i1[j])*0.5;cnt++;var mt=t0+j*step;if(mt>=pe){pb.push(acc/cnt);acc=0;cnt=0;pe=pt+(pb.length+1)/PS}}
pnext=t0+n*step;if(pb.length>=PS*0.5){var keep=pe-pt-pb.length/PS;send();pe=pt+keep}}
function locate(T){var sr=ctx.sampleRate,c=S.cur;if(c&&T>=c.t&&T<c.t+c.n*c.r/sr&&c.i>=ri-RL)return c.i+(T-c.t)*sr/c.r;
for(var j=CH.length-1;j>=0;j--){c=CH[j];if(T>=c.t&&T<c.t+c.n*c.r/sr&&c.i>=ri-RL){S.cur=c;return c.i+(T-c.t)*sr/c.r}}return -1}
function read(T){if(!RB)return 0;var x=locate(T);if(x<0)return 0;var a=Math.floor(x),f=x-a;return RB[a%RL]*(1-f)+RB[(a+1)%RL]*f}
function voice(e){var o=e.outputBuffer.getChannelData(0),n=o.length;if(!S.on){for(var j=0;j<n;j++)o[j]=0;return}
var dt=1/ctx.sampleRate,w0=2*Math.PI*22,done=0;
for(var j=0;j<n;j++){var vel;
if(S.mode===2){S.spinV*=Math.exp(-dt/0.42);S.head+=S.spinV*dt;vel=S.spinV;if(Math.abs(S.spinV)<0.04)done=1}
else{var a=w0*w0*(S.target-S.head)-2*w0*S.vel;S.vel+=a*dt;S.head+=S.vel*dt;vel=S.vel}
var amp=Math.min(1,Math.abs(vel)*2.5);S.amp+=(amp-S.amp)*0.004;o[j]=read(S.head)*S.amp}
if(done&&S.mode===2){S.on=0;S.mode=0;parent.postMessage({fmdjFrame:"spun",t:Math.max(0,S.head)},"*")}}
function graph(v){
if(G&&G.v===v)return G;
if(tries>8||!blob(v))return null;
var C=window.AudioContext||window.webkitAudioContext;if(!C)return null;
try{ctx=ctx||new C({latencyHint:"interactive"})}catch(x){tries=99;return null}
if(ctx.state!=="running"){tries++;ctx.resume().then(apply,function(){});return null}
try{var g={v:v},c=ctx;
g.src=c.createMediaElementSource(v);g.trim=c.createGain();
g.low=c.createBiquadFilter();g.low.type="lowshelf";g.low.frequency.value=220;
g.mid=c.createBiquadFilter();g.mid.type="peaking";g.mid.frequency.value=1000;g.mid.Q.value=0.7;
g.high=c.createBiquadFilter();g.high.type="highshelf";g.high.frequency.value=3500;
g.lp=c.createBiquadFilter();g.lp.type="lowpass";g.lp.frequency.value=Math.min(22000,c.sampleRate/2-100);
g.hp=c.createBiquadFilter();g.hp.type="highpass";g.hp.frequency.value=10;
g.out=c.createGain();g.meter=c.createAnalyser();g.meter.fftSize=1024;g.buf=new Float32Array(1024);
g.dl=c.createDelay(4);g.fb=c.createGain();g.fb.gain.value=0.4;g.ew=c.createGain();g.ew.gain.value=0;
g.cv=c.createConvolver();g.cv.buffer=impulse(c,2.6);g.rw=c.createGain();g.rw.gain.value=0;
g.fl=c.createDelay(0.05);g.fl.delayTime.value=0.004;g.lfo=c.createOscillator();g.lfo.frequency.value=0.25;g.ld=c.createGain();g.ld.gain.value=0.003;g.lfo.connect(g.ld);g.ld.connect(g.fl.delayTime);g.lfo.start();
g.ffb=c.createGain();g.ffb.gain.value=0.55;g.fw=c.createGain();g.fw.gain.value=0;
g.src.connect(g.trim);g.trim.connect(g.low);g.low.connect(g.mid);g.mid.connect(g.high);g.high.connect(g.lp);g.lp.connect(g.hp);g.hp.connect(g.out);
g.hp.connect(g.dl);g.dl.connect(g.fb);g.fb.connect(g.dl);g.dl.connect(g.ew);g.ew.connect(g.out);
g.hp.connect(g.cv);g.cv.connect(g.rw);g.rw.connect(g.out);
g.hp.connect(g.fl);g.fl.connect(g.ffb);g.ffb.connect(g.fl);g.fl.connect(g.fw);g.fw.connect(g.out);
g.cap=c.createScriptProcessor(2048,2,1);g.cap.onaudioprocess=capture;g.z=c.createGain();g.z.gain.value=0;g.src.connect(g.cap);g.cap.connect(g.z);g.z.connect(c.destination);
g.sv=c.createScriptProcessor(1024,1,1);g.sv.onaudioprocess=voice;g.sv.connect(g.trim);
g.out.connect(g.meter);g.out.connect(c.destination);
if(G&&G.out)try{G.out.disconnect()}catch(x){}
G=g;resetCapture();
c.onstatechange=function(){if(c.state!=="running"&&c.state!=="closed")c.resume().catch(function(){})};
parent.postMessage({fmdjFrame:"routed"},"*");return g}catch(x){tries=99;return null}}
function apply(){var v=video();if(!v)return;
if(!braking&&!S.on&&Math.abs(v.playbackRate-w.rate)>0.0005)try{v.playbackRate=w.rate}catch(x){}
var keep=!!w.keylock;if(v.preservesPitch!==keep)try{v.preservesPitch=keep}catch(x){}
var g=graph(v);
if(!g){var vo=clamp(w.gain,0,1);if(Math.abs(v.volume-vo)>0.004)try{v.volume=vo}catch(x){}return}
var t=ctx.currentTime,f=clamp(w.filter,-1,1),eq=w.eq||[0,0,0],fx=w.fx||[0,0,0];
g.out.gain.setTargetAtTime(clamp(w.gain,0,1.5),t,0.012);
g.trim.gain.setTargetAtTime(clamp(w.trim,0,4),t,0.02);
g.low.gain.setTargetAtTime(clamp(eq[0],-40,12),t,0.02);g.mid.gain.setTargetAtTime(clamp(eq[1],-40,12),t,0.02);g.high.gain.setTargetAtTime(clamp(eq[2],-40,12),t,0.02);
var ny=Math.min(22000,ctx.sampleRate/2-100);
g.lp.frequency.setTargetAtTime(f<-0.02?Math.max(120,ny*Math.pow(0.006,-f)):ny,t,0.03);g.hp.frequency.setTargetAtTime(f>0.02?20*Math.pow(300,f):10,t,0.03);
g.lp.Q.value=f<-0.02?1.4:0.7;g.hp.Q.value=f>0.02?1.4:0.7;
g.ew.gain.setTargetAtTime(clamp(fx[0],0,1)*0.75,t,0.03);g.fb.gain.setTargetAtTime(0.3+clamp(fx[0],0,1)*0.3,t,0.05);g.dl.delayTime.setTargetAtTime(clamp(w.beat*0.75,0.05,3),t,0.05);
g.rw.gain.setTargetAtTime(clamp(fx[1],0,1)*0.9,t,0.03);
g.fw.gain.setTargetAtTime(clamp(fx[2],0,1)*0.8,t,0.03)}
function scratchState(){var v=video();if(!v)return;var hand=typeof w.scratch==="number";
if(hand){if(!S.on||S.mode!==1){S.on=1;S.mode=1;S.head=v.currentTime;S.vel=0;S.amp=0;try{v.pause()}catch(x){}}S.target=w.scratch}
else if(S.on&&S.mode===1){S.on=0;S.mode=0}}
function brake(){var v=video();if(!v||braking)return;braking=1;var r0=v.playbackRate||1,t0=Date.now();
var h=setInterval(function(){var k=(Date.now()-t0)/900;if(k>=1){clearInterval(h);try{v.pause()}catch(x){}braking=0;try{v.playbackRate=w.rate}catch(x){}parent.postMessage({fmdjFrame:"spun",t:v.currentTime},"*");return}
try{v.playbackRate=Math.max(0.0625,r0*Math.pow(1-k,1.6))}catch(x){}},15)}
function backspin(){var v=video();if(!v)return;S.on=1;S.mode=2;S.head=v.currentTime;S.spinV=-3.6;S.amp=1;try{v.pause()}catch(x){}}
function meter(){if(!G||!G.meter)return;G.meter.getFloatTimeDomainData(G.buf);var p=0;for(var i=0;i<G.buf.length;i++){var a=Math.abs(G.buf[i]);if(a>p)p=a}parent.postMessage({fmdjFrame:"level",l:Math.round(p*1000)/1000},"*")}
addEventListener("message",function(e){var m=e.data;if(e.source!==parent||!m||typeof m!=="object"||m.fmdj!==1||!m.w)return;var o=m.w;
if(o.gen!==w.gen){send();resetCapture();S.on=0;S.mode=0}
var b=o.brake!==w.brake&&o.brake,s=o.spin!==w.spin&&o.spin;w=o;apply();scratchState();if(b&&b!==brakeId){brakeId=b;brake()}if(s&&s!==spinId){spinId=s;backspin()}});
["loadedmetadata","playing","ratechange","volumechange","loadstart"].forEach(function(t){document.addEventListener(t,function(e){if(e.target&&e.target.tagName==="VIDEO"){if(t==="loadstart"){send();resetCapture()}apply()}},true)});
document.addEventListener("seeking",function(e){if(e.target&&e.target.tagName==="VIDEO")send()},true);
setInterval(function(){apply();meter()},100);
parent.postMessage({fmdjFrame:"hello"},"*");
})();)JS");
}

}
