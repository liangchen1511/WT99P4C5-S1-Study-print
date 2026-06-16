(function () {
  "use strict";
  var C = {
    themes: ["light", "dark", "black"],
    labels: { light: "浅色", dark: "深色", black: "纯黑" },
    panels: ["dashboard", "history", "chat", "policy", "album", "print"],
    titles: { dashboard: "总览", history: "搜题记录", chat: "亲子聊天", policy: "管控策略", album: "相册传图", print: "远程打印" },
    counts: { today: 11, week: 43 },
    magR: 28, magS: .22
  };
  function $(s, r) { return (r || document).querySelector(s); }
  function $$(s, r) { return Array.from((r || document).querySelectorAll(s)); }
  function vt(fn) { return document.startViewTransition ? document.startViewTransition(fn) : (fn(), null); }
  function initTheme() {
    var s = null;
    try { s = localStorage.getItem("parent_theme"); } catch (e) {}
    if (C.themes.indexOf(s) >= 0) document.documentElement.dataset.theme = s;
    else document.documentElement.dataset.theme = matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
  }
  function theme() { var t = document.documentElement.dataset.theme; return C.themes.indexOf(t) >= 0 ? t : "light"; }
  function setThemeBtn() {
    var l = "主题：" + C.labels[theme()] + "（点击切换）";
    $$("#btn-theme,#btn-theme-app").forEach(function (b) { if (b) b.title = l; });
  }
  function cycleTheme() {
    document.documentElement.dataset.theme = C.themes[(C.themes.indexOf(theme()) + 1) % C.themes.length];
    try { localStorage.setItem("parent_theme", document.documentElement.dataset.theme); } catch (e) {}
    setThemeBtn();
  }
  initTheme(); setThemeBtn();
  $$("#btn-theme,#btn-theme-app").forEach(function (b) { b.addEventListener("click", cycleTheme); });

  var drawer = $("#drawer");
  function openDrawer() { if (!drawer) return; drawer.hidden = false; requestAnimationFrame(function () { drawer.classList.add("open"); }); }
  function closeDrawer() { if (!drawer) return; drawer.classList.remove("open"); drawer.hidden = true; }
  $$("[data-open-drawer]").forEach(function (el) { el.addEventListener("click", openDrawer); });
  var dc = $("#drawer-close"), db = $(".drawer__bg");
  if (dc) dc.addEventListener("click", closeDrawer);
  if (db) db.addEventListener("click", closeDrawer);

  function moveDockPill() {
    var dock = $(".dock"), pill = $(".dock__pill"), on = $(".dock button.is-on");
    if (!pill || !on || !dock) return;
    pill.style.width = on.offsetWidth + "px";
    pill.style.left = on.offsetLeft + "px";
  }

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

  function navOn(name) {
    $$(".scene-btn,.dock button").forEach(function (b) {
      if (!b.dataset.scene || b.dataset.scene === "login") return;
      b.classList.toggle("is-on", b.dataset.scene === name);
    });
    moveDockPill();
  }

  function showPanel(name) {
    if (C.panels.indexOf(name) < 0) return;
    navOn(name);
    C.panels.forEach(function (p) {
      var el = document.getElementById("panel-" + p);
      if (!el) return;
      el.classList.toggle("is-on", p === name);
      el.hidden = p !== name;
    });
    var t = $("#page-title");
    if (t) t.textContent = C.titles[name] || name;
    $$(".scene-btn").forEach(function (b) { if (b.dataset.scene !== "login") b.classList.toggle("is-on", b.dataset.scene === name); });
    if (name === "dashboard") animCounts();
  }

  function showScene(name) {
    if (!name) return;
    var run = function () {
      if (name === "login") {
        $$(".scene-btn").forEach(function (b) { b.classList.toggle("is-on", b.dataset.scene === "login"); });
        $("#scene-login").classList.add("is-on");
        $("#scene-app").classList.remove("is-on");
        var d = $(".dock"); if (d) d.hidden = true;
        closeDrawer();
        return;
      }
      $$(".scene-btn").forEach(function (b) { b.classList.toggle("is-on", b.dataset.scene === name); });
      $("#scene-login").classList.remove("is-on");
      $("#scene-app").classList.add("is-on");
      var d2 = $(".dock"); if (d2) d2.hidden = false;
      showPanel(name);
      moveDockPill();
    };
    vt(run);
  }

  $$(".scene-btn,[data-scene]").forEach(function (b) {
    if (!b.dataset.scene || b.closest(".dock")) return;
    b.addEventListener("click", function () { showScene(b.dataset.scene); });
  });
  $$(".dock button").forEach(function (b) { b.addEventListener("click", function () { showScene(b.dataset.scene); }); });
  $$("[data-goto]").forEach(function (b) { b.addEventListener("click", function () { showScene(b.dataset.goto); }); });
  $$(".day[data-day]").forEach(function (btn) { btn.addEventListener("click", function () { btn.classList.toggle("on"); }); });
  document.addEventListener("keydown", function (e) { if (e.key === "Escape") closeDrawer(); });

  if (!matchMedia("(prefers-reduced-motion: reduce)").matches) {
    $$(".spot").forEach(function (el) {
      el.addEventListener("mousemove", function (e) {
        var r = el.getBoundingClientRect();
        el.style.setProperty("--spot-x", ((e.clientX - r.left) / r.width * 100).toFixed(1) + "%");
        el.style.setProperty("--spot-y", ((e.clientY - r.top) / r.height * 100).toFixed(1) + "%");
      });
    });
    $$(".mag").forEach(function (btn) {
      btn.addEventListener("mousemove", function (e) {
        var r = btn.getBoundingClientRect(), cx = r.left + r.width / 2, cy = r.top + r.height / 2;
        var dx = (e.clientX - cx) / C.magR, dy = (e.clientY - cy) / C.magR;
        btn.style.transform = "translate(" + (dx * C.magS * 10) + "px," + (dy * C.magS * 10) + "px)";
      });
      btn.addEventListener("mouseleave", function () { btn.style.transform = ""; });
    });
  }

  var login = $(".login-v5");
  if (login) requestAnimationFrame(function () { login.classList.add("is-in"); });
  moveDockPill();
  window.addEventListener("resize", moveDockPill);
})();
