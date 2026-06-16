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
    var s = null;
    try { s = localStorage.getItem("parent_theme"); } catch (e) {}
    if (C.themes.indexOf(s) >= 0) document.documentElement.dataset.theme = s;
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
  function openDrawer() { if (!drawer) return; drawer.hidden = false; requestAnimationFrame(function () { drawer.classList.add("open"); }); }
  function closeDrawer() { if (!drawer) return; drawer.classList.remove("open"); drawer.hidden = true; }
  $$("[data-open-drawer]").forEach(function (el) { el.addEventListener("click", openDrawer); });
  var dc = $("#drawer-close"), db = $(".drawer__bg");
  if (dc) dc.addEventListener("click", closeDrawer);
  if (db) db.addEventListener("click", closeDrawer);

  function navOn(name) {
    $$(".scene-btn,.pv-nav button,.dock button").forEach(function (b) {
      if (!b.dataset.scene || b.dataset.scene === "login") return;
      b.classList.toggle("is-on", b.dataset.scene === name);
    });
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
    $$(".reveal-on", $("#panel-" + name)).forEach(function (el) { el.classList.add("is-in"); });
  }

  function showScene(name) {
    if (!name) return;
    if (name === "login") {
      $$(".scene-btn").forEach(function (b) { b.classList.toggle("is-on", b.dataset.scene === "login"); });
      var sl = $("#scene-login"), sa = $("#scene-app");
      if (sl) sl.classList.add("is-on");
      if (sa) sa.classList.remove("is-on");
      var dock = $(".dock,.pv-dock");
      if (dock) dock.hidden = true;
      closeDrawer();
      return;
    }
    $$(".scene-btn").forEach(function (b) { b.classList.toggle("is-on", b.dataset.scene === name); });
    var sl2 = $("#scene-login"), sa2 = $("#scene-app");
    if (sl2) sl2.classList.remove("is-on");
    if (sa2) sa2.classList.add("is-on");
    var dock2 = $(".dock,.pv-dock");
    if (dock2) dock2.hidden = false;
    showPanel(name);
  }

  $$(".scene-btn,[data-scene]").forEach(function (b) {
    if (!b.dataset.scene || b.closest(".dock,.pv-dock,.pv-nav")) return;
    b.addEventListener("click", function () { showScene(b.dataset.scene); });
  });
  $$(".dock button,.pv-dock button,.pv-nav button").forEach(function (b) {
    b.addEventListener("click", function () { showScene(b.dataset.scene); });
  });
  $$("[data-goto]").forEach(function (b) { b.addEventListener("click", function () { showScene(b.dataset.goto); }); });
  $$(".day[data-day]").forEach(function (btn) { btn.addEventListener("click", function () { btn.classList.toggle("on"); }); });
  document.addEventListener("keydown", function (e) { if (e.key === "Escape") closeDrawer(); });

  if (!matchMedia("(prefers-reduced-motion: reduce)").matches) {
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) { if (e.isIntersecting) { e.target.classList.add("is-in"); io.unobserve(e.target); } });
    }, { threshold: 0.06 });
    $$(".reveal-on").forEach(function (el) { io.observe(el); });
  } else $$(".reveal-on").forEach(function (el) { el.classList.add("is-in"); });
})();
