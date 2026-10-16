import FMPackages
import SwiftUI
import UIKit
import WebKit

enum DjPlayerEvent {
    case ready
    case tick(time: Double, duration: Double, state: Int)
    case state(Int)
    case meta(title: String, author: String)
    case failed(Int)
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
    func setVolume(_ volume: DjVolume) { host.setVolume(side, volume) }
    func scratch(_ speed: Double?, at seconds: Double) { host.scratch(side, speed, at: seconds) }
}

@MainActor
final class DjPlayerHost: NSObject, WKNavigationDelegate {
    let webView: WKWebView
    var onEvent: ((DjDeckSide, DjPlayerEvent) -> Void)?

    private let origin: URL
    private var apiReady = false
    private var pending: [DjDeckSide: String] = [:]
    private var lastLoad: [DjDeckSide: String] = [:]
    private var lastVolume: [DjDeckSide: DjVolume] = [:]
    private var handOff: UIBackgroundTaskIdentifier = .invalid

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

    func setRate(_ side: DjDeckSide, _ rate: Double) { run("fmRate('\(side.rawValue)',\(Self.number(rate)))") }

    func scratch(_ side: DjDeckSide, _ speed: Double?, at seconds: Double) {
        run("fmScratch('\(side.rawValue)',\(speed.map(Self.number) ?? "null"),\(Self.number(seconds)))")
    }

    func setVolume(_ side: DjDeckSide, _ volume: DjVolume) {
        guard apiReady, lastVolume[side] != volume else { return }
        lastVolume[side] = volume
        run("fmVolume('\(side.rawValue)',\(volume.level),\(volume.muted),\(Self.number(volume.gain)))")
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
        default:
            break
        }
    }

    private func run(_ script: String) {
        guard apiReady else { return }
        webView.evaluateJavaScript(script, completionHandler: nil)
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
    }

    private static let html = """
        <!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
        <style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:#000}.s{position:absolute;top:0;bottom:0;width:50%}#sa{left:0}#sb{right:0}.s>*{position:absolute;inset:0;width:100%;height:100%;border:0}</style>
        </head><body><div class="s" id="sa"><div id="a"></div></div><div class="s" id="sb"><div id="b"></div></div><script>
        var K=["a","b"],D={},n=0,B=0;
        K.forEach(function(k){D[k]={p:null,ready:false,meta:"",routed:false,w:{rate:1,volume:100,muted:false,gain:1,scratch:null,at:0}}});
        function post(m){try{webkit.messageHandlers.deck.postMessage(m)}catch(e){}}
        function onYouTubeIframeAPIReady(){post({type:"api"})}
        function frame(k){try{var f=D[k].p&&D[k].p.getIframe&&D[k].p.getIframe();return f&&f.contentWindow}catch(e){return null}}
        function push(k){var f=frame(k),x=D[k].w;if(f)f.postMessage({fmdj:1,rate:x.rate,gain:x.gain,scratch:x.scratch,at:x.at,bg:B},"*")}
        function fmCall(k,name){var d=D[k];if(!d.p||!d.ready||typeof d.p[name]!=="function")return;d.p[name].apply(d.p,[].slice.call(arguments,2));setTimeout(function(){tick(k)},60)}
        function fmStop(k){D[k].w.scratch=null;fmCall(k,"stopVideo")}
        function fmRate(k,r){D[k].w.rate=r;push(k)}
        function fmScratch(k,s,at){var x=D[k].w;x.scratch=s;x.at=at;push(k)}
        function fmVolume(k,v,m,g){var d=D[k];d.w.volume=v;d.w.muted=m;d.w.gain=g;push(k);if(!d.p||!d.ready)return;d.p.setVolume(d.routed?100:v);if(m)d.p.mute();else d.p.unMute()}
        function tick(k){var d=D[k],p=d.p;if(!p||!d.ready||!p.getCurrentTime)return;var v=p.getVideoData?p.getVideoData():null;if(v&&v.title&&v.title!==d.meta){d.meta=v.title;post({type:"meta",deck:k,title:v.title,author:v.author||""})}post({type:"tick",deck:k,time:p.getCurrentTime()||0,duration:p.getDuration()||0,state:p.getPlayerState()})}
        function fmLoad(k,id,start,rate,v,m,g){var d=D[k];d.w.rate=rate;d.w.scratch=null;d.meta="";d.resume=false;
        if(d.p&&d.ready){d.p.cueVideoById({videoId:id,startSeconds:start});fmVolume(k,v,m,g);setTimeout(function(){tick(k)},300);return}
        d.w.volume=v;d.w.muted=m;d.w.gain=g;make(k,id,start)}
        function make(k,id,start){var d=D[k];
        if(d.p){try{d.p.destroy()}catch(e){}document.getElementById("s"+k).innerHTML='<div id="'+k+'"></div>';d.ready=false;d.routed=false}
        d.p=new YT.Player(k,{host:"https://www.youtube-nocookie.com",width:"100%",height:"100%",videoId:id,
        playerVars:{playsinline:1,controls:0,disablekb:1,fs:0,rel:0,iv_load_policy:3,cc_load_policy:0,enablejsapi:1,start:Math.floor(start),origin:location.origin},
        events:{onReady:function(){d.ready=true;if(d.resume){d.resume=false;d.p.loadVideoById({videoId:id,startSeconds:start})}fmVolume(k,d.w.volume,d.w.muted,d.w.gain);post({type:"ready",deck:k});tick(k)},
        onStateChange:function(e){if(e.data===1)push(k);post({type:"state",deck:k,state:e.data});tick(k)},
        onError:function(e){post({type:"error",deck:k,code:e.data})}}})}
        function fmBackground(on){B=on?1:0;K.forEach(function(k){var d=D[k],p=d.p;if(B&&d.routed&&p&&d.ready&&!d.w.muted&&d.w.gain>=0.02){var s=p.getPlayerState(),v=p.getVideoData?p.getVideoData():null;if((s===1||s===3)&&v&&v.video_id){d.resume=true;make(k,v.video_id,p.getCurrentTime()||0);return}}push(k)})}
        addEventListener("message",function(e){var m=e.data;if(!m||typeof m!=="object"||!m.fmdjFrame)return;K.forEach(function(k){if(!e.source||frame(k)!==e.source)return;var d=D[k];if(m.fmdjFrame==="routed"){if(!d.routed){d.routed=true;fmVolume(k,d.w.volume,d.w.muted,d.w.gain)}}else push(k)})});
        setInterval(function(){n++;K.forEach(function(k){var d=D[k];if(!d.p||!d.ready)return;var s=d.p.getPlayerState();if(s===1||s===3||n%4===0)tick(k)})},250);
        </script><script src="https://www.youtube.com/iframe_api"></script></body></html>
        """

    private static let frameScript = """
        (function(){
        if(window.top===window||!/(^|\\.)youtube(-nocookie)?\\.com$/.test(location.hostname))return;
        var w={rate:1,gain:1,scratch:null,at:0,bg:0},ctx=null,amp=null,routed=null,tries=0;
        var P=HTMLMediaElement.prototype,md=Object.getOwnPropertyDescriptor(P,"muted"),H=new WeakMap(),T=new WeakMap(),decoys=[];
        function held(v){return H.get(v)||{want:md.get.call(v),hold:false}}
        function hold(v,on){var h=held(v);h.hold=on;H.set(v,h);md.set.call(v,on||h.want)}
        Object.defineProperty(P,"muted",{configurable:true,enumerable:md.enumerable,get:function(){var h=H.get(this);return h?h.want:md.get.call(this)},set:function(x){var h=held(this);h.want=!!x;H.set(this,h);md.set.call(this,h.want||h.hold)}});
        var play=P.play;P.play=function(){if(this.tagName==="VIDEO"&&this.readyState<1&&decoys.indexOf(this)<0)hold(this,true);return play.apply(this,arguments)};
        function desc(o,n){for(;o;o=Object.getPrototypeOf(o)){var d=Object.getOwnPropertyDescriptor(o,n);if(d)return d}return null}
        function busy(t){for(var i=0;i<t.sourceBuffers.length;i++)if(t.sourceBuffers[i].updating)return true;return false}
        var S=window.MediaSource;
        if(S&&S.prototype&&S.prototype.addSourceBuffer){
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
        document.addEventListener("loadedmetadata",function(e){var v=e.target,h=v&&H.get(v);if(h&&h.hold)hold(v,false)},true);
        try{var DP=Document.prototype;Object.defineProperty(DP,"hidden",{configurable:true,get:function(){return false}});Object.defineProperty(DP,"visibilityState",{configurable:true,get:function(){return "visible"}});Object.defineProperty(DP,"webkitHidden",{configurable:true,get:function(){return false}});Object.defineProperty(DP,"webkitVisibilityState",{configurable:true,get:function(){return "visible"}})}catch(e){}
        ["visibilitychange","webkitvisibilitychange"].forEach(function(t){addEventListener(t,function(e){e.stopImmediatePropagation()},true)});
        function video(){return document.querySelector("video")}
        function speed(v,r){if(Math.abs(v.playbackRate-r)>0.001)v.playbackRate=r}
        function route(v){
        if(routed===v)return true;
        if(routed||tries>4||String(v.currentSrc||"").indexOf("blob:")!==0)return false;
        var A=window.AudioContext||window.webkitAudioContext;if(!A)return false;
        try{ctx=ctx||new A()}catch(e){tries=9;return false}
        if(ctx.state!=="running"){tries++;ctx.resume().then(apply,function(){});return false}
        try{var s=ctx.createMediaElementSource(v);amp=ctx.createGain();s.connect(amp);amp.connect(ctx.destination);routed=v;
        ctx.onstatechange=function(){if(ctx.state!=="running"&&ctx.state!=="closed")ctx.resume().catch(function(){})};
        parent.postMessage({fmdjFrame:"routed"},"*");return true}catch(e){tries=9;return false}}
        function apply(){
        var v=video();if(!v)return;
        if(v.preservesPitch!==false)try{v.preservesPitch=false}catch(e){}
        if(v.webkitPreservesPitch)try{v.webkitPreservesPitch=false}catch(e){}
        if((w.gain<0.999||routed)&&(routed===v||!w.bg)&&route(v))amp.gain.setTargetAtTime(Math.max(0,Math.min(1,w.gain)),ctx.currentTime,0.012);
        if(typeof w.scratch==="number"){var s=w.scratch;
        if(s>0.08){if(Math.abs(v.currentTime-w.at)>0.35)v.currentTime=w.at;speed(v,Math.min(4,Math.max(0.25,s)));if(v.paused){var q=v.play();if(q&&q.catch)q.catch(function(){})}}
        else{if(!v.paused)v.pause();if(Math.abs(v.currentTime-w.at)>0.04)v.currentTime=w.at}}
        else speed(v,w.rate)}
        addEventListener("message",function(e){var m=e.data;if(e.source!==parent||!m||typeof m!=="object"||m.fmdj!==1)return;w=m;apply()});
        ["ratechange","playing","loadedmetadata"].forEach(function(t){document.addEventListener(t,function(e){if(e.target&&e.target.tagName==="VIDEO")apply()},true)});
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
