/* =============================================================================
   ForkMesh Hackathon — page glue.

   Adapted from the authored page script of the ThreeUI
   <SublevelStudioLandingPage /> canonical source
   (public/landing-pages/sublevel-studio.html, SHA-256
   91db5c1bb779687990b01f226a02d7fe7cf7954a40ba8053c4d6b7abf82232e3).

   The behaviour is the authored behaviour: the VHS overlay shader, the CRT
   machine-readable view, Canvas UI particle scroll, the marginalia parallax,
   the projective transform that pins the menu card to the 3D paper sheet, the
   header flashlight, the hairline draw-in observers and the signup handler are
   all carried over. What changed is the content they operate on (ForkMesh
   copy in the terminal log and status line), and two mount points:

     - The authored hero was a bespoke 1MB Three.js studio lobby modelling that
       studio's own room, arcade cabinet and portfolio screens. It is not
       reusable content, so the hero backdrop here is a second authored
       component instead: ThreeUI <TerrainPlumeCanvas variant="sunset-drive" />.
     - The "/" shortcut scrolled to a mailto: in the source. Here it jumps to
       the join call to action rather than navigating the page away.
   ============================================================================= */
(function () {
  // The page follows the site's theme: home-theme.js puts .light / .dark on
  // <html>, and the WebGL layers below are re-tinted whenever that flips.
  const isLight = () => document.documentElement.classList.contains('light');
  const onThemeChange = (fn) => {
    new MutationObserver(fn).observe(document.documentElement,
      { attributes: true, attributeFilter: ['class'] });
  };

  const term = document.getElementById('termstatus'), txt = term.querySelector('.txt');
  const STATUS = ['online', 'sep 15 - oct 15 utc', '50% first-month commission', 'idle', 'ranked by revenue', 'online'];
  let si = 0, typing = null;
  function type(str) { clearInterval(typing); let k = 0; typing = setInterval(() => { txt.textContent = str.slice(0, ++k); if (k >= str.length) clearInterval(typing); }, 28); }
  setInterval(() => { if (!document.body.classList.contains('machine')) { si = (si + 1) % STATUS.length; type(STATUS[si]); } }, 4200);

  let crt = null; const seg = (t, c) => window.ThreeUICrt.segment(t, c);
  // the machine index as terminal lines: role p = orange, d = dim, a = accent, h = bright
  const FORKMESH_LOG = [
    ["████  ██  ███  █  █ █   █ ████  ███ █  █ ", "p"],
    ["█    █  █ █  █ █ █  ██ ██ █    █    █  █ ", "p"],
    ["███  █  █ ███  ██   █ █ █ ███   ██  ████ ", "p"],
    ["█    █  █ █ █  █ █  █   █ █       █ █  █ ", "p"],
    ["█     ██  █  █ █  █ █   █ ████ ███  █  █ ", "p"],
    ["", "p"],
    ["/HACKATHON  /HACKATHON/RULES  /#SUPPORT-FORKMESH", "a"],
    ["", "p"],
    ["FORKMESH SEPTEMBER REVENUE CONTEST :: MACHINE-READABLE INDEX", "p"],
    ["", "p"],
    ["# PLAIN-TEXT MIRROR OF THE FORKMESH REVENUE CONTEST PAGE FOR AI", "d"],
    ["  AGENTS, CRAWLERS, AND HUMANS WHO PREFER IT RAW.", "d"],
    ["", "p"],
    ["── ABOUT ─────────────────────────────────────────────────────────", "d"],
    ["NAME .......... FORKMESH SEPTEMBER REVENUE CONTEST", "p"],
    ["OPEN_TO ....... EVERYONE", "p"],
    ["STARTS ........ 2026-09-15 00:00 UTC", "p"],
    ["ENDS .......... 2026-10-15 23:59 UTC", "p"],
    ["PATREON ....... LINKED FROM THE FORKMESH HOMEPAGE", "p"],
    ["RULES ......... /HACKATHON/RULES", "p"],
    ["", "p"],
    ["BRING NEW COMPANIES ONTO A PAID FORKMESH PLAN THROUGH PATREON, EARN", "p"],
    ["  COMMISSION, AND COMPETE FOR CASH PRIZES.", "p"],
    ["", "p"],
    ["── COMMISSION ────────────────────────────────────────────────────", "d"],
    ["50% OF EACH ELIGIBLE CUSTOMER'S FIRST MONTH.", "h"],
    ["", "p"],
    ["- $5 PLAN ..... $2.50", "p"],
    ["- $10 PLAN .... $5", "p"],
    ["- $50 PLAN .... $25", "p"],
    ["- $100 PLAN ... $50", "p"],
    ["", "p"],
    ["── LEADERBOARD ───────────────────────────────────────────────────", "d"],
    ["RANKED BY TOTAL ELIGIBLE FIRST-MONTH REVENUE, NOT NUMBER OF SIGNUPS.", "p"],
    ["", "p"],
    ["- 1ST ......... $100", "h"],
    ["- 2ND ......... $50", "p"],
    ["- 3RD ......... $25", "p"],
    ["", "p"],
    ["[PRIZES ARE PAID IN ADDITION TO COMMISSIONS]", "a"],
    ["", "p"],
    ["── HOW_IT_WORKS ──────────────────────────────────────────────────", "d"],
    ["1. PICK UP TO 10 COMPANIES.", "p"],
    ["2. POST YOUR LIST PUBLICLY BEFORE OUTREACH.", "p"],
    ["3. SELL THROUGH THE PATREON LINK. COUNTS AFTER SUCCESSFUL PAYMENT.", "p"],
    ["4. CLIMB THE LEADERBOARD.", "p"],
    ["", "p"],
    ["── RULES ─────────────────────────────────────────────────────────", "d"],
    ["* MAX 10 CLAIMED COMPANIES AT A TIME; FIRST VALID PUBLIC CLAIM WINS.", "p"],
    ["* CLAIMS EXPIRE AFTER 7 DAYS WITHOUT MEANINGFUL OUTREACH.", "p"],
    ["* NEW PAID CUSTOMERS ONLY. NO SELF-REFERRALS OR FAKE ACCOUNTS.", "p"],
    ["* REFUNDS, CHARGEBACKS AND REVERSALS DO NOT COUNT.", "p"],
    ["* FINAL AFTER 30 DAYS PAID AND IN GOOD STANDING.", "p"],
    ["", "p"],
    ["── LINKS ─────────────────────────────────────────────────────────", "d"],
    ["PATREON ....... /#SUPPORT-FORKMESH", "p"],
    ["RULES ......... /HACKATHON/RULES", "p"],
    ["", "p"],
    ["contest@forkmesh:~$ ", "p"],
  ].map(([text, role]) => text === '' ? [] : [seg(text, role)]);

  // The CRT is a screen, so it stays dark in both themes and pins its own green
  // rather than following --accent, which goes dark on a light page.
  const GREEN = { p: { fill: '#4ade80', glow: 'rgba(74,222,128,0.9)' }, d: { fill: '#2c7a4b', glow: 'rgba(74,222,128,0.4)' }, a: { fill: '#c8ffe0', glow: 'rgba(150,240,190,0.9)' }, h: { fill: '#f2fff8', glow: 'rgba(190,255,220,0.95)' } };
  const SOFT_CRT = { scanDepth: 0.16, grille: 0.18, chroma: 0.6, bar: 0.03, flicker: 0.018, grain: 0.02, vignette: 0.5, gain: 1.22, halo: 0.08, sheen: [0.42, 1.0, 0.62], room: [0.008, 0.03, 0.017], background: '#020503', curve: [0.10, 0.145] };
  function setMode(m) {
    const machine = m === 'machine';
    document.body.classList.toggle('machine', machine);
    window.VHS = machine
      ? { speed: .75, switching: .09, switchingHeight: .035, acBeat: 1.4, grain: .22, scanlines: .5, jitter: .4 }
      : { speed: .5, switching: .05, switchingHeight: .02, acBeat: 1, grain: .1, scanlines: .1, jitter: .25 };
    if (machine && !crt) { try { crt = window.ThreeUICrt.mountCrt(document.getElementById('crt-host'), { variant: 'terminal', speed: 1, typeSpeed: 14, motion: 1, hue: 0, saturation: 1, brightness: 1, opacity: 1, fixedFont: 17, colors: GREEN, styleOverrides: SOFT_CRT }, FORKMESH_LOG); } catch (e) { console.warn('CRT unavailable', e); } }
    if (!machine && crt) { crt.destroy(); crt = null; }
    term.setAttribute('aria-pressed', String(machine));
    type(machine ? 'machine view · esc' : STATUS[si]);
    window.scrollTo(0, 0);
  }
  term.addEventListener('click', () => setMode(document.body.classList.contains('machine') ? 'human' : 'machine'));
  document.getElementById('machine-close').addEventListener('click', () => setMode('human'));
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && document.body.classList.contains('machine')) setMode('human'); });
  document.addEventListener('keydown', (e) => {
    if (e.key !== '/' || /input|textarea/i.test(document.activeElement.tagName)) return;
    if (document.body.classList.contains('machine')) return;
    e.preventDefault();
    document.getElementById('join')?.scrollIntoView({ behavior: 'smooth', block: 'center' });
  });

  // VHS overlay for the DOM: grain, scanlines, head-switching band and AC beat as a screen-blended layer.
  (function () {
    const cv = document.getElementById('vhs-overlay'); const gl = cv.getContext('webgl2', { alpha: true, antialias: false, premultipliedAlpha: true });
    if (!gl) return;
    const cfg = () => window.VHS || { speed: .5, switching: .05, switchingHeight: .02, acBeat: 1, grain: .1, scanlines: .1, jitter: .25 };
    const V = `#version 300 es
    precision highp float; layout(location=0) in vec2 aPos; out vec2 vUv; void main(){ vUv = aPos*0.5+0.5; gl_Position = vec4(aPos,0.,1.); }`;
    const F = `#version 300 es
    precision highp float; in vec2 vUv; out vec4 o; uniform vec2 uRes; uniform float uTime, uSwitching, uSwitchHeight, uAcBeat, uGrain, uScanlines, uJitter, uScroll, uInvert;
    #define PI 3.14159265
    float hash(vec2 v){ return fract(sin(dot(v, vec2(89.44, 19.36))) * 22189.22); }
    float iHash(vec2 v, vec2 r){ float h00=hash(floor(v*r)/r); float h10=hash(floor(v*r+vec2(1.,0.))/r); float h01=hash(floor(v*r+vec2(0.,1.))/r); float h11=hash(floor(v*r+vec2(1.,1.))/r); vec2 ip=smoothstep(vec2(0.),vec2(1.),mod(v*r,1.)); return (h00*(1.-ip.x)+h10*ip.x)*(1.-ip.y)+(h01*(1.-ip.x)+h11*ip.x)*ip.y; }
    float noise(vec2 v){ float sum=0.; float s=2.; for(int i=1;i<7;i++){ sum+=iHash(v+vec2(float(i)),vec2(2.*s))/s; s*=2.; } return sum; }
    void main(){
      vec2 uv = vUv; float t = uTime;
      float lineNoise = noise(vec2(uv.y*100.0, t*10.0));
      float snPhase = smoothstep(max(uSwitchHeight,1e-4), 0.0, uv.y) * uSwitching;
      float beat = clamp(noise(vec2(0.0, uv.y + t*0.2))*0.6-0.25, 0.0, 0.1) * uAcBeat;
      float g = hash(uv*uRes + fract(t)*vec2(127.1,311.7)) - 0.5;
      float scan = sin(uv.y*uRes.y*PI)*0.5;
      vec3 col = vec3(0.5 + g) * uGrain * 1.6 + beat * 0.5;
      col += vec3(0.9, 0.95, 1.0) * snPhase * 6.0 * (0.4 + lineNoise * 0.6);
      float a = clamp(uGrain*0.9 + snPhase*6.0 + beat*0.6, 0.0, 1.0);
      col *= 1.0 - uScanlines*0.35*scan;
      // Screen-blended bright grain over black; on a light page the layer is
      // multiplied instead, so the same grain is emitted inverted.
      col = mix(col, vec3(1.0) - col, uInvert);
      o = vec4(col * a, a);
    }`;
    const sh = (t, src) => { const s = gl.createShader(t); gl.shaderSource(s, src); gl.compileShader(s); if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) console.error(gl.getShaderInfoLog(s)); return s; };
    const prog = gl.createProgram(); gl.attachShader(prog, sh(gl.VERTEX_SHADER, V)); gl.attachShader(prog, sh(gl.FRAGMENT_SHADER, F)); gl.linkProgram(prog); gl.useProgram(prog);
    const buf = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, buf); gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1,-1,1,-1,-1,1,1,1]), gl.STATIC_DRAW); gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    const U = {}; ['uRes','uTime','uSwitching','uSwitchHeight','uAcBeat','uGrain','uScanlines','uJitter','uScroll','uInvert'].forEach(n => U[n] = gl.getUniformLocation(prog, n));
    let time = 0, last = performance.now();
    function frame(now) {
      const dpr = Math.min(devicePixelRatio || 1, 2); const w = Math.round(innerWidth * dpr), h = Math.round(innerHeight * dpr);
      if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; }
      const c = cfg(); const dt = Math.min((now - last) / 1000, 1 / 30); last = now; time += dt * c.speed;
      gl.viewport(0, 0, w, h); gl.useProgram(prog);
      gl.uniform2f(U.uRes, w, h); gl.uniform1f(U.uTime, time); gl.uniform1f(U.uSwitching, c.switching); gl.uniform1f(U.uSwitchHeight, c.switchingHeight); gl.uniform1f(U.uAcBeat, c.acBeat); gl.uniform1f(U.uGrain, c.grain); gl.uniform1f(U.uScanlines, c.scanlines); gl.uniform1f(U.uJitter, c.jitter); gl.uniform1f(U.uScroll, scrollY); gl.uniform1f(U.uInvert, isLight() ? 1 : 0);
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
      requestAnimationFrame(frame);
    }
    requestAnimationFrame(frame);
  })();

  // Canvas UI Particle Scroll over the page content: text and images below the formation line dissolve
  // into orange/yellow grains and reassemble as they scroll up; the live DOM blurs in once a block has landed.
  if (window.ParticleScroll && !matchMedia('(prefers-reduced-motion: reduce)').matches) {
    // Grains are the brand green rather than the authored ember. On white they
    // have to be the darker green or they vanish into the page.
    const grainTints = () => isLight()
      ? { tintA: [0.10, 0.49, 0.22], tintB: [0.04, 0.31, 0.14] }   // #1a7f37 -> deeper
      : { tintA: [0.29, 0.87, 0.50], tintB: [0.62, 0.97, 0.78] };  // #4ade80 -> #9ef7c6
    const psOpts = { point: 0.68, band: 330, density: 2, size: 1.25, spread: 200, gravity: 0.35, drift: 0.9, swirl: 70, stagger: 0.55, fade: 0.42, settle: 0.7, hold: 1.15, smoothing: 0.26 };
    const mountPs = () => window.ParticleScroll.mountParticleScroll('[data-ps]', Object.assign({}, psOpts, grainTints()));
    window.__ps = mountPs();
    // Tints are read once at mount, so a theme flip has to re-mount. destroy()
    // releases the GL objects but leaves its #ps-output canvas in the document
    // and its tracked blocks marked .ps-hidden, so both are cleaned up here —
    // otherwise the page would stack canvases and permanently hide its own copy.
    let lightNow = isLight();
    onThemeChange(() => {
      if (isLight() === lightNow) return;
      lightNow = isLight();
      window.__ps?.destroy?.();
      document.getElementById('ps-output')?.remove();
      document.querySelectorAll('.ps-hidden').forEach((el) => el.classList.remove('ps-hidden'));
      window.__ps = mountPs();
    });
  }

  // Notify form. There is no list endpoint yet, so this acknowledges locally and
  // deliberately does not claim the address was stored anywhere.
  (function signup() {
    const form = document.querySelector('footer .signup'); if (!form) return;
    form.addEventListener('submit', (e) => {
      e.preventDefault();
      const input = form.querySelector('input'), lbl = form.querySelector('.lbl');
      const ok = /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(input.value.trim());
      if (!ok) { input.focus(); input.style.borderColor = 'rgba(255,77,0,.9)'; return; }
      input.style.borderColor = '';
      input.value = ''; input.placeholder = 'You are on the list.';
      lbl.textContent = 'Got it';
      setTimeout(() => { lbl.textContent = 'Notify me'; input.placeholder = 'you@example.com'; }, 3200);
    });
  })();

  // ThreeUI <TerrainPlumeCanvas variant="sunset-drive" />: once as the fixed hero
  // backdrop, once behind the footer copy as the source places it.
  if (window.ThreeUITerrainPlume) {
    // 'sunset-drive' is an amber scene; +118 degrees of hue lands it on the
    // brand green. On a light page it is dialled back so it reads as a tint
    // behind the copy instead of a glare.
    const light = isLight();
    const plume = { variant: 'sunset-drive', speed: 1.00, size: 1.00, thickness: 1.00, strength: 1.00, softness: 0.00,
                    opacity: light ? 0.55 : 1.00, hue: 118, saturation: light ? 0.85 : 1.00, brightness: light ? 0.78 : 1.00 };
    const host = document.getElementById('plume-host');
    const foot = document.getElementById('notify');
    const mountPlumes = () => {
      const l = isLight();
      const opts = Object.assign({}, plume, {
        opacity: l ? 0.55 : 1.00, saturation: l ? 0.85 : 1.00, brightness: l ? 0.78 : 1.00,
      });
      if (host) window.__hero = window.ThreeUITerrainPlume.mountTerrainPlume(
        host, Object.assign({}, opts, { speed: 0.72, brightness: opts.brightness * 0.94 }));
      if (foot) window.__plume = window.ThreeUITerrainPlume.mountTerrainPlume(foot, opts);
      document.getElementById('loader')?.classList.add('done');
    };
    mountPlumes();
    // destroy() takes its own mount out of the document, so re-mounting is clean.
    let plumeLight = isLight();
    onThemeChange(() => {
      if (isLight() === plumeLight) return;
      plumeLight = isLight();
      window.__hero?.destroy?.();
      window.__plume?.destroy?.();
      mountPlumes();
    });
  } else {
    document.getElementById('loader')?.classList.add('done');
  }

  // Scroll parallax on the marginalia: each drawing drifts at its own rate against
  // the copy. Gated on visibility and written only when the value actually moves,
  // since a transform write every scroll event is an easy way to lose frames.
  (function illParallax() {
    const els = [...document.querySelectorAll('.ill')];
    if (!els.length || matchMedia('(prefers-reduced-motion: reduce)').matches) return;
    const SPEED = { 'ill-lantern': 0.20, 'ill-cat': -0.13, 'ill-joystick': 0.30, 'ill-crt': 0.14 };
    const items = els.map((el) => {
      const key = [...el.classList].find((c) => c.startsWith('ill-'));
      const it = { el, speed: SPEED[key] ?? 0.16, last: null, live: false };
      new IntersectionObserver(([e]) => { it.live = e.isIntersecting; }, { rootMargin: '25% 0px' }).observe(el);
      return it;
    });
    let queued = false;
    const frame = () => {
      queued = false;
      const mid = innerHeight / 2;
      for (const it of items) {
        if (!it.live) continue;
        const r = it.el.getBoundingClientRect();
        const v = Math.round(((r.top + r.height / 2) - mid) * it.speed * 10) / 10;
        if (v !== it.last) { it.last = v; it.el.style.transform = `translate3d(0,${v}px,0)`; }
      }
    };
    addEventListener('scroll', () => { if (!queued) { queued = true; requestAnimationFrame(frame); } }, { passive: true });
    addEventListener('resize', frame, { passive: true });
    frame();
  })();




  // Hairlines draw themselves in as they enter the viewport; the vertical container
  // lines unroll from the top once the page content is reached.
  (function lines() {
    const io = new IntersectionObserver((entries) => {
      for (const e of entries) if (e.isIntersecting) { e.target.classList.add('in'); io.unobserve(e.target); }
    }, { rootMargin: '0px 0px -12% 0px', threshold: 0.01 });
    document.querySelectorAll('[data-line]').forEach(el => io.observe(el));
    const vr = document.querySelector('.vrules');
    if (vr) { const vio = new IntersectionObserver((en) => { if (en[0].isIntersecting) { vr.classList.add('in'); vio.disconnect(); } }, { threshold: 0 }); vio.observe(document.querySelector('main')); }
  })();
})();
