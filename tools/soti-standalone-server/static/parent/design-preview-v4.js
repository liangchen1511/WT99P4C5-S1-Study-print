(function () {
  "use strict";
  var C = {
    themes: ["light", "dark", "black"],
    labels: { light: "浅色", dark: "深色", black: "纯黑" },
    panels: ["dashboard", "history", "chat", "policy", "album", "print"],
    titles: { dashboard: "总览", history: "搜题记录", chat: "亲子聊天", policy: "管控策略", album: "相册传图", print: "远程打印" }
  };
  function $(s, r) { return (r || document).querySelector(s); }
  function $$(s, r) { return Array.from((r || document).querySelectorAll(s)); }

  function initTheme() {
    var saved = null;
    try { saved = localStorage.getItem("parent_theme"); } catch (e) {}
    if (C.themes.indexOf(saved) >= 0) document.documentElement.dataset.theme = saved;
    else if (!document.documentElement.dataset.theme) {
      document.documentElement.dataset.theme = matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
    }
  }
  function theme() {
    var t = document.documentElement.dataset.theme;
    return C.themes.indexOf(t) >= 0 ? t : "light";
  }
  function setThemeBtn() {
    var l = "主题：" + C.labels[theme()] + "（点击切换）";
    $$("#btn-theme,#btn-theme-app").forEach(function (b) { if (b) b.title = l; });
  }
  function cycleTheme() {
    document.documentElement.dataset.theme = C.themes[(C.themes.indexOf(theme()) + 1) % C.themes.length];
    try { localStorage.setItem("parent_theme", document.documentElement.dataset.theme); } catch (e) {}
    setThemeBtn();
  }
  initTheme();
  setThemeBtn();
  $$("#btn-theme,#btn-theme-app").forEach(function (b) { b.addEventListener("click", cycleTheme); });

  var drawer = $("#drawer");
  function openDrawer() { drawer.hidden = false; requestAnimationFrame(function () { drawer.classList.add("open"); }); }
  function closeDrawer() { drawer.classList.remove("open"); drawer.hidden = true; }
  $$("[data-open-drawer]").forEach(function (el) { el.addEventListener("click", openDrawer); });
  $("#drawer-close").addEventListener("click", closeDrawer);
  $(".drawer__bg").addEventListener("click", closeDrawer);

  function revealPanel(panel) {
    if (!panel) return;
    $$(".gsap-hide", panel).forEach(function (el) { el.classList.remove("gsap-hide"); });
  }

  function showPanel(name) {
    if (C.panels.indexOf(name) < 0) return;
    $$(".dock button").forEach(function (b) { b.classList.toggle("is-on", b.dataset.scene === name); });
    C.panels.forEach(function (p) {
      var el = document.getElementById("panel-" + p);
      if (!el) return;
      el.classList.toggle("is-on", p === name);
      el.hidden = p !== name;
    });
    $("#page-title").textContent = C.titles[name] || name;
    $$(".scene-btn").forEach(function (b) { b.classList.toggle("is-on", b.dataset.scene === name); });
    runPanelMotion(name);
  }

  function showScene(name) {
    if (!name) return;
    if (name === "login") {
      $$(".scene-btn").forEach(function (b) { b.classList.toggle("is-on", b.dataset.scene === "login"); });
      $("#scene-login").classList.add("is-on");
      $("#scene-app").classList.remove("is-on");
      var dock = $(".dock");
      if (dock) dock.hidden = true;
      closeDrawer();
      return;
    }
    $$(".scene-btn").forEach(function (b) { b.classList.toggle("is-on", b.dataset.scene === name); });
    $("#scene-login").classList.remove("is-on");
    $("#scene-app").classList.add("is-on");
    var dock = $(".dock");
    if (dock) dock.hidden = false;
    showPanel(name);
  }

  $$(".scene-btn,[data-scene]").forEach(function (b) {
    if (!b.dataset.scene || b.closest(".dock")) return;
    b.addEventListener("click", function () { showScene(b.dataset.scene); });
  });
  $$(".dock button").forEach(function (b) { b.addEventListener("click", function () { showScene(b.dataset.scene); }); });
  $$("[data-goto]").forEach(function (b) { b.addEventListener("click", function () { showScene(b.dataset.goto); }); });

  $$(".h-acc__slice").forEach(function (sl, i) {
    sl.addEventListener("click", function (e) {
      if (e.target.closest("[data-stop]")) return;
      $$(".h-acc__slice").forEach(function (x) { x.classList.remove("is-open"); });
      sl.classList.add("is-open");
    });
    if (i === 0) sl.classList.add("is-open");
  });
  $$("[data-stop]").forEach(function (el) { el.addEventListener("click", function (e) { e.stopPropagation(); }); });

  $$(".day[data-day]").forEach(function (btn) {
    btn.addEventListener("click", function () { btn.classList.toggle("on"); });
  });

  document.addEventListener("keydown", function (e) { if (e.key === "Escape") closeDrawer(); });

  var gsapOk = typeof gsap !== "undefined" && typeof ScrollTrigger !== "undefined";
  if (gsapOk) gsap.registerPlugin(ScrollTrigger);
  else $$(".gsap-hide").forEach(function (el) { el.classList.remove("gsap-hide"); });

  function runPanelMotion(name) {
    var panel = $("#panel-" + name);
    if (!panel) return;
    if (!gsapOk || matchMedia("(prefers-reduced-motion: reduce)").matches) {
      revealPanel(panel);
      return;
    }
    var items = panel.querySelectorAll(".tile, .form-card, .chat-box, .h-item");
    gsap.fromTo(items, { opacity: 0, y: 28 }, {
      opacity: 1, y: 0, duration: 0.65, stagger: 0.05, ease: "power3.out",
      onComplete: function () { revealPanel(panel); }
    });
    if (name === "history") bindHistoryScroll();
  }

  function bindHistoryScroll() {
    $$("#panel-history .h-item").forEach(function (item) {
      if (item.dataset.st) return;
      item.dataset.st = "1";
      var img = $(".h-item__img", item);
      if (!img) return;
      ScrollTrigger.create({
        trigger: item,
        start: "top 88%",
        end: "bottom 12%",
        onEnter: function () { item.classList.add("is-lit"); gsap.to(img, { scale: 1, opacity: 1, duration: 0.5, ease: "power2.out" }); },
        onLeave: function () { item.classList.remove("is-lit"); gsap.to(img, { scale: 0.9, opacity: 0.5, duration: 0.4 }); },
        onEnterBack: function () { item.classList.add("is-lit"); gsap.to(img, { scale: 1, opacity: 1, duration: 0.4 }); }
      });
    });
  }

  if (gsapOk && !matchMedia("(prefers-reduced-motion: reduce)").matches) {
    gsap.from(".login-edit__copy > *, .login-edit__visual", { opacity: 0, y: 36, duration: 0.8, stagger: 0.08, ease: "power3.out", delay: 0.08 });
  }
})();
