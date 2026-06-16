(function () {
  "use strict";
  var C = {
    themes: ["light", "dark", "black"],
    themeLbl: { light: "浅色", dark: "深色", black: "纯黑" },
    pals: ["amber", "glass", "teal", "champagne"],
    palLbl: { amber: "暖琥珀", glass: "紫雾玻璃", teal: "青绿编辑", champagne: "香槟金" },
    panels: ["dashboard", "history", "chat", "policy", "album", "print"],
    titles: { dashboard: "总览", history: "搜题记录", chat: "亲子聊天", policy: "管控策略", album: "相册传图", print: "远程打印" }
  };
  function $(s, r) { return (r || document).querySelector(s); }
  function $$(s, r) { return Array.from((r || document).querySelectorAll(s)); }
  function vt(fn) { return document.startViewTransition ? document.startViewTransition(fn) : (fn(), null); }

  function init() {
    try {
      var t = localStorage.getItem("parent_theme"), p = localStorage.getItem("parent_palette");
      if (C.themes.indexOf(t) >= 0) document.documentElement.dataset.theme = t;
      else document.documentElement.dataset.theme = matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
      if (C.pals.indexOf(p) >= 0) document.documentElement.dataset.palette = p;
      else document.documentElement.dataset.palette = "glass";
    } catch (e) { document.documentElement.dataset.palette = "glass"; }
  }
  function theme() { var t = document.documentElement.dataset.theme; return C.themes.indexOf(t) >= 0 ? t : "light"; }
  function pal() { var p = document.documentElement.dataset.palette; return C.pals.indexOf(p) >= 0 ? p : "glass"; }
  function syncBtns() {
    var tb = "主题：" + C.themeLbl[theme()] + "（点击切换）";
    $$("#btn-theme,#btn-theme-inner").forEach(function (b) { if (b) b.title = tb; });
    var pb = $("#btn-palette");
    if (pb) { pb.textContent = C.palLbl[pal()]; pb.title = "配色：" + C.palLbl[pal()] + "（点击切换）"; }
  }
  function cycleTheme() {
    document.documentElement.dataset.theme = C.themes[(C.themes.indexOf(theme()) + 1) % C.themes.length];
    try { localStorage.setItem("parent_theme", document.documentElement.dataset.theme); } catch (e) {}
    syncBtns();
  }
  function cyclePal() {
    document.documentElement.dataset.palette = C.pals[(C.pals.indexOf(pal()) + 1) % C.pals.length];
    try { localStorage.setItem("parent_palette", document.documentElement.dataset.palette); } catch (e) {}
    syncBtns();
  }
  init(); syncBtns();
  $$("#btn-theme,#btn-theme-inner").forEach(function (b) { b.addEventListener("click", cycleTheme); });
  var bp = $("#btn-palette"); if (bp) bp.addEventListener("click", cyclePal);

  var overlay = $("#nav-overlay"), menuBtn = $("#menu-btn");
  function closeMenu() {
    if (!overlay) return;
    overlay.classList.remove("is-open"); overlay.hidden = true; overlay.setAttribute("aria-hidden", "true");
    if (menuBtn) { menuBtn.classList.remove("is-open"); menuBtn.setAttribute("aria-expanded", "false"); }
  }
  function openMenu() {
    if (!overlay) return;
    overlay.hidden = false; overlay.setAttribute("aria-hidden", "false");
    requestAnimationFrame(function () { overlay.classList.add("is-open"); });
    if (menuBtn) { menuBtn.classList.add("is-open"); menuBtn.setAttribute("aria-expanded", "true"); }
  }
  if (menuBtn) menuBtn.addEventListener("click", function () { overlay.classList.contains("is-open") ? closeMenu() : openMenu(); });
  if (overlay) overlay.addEventListener("click", function (e) { if (e.target === overlay) closeMenu(); });

  var drawer = $("#drawer");
  function openDrawer() { if (!drawer) return; drawer.hidden = false; requestAnimationFrame(function () { drawer.classList.add("is-open"); }); }
  function closeDrawer() { if (!drawer) return; drawer.classList.remove("is-open"); drawer.hidden = true; }
  $$("[data-open-drawer]").forEach(function (el) { el.addEventListener("click", openDrawer); });
  var dc = $("#drawer-close"), db = $("#drawer-backdrop");
  if (dc) dc.addEventListener("click", closeDrawer);
  if (db) db.addEventListener("click", closeDrawer);

  function animCounts() {
    if (matchMedia("(prefers-reduced-motion: reduce)").matches) return;
    $$("[data-count]").forEach(function (el) {
      var to = +el.dataset.count, t0 = performance.now();
      function step(now) {
        var p = Math.min(1, (now - t0) / 900);
        el.textContent = Math.round(to * (1 - Math.pow(1 - p, 3)));
        if (p < 1) requestAnimationFrame(step);
      }
      requestAnimationFrame(step);
    });
  }

  var gsapOk = typeof gsap !== "undefined";
  function revealPanel(panel) { if (panel) $$(".gsap-hide", panel).forEach(function (el) { el.classList.remove("gsap-hide"); }); }
  function runPanelMotion(name) {
    var panel = $("#panel-" + name);
    if (!panel) return;
    if (!gsapOk || matchMedia("(prefers-reduced-motion: reduce)").matches) { revealPanel(panel); return; }
    var items = panel.querySelectorAll(".shell, .history-row, .form-stack");
    gsap.fromTo(items, { opacity: 0, y: 28 }, { opacity: 1, y: 0, duration: 0.65, stagger: 0.05, ease: "power3.out", onComplete: function () { revealPanel(panel); } });
  }

  function showPanel(name) {
    if (C.panels.indexOf(name) < 0) return;
    $$("#preview-nav button").forEach(function (b) { b.classList.toggle("is-on", b.dataset.scene === name); });
    C.panels.forEach(function (p) {
      var el = document.getElementById("panel-" + p);
      if (!el) return;
      el.classList.toggle("is-on", p === name);
      el.hidden = p !== name;
    });
    var t = $("#page-title"); if (t) t.textContent = C.titles[name] || name;
    $$(".scene-btn").forEach(function (b) { if (b.dataset.scene !== "login") b.classList.toggle("is-on", b.dataset.scene === name); });
    runPanelMotion(name);
    if (name === "dashboard") animCounts();
  }

  function showScene(name) {
    if (!name) return;
    var run = function () {
      closeMenu();
      $$(".scene-btn").forEach(function (b) { b.classList.toggle("is-on", b.dataset.scene === name); });
      if (name === "login") {
        $("#scene-login").classList.add("is-on");
        $("#scene-app").classList.remove("is-on");
        closeDrawer();
        return;
      }
      $("#scene-login").classList.remove("is-on");
      $("#scene-app").classList.add("is-on");
      showPanel(name);
    };
    vt(run);
  }

  $$(".scene-btn,[data-scene]").forEach(function (b) {
    if (!b.dataset.scene || b.closest("#preview-nav")) return;
    if (b.closest(".nav-overlay__links")) return;
    b.addEventListener("click", function () { showScene(b.dataset.scene); });
  });
  $$("#preview-nav button").forEach(function (b) { b.addEventListener("click", function () { showScene(b.dataset.scene); }); });
  $$("[data-goto]").forEach(function (b) { b.addEventListener("click", function () { showScene(b.dataset.goto); }); });
  $$(".day-chip[data-day]").forEach(function (btn) { btn.addEventListener("click", function () { btn.classList.toggle("day-chip--on"); btn.classList.toggle("on"); }); });
  document.addEventListener("keydown", function (e) { if (e.key === "Escape") { closeDrawer(); closeMenu(); } });

  if (!matchMedia("(prefers-reduced-motion: reduce)").matches) {
    $$(".spot").forEach(function (el) {
      el.addEventListener("mousemove", function (e) {
        var r = el.getBoundingClientRect();
        el.style.setProperty("--spot-x", ((e.clientX - r.left) / r.width * 100).toFixed(1) + "%");
        el.style.setProperty("--spot-y", ((e.clientY - r.top) / r.height * 100).toFixed(1) + "%");
      });
    });
    if (gsapOk) gsap.from(".login-copy, .login-form-wrap", { opacity: 0, y: 36, duration: 0.8, stagger: 0.08, ease: "power3.out", delay: 0.08 });
    else $$(".gsap-hide").forEach(function (el) { el.classList.remove("gsap-hide"); });
  } else $$(".gsap-hide").forEach(function (el) { el.classList.remove("gsap-hide"); });
})();
