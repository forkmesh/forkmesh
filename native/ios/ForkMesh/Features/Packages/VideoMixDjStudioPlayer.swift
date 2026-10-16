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

@MainActor
final class DjYouTubePlayer: NSObject, WKNavigationDelegate {
    let webView: WKWebView
    var onEvent: ((DjPlayerEvent) -> Void)?

    private let origin: URL
    private var apiReady = false
    private var pending: String?
    private var lastLoad: String?
    private var lastVolume: (Int, Bool)?

    init(origin: URL) {
        self.origin = origin
        let configuration = WKWebViewConfiguration()
        configuration.allowsInlineMediaPlayback = true
        configuration.mediaTypesRequiringUserActionForPlayback = []
        configuration.allowsPictureInPictureMediaPlayback = false
        configuration.suppressesIncrementalRendering = false
        let controller = WKUserContentController()
        configuration.userContentController = controller
        webView = WKWebView(frame: CGRect(x: 0, y: 0, width: 320, height: 180), configuration: configuration)
        super.init()
        controller.add(DjScriptProxy(self), name: "deck")
        webView.navigationDelegate = self
        webView.isOpaque = false
        webView.backgroundColor = .black
        webView.scrollView.backgroundColor = .black
        webView.scrollView.isScrollEnabled = false
        webView.scrollView.contentInsetAdjustmentBehavior = .never
        webView.isUserInteractionEnabled = false
        webView.allowsLinkPreview = false
        webView.loadHTMLString(Self.html, baseURL: origin)
    }

    func load(_ id: String, start: Double, rate: Double, volume: Int, muted: Bool) {
        guard DjStudio.isVideoId(id) else { return }
        lastVolume = (volume, muted)
        let script = "fmLoad('\(id)',\(Self.number(start)),\(Self.number(rate)),\(volume),\(muted))"
        lastLoad = script
        if apiReady { run(script) } else { pending = script }
    }

    func play() { run("fmCall('playVideo')") }
    func pause() { run("fmCall('pauseVideo')") }
    func stop() {
        lastLoad = nil
        pending = nil
        run("fmCall('stopVideo')")
    }

    func seek(_ seconds: Double) { run("fmCall('seekTo',\(Self.number(seconds)),true)") }
    func setRate(_ rate: Double) { run("fmRate(\(Self.number(rate)))") }

    func setVolume(_ volume: Int, muted: Bool) {
        guard apiReady else { return }
        if let lastVolume, lastVolume == (volume, muted) { return }
        lastVolume = (volume, muted)
        run("fmVolume(\(volume),\(muted))")
    }

    fileprivate func receive(_ body: Any) {
        guard let message = body as? [String: Any], let type = message["type"] as? String else { return }
        let number = { (key: String) -> Double in (message[key] as? NSNumber)?.doubleValue ?? 0 }
        switch type {
        case "api":
            apiReady = true
            if let pending {
                self.pending = nil
                run(pending)
            }
        case "ready":
            lastVolume = nil
            onEvent?(.ready)
        case "tick":
            onEvent?(.tick(time: number("time"), duration: number("duration"), state: Int(number("state"))))
        case "state":
            onEvent?(.state(Int(number("state"))))
        case "meta":
            onEvent?(.meta(title: message["title"] as? String ?? "", author: message["author"] as? String ?? ""))
        case "error":
            onEvent?(.failed(Int(number("code"))))
        default:
            break
        }
    }

    private func run(_ script: String) {
        guard apiReady else { return }
        webView.evaluateJavaScript(script, completionHandler: nil)
    }

    private static func number(_ value: Double) -> String {
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
        onEvent?(.failed(-1))
    }

    func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
        apiReady = false
        pending = lastLoad
        webView.loadHTMLString(Self.html, baseURL: origin)
    }

    private static let html = """
        <!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
        <style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:#000}#p,iframe{position:absolute;inset:0;width:100%;height:100%;border:0}</style>
        </head><body><div id="p"></div><script>
        var p=null,ready=false,meta="",want={rate:1,volume:100,muted:false},n=0;
        function post(m){try{webkit.messageHandlers.deck.postMessage(m)}catch(e){}}
        function onYouTubeIframeAPIReady(){post({type:"api"})}
        function fmCall(name){if(!p||!ready||typeof p[name]!=="function")return;p[name].apply(p,[].slice.call(arguments,1));setTimeout(tick,60)}
        function fmRate(r){want.rate=r;if(p&&ready)p.setPlaybackRate(r)}
        function fmVolume(v,m){want.volume=v;want.muted=m;if(!p||!ready)return;p.setVolume(v);if(m)p.mute();else p.unMute()}
        function tick(){if(!p||!ready||!p.getCurrentTime)return;var d=p.getVideoData?p.getVideoData():null;if(d&&d.title&&d.title!==meta){meta=d.title;post({type:"meta",title:d.title,author:d.author||""})}post({type:"tick",time:p.getCurrentTime()||0,duration:p.getDuration()||0,state:p.getPlayerState()})}
        function fmLoad(id,start,rate,volume,muted){want={rate:rate,volume:volume,muted:muted};meta="";
        if(p&&ready){p.cueVideoById({videoId:id,startSeconds:start});fmVolume(volume,muted);p.setPlaybackRate(rate);setTimeout(tick,300);return}
        if(p){try{p.destroy()}catch(e){}document.body.insertAdjacentHTML("afterbegin",'<div id="p"></div>');ready=false}
        p=new YT.Player("p",{host:"https://www.youtube-nocookie.com",width:"100%",height:"100%",videoId:id,
        playerVars:{playsinline:1,controls:0,disablekb:1,fs:0,rel:0,iv_load_policy:3,cc_load_policy:0,enablejsapi:1,start:Math.floor(start),origin:location.origin},
        events:{onReady:function(){ready=true;fmVolume(want.volume,want.muted);p.setPlaybackRate(want.rate);post({type:"ready"});tick()},
        onStateChange:function(e){if((e.data===1||e.data===5)&&p.getPlaybackRate()!==want.rate)p.setPlaybackRate(want.rate);post({type:"state",state:e.data});tick()},
        onError:function(e){post({type:"error",code:e.data})}}})}
        setInterval(function(){if(!p||!ready)return;var s=p.getPlayerState();if(s===1||s===3||++n%4===0)tick()},250);
        </script><script src="https://www.youtube.com/iframe_api"></script></body></html>
        """
}

@MainActor
private final class DjScriptProxy: NSObject, WKScriptMessageHandler {
    weak var target: DjYouTubePlayer?

    init(_ target: DjYouTubePlayer) { self.target = target }

    func userContentController(_ userContentController: WKUserContentController, didReceive message: WKScriptMessage) {
        target?.receive(message.body)
    }
}

struct DjVideoSurface: UIViewRepresentable {
    let player: DjYouTubePlayer

    func makeUIView(context: Context) -> UIView {
        let host = UIView()
        host.backgroundColor = .black
        host.isUserInteractionEnabled = false
        host.clipsToBounds = true
        adopt(host)
        return host
    }

    func updateUIView(_ host: UIView, context: Context) {
        if player.webView.superview !== host { adopt(host) }
    }

    private func adopt(_ host: UIView) {
        let view = player.webView
        view.removeFromSuperview()
        view.frame = host.bounds
        view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        host.addSubview(view)
    }
}
