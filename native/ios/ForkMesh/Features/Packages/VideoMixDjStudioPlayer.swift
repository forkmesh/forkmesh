import FMPackages
import QuartzCore
import SwiftUI
import UIKit
import WebKit

enum DjPlayerEvent {
    case ready
    case tick(time: Double, duration: Double, state: Int)
    case state(Int)
    case meta(title: String, author: String)
    case failed(Int)
    case source(id: String, url: URL)
}

struct DjVolume: Equatable {
    var level: Int
    var muted: Bool
    var gain: Double
}

@MainActor
final class DjDeckPlayer {
    let side: DjDeckSide
    private let host: DjPlayerHost

    init(side: DjDeckSide, host: DjPlayerHost) {
        self.side = side
        self.host = host
    }

    func load(_ id: String, start: Double, rate: Double, volume: DjVolume) { host.load(side, id: id, start: start, rate: rate, volume: volume) }
    func play() { host.call(side, "playVideo") }
    func pause() { host.call(side, "pauseVideo") }
    func stop() { host.stop(side) }
    func seek(_ seconds: Double) { host.call(side, "seekTo", DjPlayerHost.number(seconds), "true") }
    func setRate(_ rate: Double) { host.setRate(side, rate) }
    func scratch(_ speed: Double?, at seconds: Double) { host.scratch(side, speed, at: seconds) }
}

@MainActor
final class DjPlayerHost: NSObject, WKNavigationDelegate {
    let webView: WKWebView
    var onEvent: ((DjDeckSide, DjPlayerEvent) -> Void)?
    var onReload: (() -> Void)?

    private let origin: URL
    private var apiReady = false
    private var pending: [DjDeckSide: String] = [:]
    private var lastLoad: [DjDeckSide: String] = [:]
    private var lastVolume: [DjDeckSide: DjVolume] = [:]
    private var handOff: UIBackgroundTaskIdentifier = .invalid
    private var scripts = 0

    init(origin: URL) {
        self.origin = origin
        let configuration = WKWebViewConfiguration()
        configuration.allowsInlineMediaPlayback = true
        configuration.mediaTypesRequiringUserActionForPlayback = []
        configuration.allowsPictureInPictureMediaPlayback = false
        let controller = WKUserContentController()
        controller.addUserScript(WKUserScript(source: Self.frameScript, injectionTime: .atDocumentStart, forMainFrameOnly: false))
        configuration.userContentController = controller
        webView = WKWebView(frame: CGRect(x: 0, y: 0, width: 420, height: 240), configuration: configuration)
        super.init()
        controller.add(DjScriptProxy(self), name: "deck")
        webView.navigationDelegate = self
        webView.isOpaque = true
        webView.backgroundColor = .black
        webView.scrollView.isScrollEnabled = false
        webView.scrollView.contentInsetAdjustmentBehavior = .never
        webView.isUserInteractionEnabled = false
        webView.allowsLinkPreview = false
        webView.accessibilityElementsHidden = true
        webView.loadHTMLString(Self.html, baseURL: origin)
        let center = NotificationCenter.default
        center.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.background(true) }
        }
        center.addObserver(forName: UIApplication.willEnterForegroundNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.background(false) }
        }
        center.addObserver(forName: UIApplication.willResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.run("fmAway(1)") }
        }
        center.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.run("fmAway(0)") }
        }
    }

    private func background(_ entering: Bool) {
        endHandOff()
        guard apiReady else { return }
        if entering {
            handOff = UIApplication.shared.beginBackgroundTask(withName: "DjDeckHandOff") { [weak self] in
                MainActor.assumeIsolated { self?.endHandOff() }
            }
            Task { [weak self] in
                try? await Task.sleep(for: .seconds(12))
                self?.endHandOff()
            }
        }
        run("fmBackground(\(entering ? 1 : 0))")
    }

    private func endHandOff() {
        guard handOff != .invalid else { return }
        UIApplication.shared.endBackgroundTask(handOff)
        handOff = .invalid
    }

    func dock() {
        guard webView.window == nil else { return }
        let windows = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.flatMap(\.windows)
        guard let window = windows.first(where: \.isKeyWindow) ?? windows.first else { return }
        webView.frame = CGRect(x: 0, y: 0, width: 420, height: 240)
        window.insertSubview(webView, at: 0)
    }

    func load(_ side: DjDeckSide, id: String, start: Double, rate: Double, volume: DjVolume) {
        guard DjStudio.isVideoId(id) else { return }
        lastVolume[side] = volume
        let script = "fmLoad('\(side.rawValue)','\(id)',\(Self.number(start)),\(Self.number(rate)),\(volume.level),\(volume.muted),\(Self.number(volume.gain)))"
        lastLoad[side] = script
        if apiReady { run(script) } else { pending[side] = script }
    }

    func call(_ side: DjDeckSide, _ name: String, _ arguments: String...) {
        run((["fmCall('\(side.rawValue)'", "'\(name)'"] + arguments).joined(separator: ",") + ")")
    }

    func stop(_ side: DjDeckSide) {
        lastLoad[side] = nil
        pending[side] = nil
        run("fmStop('\(side.rawValue)')")
    }

    func reset() {
        apiReady = false
        pending = [:]
        lastLoad = [:]
        lastVolume = [:]
        webView.loadHTMLString(Self.html, baseURL: origin)
    }

    func setRate(_ side: DjDeckSide, _ rate: Double) { run("fmRate('\(side.rawValue)',\(Self.number(rate)))") }

    func scratch(_ side: DjDeckSide, _ speed: Double?, at seconds: Double) {
        run("fmScratch('\(side.rawValue)',\(speed.map(Self.number) ?? "null"),\(Self.number(seconds)))")
    }

    func setVolumes(_ volumes: [(DjDeckSide, DjVolume)]) {
        guard apiReady else { return }
        var calls: [String] = []
        for (side, volume) in volumes where lastVolume[side] != volume {
            lastVolume[side] = volume
            calls.append("fmVolume('\(side.rawValue)',\(volume.level),\(volume.muted),\(Self.number(volume.gain)))")
        }
        guard !calls.isEmpty else { return }
        run(calls.joined(separator: ";"))
    }

    fileprivate func receive(_ body: Any) {
        guard let message = body as? [String: Any], let type = message["type"] as? String else { return }
        let number = { (key: String) -> Double in (message[key] as? NSNumber)?.doubleValue ?? 0 }
        if type == "api" {
            apiReady = true
            let scripts = pending
            pending = [:]
            for script in scripts.values { run(script) }
            return
        }
        guard let side = (message["deck"] as? String).flatMap(DjDeckSide.init(rawValue:)) else { return }
        switch type {
        case "ready":
            lastVolume[side] = nil
            onEvent?(side, .ready)
        case "tick":
            onEvent?(side, .tick(time: number("time"), duration: number("duration"), state: Int(number("state"))))
        case "state":
            onEvent?(side, .state(Int(number("state"))))
        case "meta":
            onEvent?(side, .meta(title: message["title"] as? String ?? "", author: message["author"] as? String ?? ""))
        case "error":
            onEvent?(side, .failed(Int(number("code"))))
        case "source":
            if let id = message["id"] as? String, DjStudio.isVideoId(id),
                let text = message["src"] as? String, let url = URL(string: text), url.scheme == "https"
            {
                onEvent?(side, .source(id: id, url: url))
            }
        default:
            break
        }
    }

    private func run(_ script: String) {
        guard apiReady else { return }
        scripts += 1
        DjVitalsRecorder.shared.update { $0.scriptSent() }
        guard scripts % DjDiagnostics.scriptSampleEvery == 0 else {
            webView.evaluateJavaScript(script, completionHandler: nil)
            return
        }
        let sent = CACurrentMediaTime()
        webView.evaluateJavaScript(script) { _, _ in
            let trip = (CACurrentMediaTime() - sent) * 1000
            DjVitalsRecorder.shared.update { $0.scriptReturned(ms: trip) }
        }
    }

    nonisolated static func number(_ value: Double) -> String {
        value.isFinite ? String(format: "%.3f", value) : "0"
    }

    func webView(
        _ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction
    ) async -> WKNavigationActionPolicy {
        guard let target = navigationAction.request.url else { return .cancel }
        if target.scheme == "about" { return .allow }
        if navigationAction.targetFrame?.isMainFrame == false { return target.scheme == "https" ? .allow : .cancel }
        if navigationAction.targetFrame == nil { return .cancel }
        return target.host == origin.host && navigationAction.navigationType == .other ? .allow : .cancel
    }

    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) {
        for side in DjDeckSide.allCases where lastLoad[side] != nil { onEvent?(side, .failed(-1)) }
    }

    func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
        apiReady = false
        pending = lastLoad
        lastVolume = [:]
        webView.loadHTMLString(Self.html, baseURL: origin)
        onReload?()
    }

    private static let html = """
        <!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
        <style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:#000}.s{position:absolute;top:0;bottom:0;width:50%}#sa{left:0}#sb{right:0}.s>*{position:absolute;inset:0;width:100%;height:100%;border:0}</style>
        </head><body><div class="s" id="sa"><div id="a"></div></div><div class="s" id="sb"><div id="b"></div></div><script>
        var K=["a","b"],D={},n=0,B=0,M=0,L=0;
        K.forEach(function(k){D[k]={p:null,ready:false,meta:"",id:"",routed:false,mirror:false,voice:null,w:{rate:1,volume:100,muted:false,gain:1,scratch:null,at:0,on:0,from:0,gen:0}}});
        function post(m){try{webkit.messageHandlers.deck.postMessage(m)}catch(e){}}
        function onYouTubeIframeAPIReady(){post({type:"api"})}
        function frame(k){try{var f=D[k].p&&D[k].p.getIframe&&D[k].p.getIframe();return f&&f.contentWindow}catch(e){return null}}
        function heard(d){return!!d.voice&&Date.now()-d.voice.at<1500}
        function push(k){var f=frame(k),x=D[k].w;if(f)f.postMessage({fmdj:1,rate:x.rate,gain:x.gain,scratch:x.scratch,at:x.at,bg:B,away:L,on:x.on,from:x.from,gen:x.gen},"*")}
        function fmCall(k,name){var d=D[k];if(!d.p||!d.ready||typeof d.p[name]!=="function")return;var a=[].slice.call(arguments,2);
        if(name==="playVideo")d.w.on=1;else if(name==="pauseVideo"||name==="stopVideo")d.w.on=0;else if(name==="seekTo")d.w.from=+a[0]||0;
        push(k);d.p[name].apply(d.p,a);setTimeout(function(){tick(k)},60)}
        function fmStop(k){D[k].w.scratch=null;fmCall(k,"stopVideo")}
        function fmRate(k,r){D[k].w.rate=r;push(k)}
        function fmScratch(k,s,at){var x=D[k].w;x.scratch=s;x.at=at;push(k)}
        function fmVolume(k,v,m,g){var d=D[k];d.w.volume=v;d.w.muted=m;d.w.gain=g;push(k);if(!d.p||!d.ready)return;d.p.setVolume(d.routed?100:v);if(m)d.p.mute();else d.p.unMute()}
        function tick(k){var d=D[k],p=d.p;if(!p||!d.ready||!p.getCurrentTime)return;var v=p.getVideoData?p.getVideoData():null;if(v&&v.title&&v.title!==d.meta){d.meta=v.title;post({type:"meta",deck:k,title:v.title,author:v.author||""})}
        var o=heard(d)?d.voice:null;
        if(o)post({type:"tick",deck:k,time:o.s===1?o.t+(Date.now()-o.at)/1000*d.w.rate:o.t,duration:o.d||p.getDuration()||0,state:o.s});
        else post({type:"tick",deck:k,time:p.getCurrentTime()||0,duration:p.getDuration()||0,state:p.getPlayerState()})}
        function fmLoad(k,id,start,rate,v,m,g){var d=D[k];d.w.rate=rate;d.w.scratch=null;d.w.on=0;d.w.from=start;d.w.gen++;d.meta="";d.resume=false;d.voice=null;d.id=id;
        if(d.p&&d.ready){push(k);d.p.cueVideoById({videoId:id,startSeconds:start});fmVolume(k,v,m,g);setTimeout(function(){tick(k)},300);return}
        d.w.volume=v;d.w.muted=m;d.w.gain=g;make(k,id,start)}
        function make(k,id,start){var d=D[k];d.id=id;d.voice=null;d.mirror=false;
        if(d.p){try{d.p.destroy()}catch(e){}document.getElementById("s"+k).innerHTML='<div id="'+k+'"></div>';d.ready=false;d.routed=false}
        d.p=new YT.Player(k,{host:"https://www.youtube-nocookie.com",width:"100%",height:"100%",videoId:id,
        playerVars:{playsinline:1,controls:0,disablekb:1,fs:0,rel:0,iv_load_policy:3,cc_load_policy:0,enablejsapi:1,start:Math.floor(start),origin:location.origin,fmmse:M},
        events:{onReady:function(){d.ready=true;if(d.resume){d.resume=false;d.p.loadVideoById({videoId:id,startSeconds:start})}fmVolume(k,d.w.volume,d.w.muted,d.w.gain);post({type:"ready",deck:k});tick(k)},
        onStateChange:function(e){if(e.data===1)push(k);if(!heard(d)){if(e.data===0&&d.w.on){d.w.on=0;push(k)}post({type:"state",deck:k,state:e.data})}tick(k)},
        onError:function(e){var was=d.w.on;d.w.on=0;push(k);if(!M&&d.id&&[2,100,101,150,152,153].indexOf(e.data)<0){M=1;d.resume=!!was;d.w.on=was;make(k,d.id,d.w.from);return}post({type:"error",deck:k,code:e.data})}}})}
        function fmBackground(on){B=on?1:0;K.forEach(function(k){var d=D[k],p=d.p;if(B&&d.routed&&!d.mirror&&p&&d.ready&&!d.w.muted&&d.w.gain>=0.02){var s=p.getPlayerState(),v=p.getVideoData?p.getVideoData():null;if((s===1||s===3)&&v&&v.video_id){var t=p.getCurrentTime()||0;d.resume=true;d.w.on=1;d.w.from=t;make(k,v.video_id,t);return}}push(k)})}
        function fmAway(on){L=on?1:0;K.forEach(push)}
        addEventListener("message",function(e){var m=e.data;if(!m||typeof m!=="object"||!m.fmdjFrame)return;K.forEach(function(k){if(!e.source||frame(k)!==e.source)return;var d=D[k];
        if(m.fmdjFrame==="voice"){d.voice={t:+m.t||0,d:+m.d||0,s:m.s|0,at:Date.now()};if(d.voice.s===0&&d.w.on){d.w.on=0;push(k)}tick(k)}
        else if(m.fmdjFrame==="source"){if(typeof m.src==="string"&&m.src.indexOf("https:")===0&&d.id)post({type:"source",deck:k,id:d.id,src:m.src})}
        else if(m.fmdjFrame==="routed"){d.mirror=!!m.mirror;if(!d.routed){d.routed=true;fmVolume(k,d.w.volume,d.w.muted,d.w.gain)}}
        else push(k)})});
        setInterval(function(){n++;K.forEach(function(k){var d=D[k];if(!d.p||!d.ready||heard(d))return;var s=d.p.getPlayerState();if(s===1||s===3||n%4===0)tick(k)})},250);
        </script><script src="https://www.youtube.com/iframe_api"></script></body></html>
        """

    private static let frameScript = """
        (function(){
        if(window.top===window||!/(^|\\.)youtube(-nocookie)?\\.com$/.test(location.hostname))return;
        var mse=/[?&]fmmse=1(&|$)/.test(location.search);
        var w={rate:1,gain:1,scratch:null,at:0,bg:0,away:0,on:0,from:0},ctx=null,amp=null,live=null,last=null,spare=null,stale="",tries=0,waking=0,moved=0,beat=0,queued=0;
        var P=HTMLMediaElement.prototype,md=Object.getOwnPropertyDescriptor(P,"muted"),H=new WeakMap(),T=new WeakMap(),A=new WeakMap(),decoys=[];
        function own(v){return!!v&&v.tagName==="VIDEO"&&decoys.indexOf(v)<0}
        function want(v){var h=H.get(v);return h?h.want:md.get.call(v)}
        function blob(e){return String(e.currentSrc||e.src||"").indexOf("blob:")===0}
        function solo(v){return blob(v)&&v.readyState>=1&&!v.videoWidth&&!(v.videoTracks&&v.videoTracks.length)}
        function seal(v){if(!own(v))return;if(!H.has(v))H.set(v,{want:md.get.call(v)});var m=want(v)||!solo(v);if(md.get.call(v)!==m)md.set.call(v,m)}
        function soon(){if(queued)return;queued=1;setTimeout(function(){queued=0;sync()},0)}
        Object.defineProperty(P,"muted",{configurable:true,enumerable:md.enumerable,get:function(){return own(this)?want(this):md.get.call(this)},set:function(x){if(!own(this)){md.set.call(this,x);return}H.set(this,{want:!!x});seal(this);soon()}});
        var play=P.play;P.play=function(){var o=own(this);if(o)seal(this);var r=play.apply(this,arguments);if(o)soon();return r};
        if(!mse)["MediaSource","ManagedMediaSource","WebKitMediaSource"].forEach(function(n){try{delete window[n]}catch(e){}try{if(window[n])Object.defineProperty(window,n,{configurable:true,writable:true,value:undefined})}catch(e){}});
        var S=mse&&window.MediaSource;
        if(S&&S.prototype&&S.prototype.addSourceBuffer){
        function desc(o,n){for(;o;o=Object.getPrototypeOf(o)){var d=Object.getOwnPropertyDescriptor(o,n);if(d)return d}return null}
        function busy(t){for(var i=0;i<t.sourceBuffers.length;i++)if(t.sourceBuffers[i].updating)return true;return false}
        var add=S.prototype.addSourceBuffer,rem=S.prototype.removeSourceBuffer,eos=S.prototype.endOfStream,dd=desc(S.prototype,"duration");
        S.prototype.addSourceBuffer=function(type){var t=T.get(this);if(t&&t.readyState==="open"&&/^video\\//i.test(String(type))){try{var b=add.call(t,type);T.set(b,t);return b}catch(e){}}return add.call(this,type)};
        S.prototype.removeSourceBuffer=function(b){var t=T.get(b);return rem.call(t||this,b)};
        S.prototype.endOfStream=function(){var t=T.get(this);if(t)try{if(t.readyState==="open"&&!busy(t))eos.call(t)}catch(e){}return eos.apply(this,arguments)};
        if(dd&&dd.set)Object.defineProperty(S.prototype,"duration",{configurable:true,enumerable:dd.enumerable,get:dd.get,set:function(x){dd.set.call(this,x);var t=T.get(this);if(t)try{if(t.readyState==="open"&&!busy(t))dd.set.call(t,x)}catch(e){}}});
        function twin(ms,M){try{var t=new M(),e=document.createElement("video");e.muted=true;e.disableRemotePlayback=true;e.preload="auto";e.src=URL.createObjectURL(t);T.set(ms,t);decoys.push(e);
        ms.addEventListener("sourceclose",function(){T.delete(ms);var i=decoys.indexOf(e);if(i>=0)decoys.splice(i,1);try{URL.revokeObjectURL(e.src);e.removeAttribute("src");e.load()}catch(x){}})}catch(x){}}
        ["MediaSource","ManagedMediaSource"].forEach(function(n){var M=window[n];if(typeof M!=="function")return;
        var W=class extends M{constructor(){super();twin(this,M)}};try{Object.defineProperty(W,"name",{value:n})}catch(e){}window[n]=W});
        }
        try{var DP=Document.prototype;Object.defineProperty(DP,"hidden",{configurable:true,get:function(){return false}});Object.defineProperty(DP,"visibilityState",{configurable:true,get:function(){return "visible"}});Object.defineProperty(DP,"webkitHidden",{configurable:true,get:function(){return false}});Object.defineProperty(DP,"webkitVisibilityState",{configurable:true,get:function(){return "visible"}})}catch(e){}
        ["visibilitychange","webkitvisibilitychange"].forEach(function(t){addEventListener(t,function(e){e.stopImmediatePropagation()},true)});
        function video(){return document.querySelector("video")}
        function scratching(){return typeof w.scratch==="number"}
        function speed(e,r){if(Math.abs(e.playbackRate-r)>0.001)e.playbackRate=r}
        function pitch(e){if(e.preservesPitch!==false)try{e.preservesPitch=false}catch(x){}if(e.webkitPreservesPitch)try{e.webkitPreservesPitch=false}catch(x){}}
        function start(e){if(e.ended)return;var q=e.play();if(q&&q.catch)q.catch(function(){})}
        function quiet(a){try{a.pause();a.removeAttribute("src");a.load()}catch(e){}}
        function fresh(v,src,cors,t){var a=document.createElement("audio");a.fmSrc=src;a.fmCors=cors;a.preload="auto";if(cors)a.crossOrigin="anonymous";
        a.addEventListener("error",function(){if(!a.fmCors||a.readyState>0||A.get(v)!==a)return;var at=a.currentTime;a.fmCors=false;a.removeAttribute("crossorigin");a.src=src;try{a.currentTime=at}catch(x){}soon()});
        ["playing","pause","waiting","seeked","ended"].forEach(function(n){a.addEventListener(n,report)});
        pitch(a);a.src=src;if(t>0)try{a.currentTime=t}catch(x){}speed(a,scratching()?1:w.rate);return a}
        function voice(v,make){if(!own(v))return null;if(blob(v))return solo(v)?v:null;var src=v.currentSrc||v.src||"";if(!src||src===stale)return null;var a=A.get(v);if(a&&a.fmSrc===src)return a;if(!make)return null;if(a)quiet(a);
        a=fresh(v,src,!/m3u8|manifest/i.test(src),v.readyState>=1?v.currentTime:w.from);if(a.fmCors)parent.postMessage({fmdjFrame:"source",src:src},"*");A.set(v,a);return a}
        function report(){var v=video(),e=v&&voice(v,false);if(!e)return;var s=e.ended?0:(w.on||!e.paused)?(!e.paused&&!e.seeking&&e.readyState>=3?1:3):2;
        parent.postMessage({fmdjFrame:"voice",t:e.currentTime||0,d:isFinite(e.duration)?e.duration:0,s:s},"*")}
        function route(e){
        if(e.fmNode)return true;
        if(tries>4||!(e===video()?blob(e):e.fmCors&&e.readyState>=1))return false;
        var C=window.AudioContext||window.webkitAudioContext;if(!C)return false;
        try{ctx=ctx||new C()}catch(x){tries=9;return false}
        if(ctx.state!=="running"){if(!waking){waking=1;tries++;ctx.resume().then(function(){waking=0;apply()},function(){waking=0})}return false}
        try{var s=ctx.createMediaElementSource(e);if(!amp){amp=ctx.createGain();amp.connect(ctx.destination)}if(live&&live.fmNode)try{live.fmNode.disconnect()}catch(x){}s.connect(amp);e.fmNode=s;live=e;
        ctx.onstatechange=function(){if(ctx.state!=="running"&&ctx.state!=="closed")ctx.resume().catch(function(){})};
        parent.postMessage({fmdjFrame:"routed",mirror:e.tagName==="AUDIO"?1:0},"*");return true}catch(x){tries=9;return false}}
        function apply(){
        var v=video();if(!v)return;seal(v);var a=voice(v,false),t=[v];if(a&&a!==v)t.push(a);
        t.forEach(pitch);
        if(a&&(w.gain<0.999||a.fmNode)&&(a.fmNode||!w.bg)&&route(a))amp.gain.setTargetAtTime(Math.max(0,Math.min(1,w.gain)),ctx.currentTime,0.012);
        if(scratching()){var s=w.scratch;t.forEach(function(e){
        if(s>0.08){if(Math.abs(e.currentTime-w.at)>0.35)e.currentTime=w.at;speed(e,Math.min(4,Math.max(0.25,s)));if(e.paused)start(e)}
        else{if(!e.paused)e.pause();if(Math.abs(e.currentTime-w.at)>0.04)e.currentTime=w.at}})}
        else t.forEach(function(e){speed(e,w.rate)})}
        function sync(){
        var v=video();if(!v)return;seal(v);var a=voice(v,true);if(last&&last!==a&&last.tagName==="AUDIO")quiet(last);last=a;
        if(a&&a!==v){
        var m=want(v);if(a.muted!==m)a.muted=m;
        if(!scratching()){speed(a,w.rate);
        if(w.on){if(a.paused)start(a)}else if(!a.paused)a.pause();
        if(!w.bg&&a.readyState>=1&&v.readyState>=1&&!a.seeking&&!v.seeking&&!a.ended&&!v.ended&&Math.abs(a.currentTime-v.currentTime)>1&&Date.now()-moved>3000){moved=Date.now();v.fmOurs=a.currentTime;try{v.currentTime=a.currentTime}catch(x){}}}}
        standby(v,a);report()}
        function drop(){if(spare){quiet(spare);spare=null}}
        function swap(v,a,go){var s=spare;spare=null;speed(s,scratching()?1:w.rate);s.muted=a.muted;A.set(v,s);if(last===a)last=s;if(a.fmNode){try{a.fmNode.disconnect()}catch(x){}if(live===a)live=null}quiet(a);if(go)start(s);else if(!s.paused)s.pause()}
        function standby(v,a){
        if(!a||a===v||!a.fmNode||a.fmLone||!w.on||w.bg||scratching()||want(v)||a.ended||!(w.gain>=0.999||w.away)){drop();return}
        if(spare&&spare.error){a.fmLone=1;drop();return}
        if(!spare||spare.fmSrc!==a.fmSrc){drop();spare=fresh(v,a.fmSrc,a.fmCors,a.currentTime+0.2);spare.muted=true;spare.fmMoved=Date.now()}
        if(spare.paused)start(spare);
        if(spare.readyState<3||spare.seeking||a.seeking||a.paused){speed(spare,w.rate);return}
        var d=spare.currentTime-a.currentTime;
        if(Math.abs(d)>0.6){if(Date.now()-spare.fmMoved>2000){spare.fmMoved=Date.now();try{spare.currentTime=a.currentTime+0.2}catch(x){}}return}
        if(w.gain>=0.999&&Math.abs(d)<0.03){swap(v,a,true);return}
        speed(spare,w.rate*(1-Math.max(-0.15,Math.min(0.15,d))))}
        function rescue(){var v=video(),a=v&&voice(v,false);if(!a||a===v||!a.fmNode)return;
        if(spare&&spare.fmSrc===a.fmSrc&&!spare.error){if(Math.abs(spare.currentTime-a.currentTime)>0.08)try{spare.currentTime=a.currentTime}catch(x){}}
        else{drop();spare=fresh(v,a.fmSrc,a.fmCors,a.currentTime)}
        swap(v,a,!a.paused)}
        document.addEventListener("seeking",function(e){var v=e.target;if(!own(v))return;var o=v.fmOurs;v.fmOurs=null;if(o!=null&&Math.abs(v.currentTime-o)<0.05)return;
        var a=voice(v,false);if(!a||a===v)return;if(Math.abs(a.currentTime-v.currentTime)>(a.paused?0.02:0.25))try{a.currentTime=v.currentTime}catch(x){}},true);
        ["loadstart","emptied","loadedmetadata","resize","play","playing","pause","ended","ratechange"].forEach(function(t){document.addEventListener(t,function(e){if(!own(e.target))return;if(t==="loadstart"||t==="emptied")stale="";if(t==="loadstart"||t==="emptied"||t==="loadedmetadata"||t==="resize")seal(e.target);if(t==="ratechange"||t==="playing"||t==="loadedmetadata")apply();soon()},true)});
        addEventListener("message",function(e){var m=e.data;if(e.source!==parent||!m||typeof m!=="object"||m.fmdj!==1)return;var was=w.bg;if(w.gen!=null&&m.gen!==w.gen){var v=video(),a=v&&A.get(v);stale=v?v.currentSrc||v.src||"":"";drop();if(a){quiet(a);A.delete(v)}}w=m;if(w.bg&&!was)rescue();apply();sync()});
        setInterval(function(){beat++;if(w.on||beat%4===0)sync()},250);
        parent.postMessage({fmdjFrame:"hello"},"*");
        })();
        """
}

@MainActor
private final class DjScriptProxy: NSObject, WKScriptMessageHandler {
    weak var target: DjPlayerHost?

    init(_ target: DjPlayerHost) { self.target = target }

    func userContentController(_ userContentController: WKUserContentController, didReceive message: WKScriptMessage) {
        target?.receive(message.body)
    }
}
