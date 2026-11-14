// The page's motion, and nothing else: no request is made, nothing is stored, nothing about the visitor is read but
// the "less motion" preference. Without this file the page is complete and still.
(function () {
  'use strict';
  var root = document.documentElement;
  var calm = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  root.classList.add('js');

  // ---- blocks rise into place the first time they are seen; the performance table fills its bars and counts up
  function countUp(el) {
    var m = /^(\d+)(.*)$/.exec(el.textContent);
    if (!m) return;
    var end = +m[1], tail = m[2], t0 = 0;
    function step(t) {
      if (!t0) t0 = t;
      var k = Math.min(1, (t - t0) / 1100), e = 1 - Math.pow(1 - k, 3);
      el.textContent = Math.round(end * e) + tail;
      if (k < 1) requestAnimationFrame(step);
    }
    el.textContent = '0' + tail;
    requestAnimationFrame(step);
  }
  function show(el) {
    el.classList.add('on');
    if (!calm && el.classList.contains('perf')) Array.prototype.forEach.call(el.querySelectorAll('.c b'), countUp);
  }
  var blocks = document.querySelectorAll('.rv');
  if (calm || !('IntersectionObserver' in window)) {
    Array.prototype.forEach.call(blocks, function (el) { el.classList.add('on'); });
  } else {
    var seen = new IntersectionObserver(function (entries) {
      entries.forEach(function (en) {
        if (en.isIntersecting) { show(en.target); seen.unobserve(en.target); }
      });
    }, { rootMargin: '0px 0px -8% 0px', threshold: 0.12 });
    Array.prototype.forEach.call(blocks, function (el) { seen.observe(el); });
  }

  // ---- the forest's fireflies: a few warm motes drifting behind the page, as in the launcher's backdrop
  var cv = document.getElementById('motes');
  if (calm || !cv || !cv.getContext) return;
  var ctx = cv.getContext('2d'), W = 0, H = 0, dpr = 1, motes = [], last = 0, running = false;
  var sprite = document.createElement('canvas');
  (function () {
    var s = sprite.width = sprite.height = 64, c = sprite.getContext('2d');
    var g = c.createRadialGradient(s / 2, s / 2, 0, s / 2, s / 2, s / 2);
    g.addColorStop(0, 'rgba(255,244,190,1)');
    g.addColorStop(0.18, 'rgba(255,226,140,.75)');
    g.addColorStop(0.5, 'rgba(230,190,90,.16)');
    g.addColorStop(1, 'rgba(230,190,90,0)');
    c.fillStyle = g;
    c.fillRect(0, 0, s, s);
  })();
  function rnd(a, b) { return a + Math.random() * (b - a); }
  function make(anywhere) {
    return { x: rnd(0, W), y: anywhere ? rnd(0, H) : H + 20, r: rnd(5, 13), vx: rnd(-6, 6), vy: rnd(-14, -4),
             sway: rnd(0.3, 0.9), ph: rnd(0, 6.28), blink: rnd(0.4, 1.1), a: rnd(0.35, 0.8) };
  }
  function size() {
    dpr = Math.min(window.devicePixelRatio || 1, 1.5);
    W = window.innerWidth; H = window.innerHeight;
    cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr);
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    var want = Math.max(10, Math.min(26, Math.round(W * H / 60000)));
    while (motes.length < want) motes.push(make(true));
    motes.length = want;
  }
  function frame(t) {
    if (!running) return;
    var dt = Math.min(0.05, (t - last) / 1000 || 0);
    last = t;
    ctx.clearRect(0, 0, W, H);
    for (var i = 0; i < motes.length; i++) {
      var m = motes[i];
      m.ph += dt * m.sway;
      m.x += (m.vx + Math.sin(m.ph) * 9) * dt;
      m.y += m.vy * dt;
      if (m.y < -20 || m.x < -30 || m.x > W + 30) { motes[i] = m = make(false); }
      ctx.globalAlpha = m.a * (0.55 + 0.45 * Math.sin(t / 1000 * m.blink + m.ph * 3));
      ctx.drawImage(sprite, m.x - m.r, m.y - m.r, m.r * 2, m.r * 2);
    }
    requestAnimationFrame(frame);
  }
  function run(on) {
    if (on === running) return;
    running = on;
    if (on) { last = 0; requestAnimationFrame(frame); }
  }
  size();
  window.addEventListener('resize', size);
  document.addEventListener('visibilitychange', function () { run(!document.hidden); });
  run(!document.hidden);
})();
