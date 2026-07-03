(function () {
  "use strict";

  const STORAGE_TOKEN = "parent_token";
  const STORAGE_DEVICE = "parent_device";
  const STORAGE_PASSWORD = "parent_password";
  const STORAGE_THEME = "parent_theme";
  const STORAGE_PALETTE = "parent_palette";
  const THEMES = ["light", "dark", "black"];
  const THEME_LABELS = { light: "浅色", dark: "深色", black: "纯黑" };
  const PALETTES = ["amber", "glass", "teal", "champagne"];
  const PALETTE_LABELS = { amber: "暖琥珀", glass: "紫雾玻璃", teal: "青绿编辑", champagne: "香槟金" };
  const MODE_LABELS = {
    solve: "解题",
    translate: "翻译",
    tutor: "讲解",
    grade: "批改",
    summary: "摘要",
    daily: "每日一句",
  };

  const APP_LABELS = {
    game_2048: "2048 游戏",
    video: "视频",
    music: "音乐",
    camera: "相机",
    print: "打印",
  };

  /** 学习时段：allow * + deny 勾选 = 仅禁止勾选项（不用白名单） */
  const STUDY_DENY_DEFAULT = ["game_2048", "video", "music"];
  const LOCK_APPS = ["game_2048", "video", "music", "camera", "print"];

  const state = {
    token: localStorage.getItem(STORAGE_TOKEN) || "",
    deviceId: localStorage.getItem(STORAGE_DEVICE) || "WT99-DEMO-01",
    policy: null,
    history: [],
    chatLastId: 0,
    chatPollTimer: null,
    chatActive: false,
  };

  const $ = (sel) => document.querySelector(sel);

  function getEffectiveTheme() {
    const cur = document.documentElement.dataset.theme;
    if (THEMES.indexOf(cur) >= 0) return cur;
    return window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
  }

  function updateThemeBtn() {
    const btn = $("#btn-theme");
    if (!btn) return;
    btn.title = "主题：" + THEME_LABELS[getEffectiveTheme()] + "（点击切换）";
  }

  function toggleTheme() {
    const cur = getEffectiveTheme();
    const next = THEMES[(THEMES.indexOf(cur) + 1) % THEMES.length];
    document.documentElement.dataset.theme = next;
    localStorage.setItem(STORAGE_THEME, next);
    updateThemeBtn();
  }

  function initTheme() {
    const saved = localStorage.getItem(STORAGE_THEME);
    if (THEMES.indexOf(saved) >= 0) {
      document.documentElement.dataset.theme = saved;
    } else if (!document.documentElement.dataset.theme) {
      document.documentElement.dataset.theme = window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
    }
    updateThemeBtn();
  }

  function getPalette() {
    const p = document.documentElement.dataset.palette;
    return PALETTES.indexOf(p) >= 0 ? p : "glass";
  }

  function updatePaletteBtn() {
    const btn = $("#btn-palette");
    if (!btn) return;
    btn.textContent = PALETTE_LABELS[getPalette()];
    btn.title = "配色：" + PALETTE_LABELS[getPalette()] + "（点击切换）";
  }

  function initPalette() {
    try {
      const saved = localStorage.getItem(STORAGE_PALETTE);
      if (PALETTES.indexOf(saved) >= 0) document.documentElement.dataset.palette = saved;
      else if (!document.documentElement.dataset.palette) document.documentElement.dataset.palette = "glass";
    } catch (e) {
      document.documentElement.dataset.palette = "glass";
    }
    updatePaletteBtn();
  }

  function cyclePalette() {
    document.documentElement.dataset.palette = PALETTES[(PALETTES.indexOf(getPalette()) + 1) % PALETTES.length];
    localStorage.setItem(STORAGE_PALETTE, document.documentElement.dataset.palette);
    updatePaletteBtn();
  }

  function vt(fn) {
    if (document.startViewTransition) document.startViewTransition(fn);
    else fn();
  }

  function animStatEl(el, to) {
    if (!el || to == null || matchMedia("(prefers-reduced-motion: reduce)").matches) {
      if (el && to != null) el.textContent = String(to);
      return;
    }
    const n = parseInt(to, 10);
    if (isNaN(n)) { el.textContent = String(to); return; }
    const t0 = performance.now();
    function step(now) {
      const p = Math.min(1, (now - t0) / 900);
      el.textContent = String(Math.round(n * (1 - Math.pow(1 - p, 3))));
      if (p < 1) requestAnimationFrame(step);
    }
    requestAnimationFrame(step);
  }

  const gsapOk = typeof gsap !== "undefined";

  function runPanelMotion(name) {
    const panel = $("#panel-" + name);
    if (!panel || !gsapOk || matchMedia("(prefers-reduced-motion: reduce)").matches) return;
    const items = panel.querySelectorAll(".shell, .history-item, .form-stack");
    if (!items.length) return;
    gsap.fromTo(items, { opacity: 0, y: 28 }, { opacity: 1, y: 0, duration: 0.65, stagger: 0.05, ease: "power3.out" });
  }

  function bindSpotlight() {
    if (matchMedia("(prefers-reduced-motion: reduce)").matches) return;
    document.querySelectorAll(".spot").forEach((el) => {
      el.addEventListener("mousemove", (e) => {
        const r = el.getBoundingClientRect();
        el.style.setProperty("--spot-x", ((e.clientX - r.left) / r.width * 100).toFixed(1) + "%");
        el.style.setProperty("--spot-y", ((e.clientY - r.top) / r.height * 100).toFixed(1) + "%");
      });
    });
  }

  const overlay = () => $("#nav-overlay");
  const menuBtn = () => $("#menu-btn");

  function closeMenu() {
    const o = overlay(), m = menuBtn();
    if (!o) return;
    o.classList.remove("is-open");
    o.hidden = true;
    o.setAttribute("aria-hidden", "true");
    if (m) { m.classList.remove("is-open"); m.setAttribute("aria-expanded", "false"); }
  }

  function openMenu() {
    const o = overlay(), m = menuBtn();
    if (!o) return;
    o.hidden = false;
    o.setAttribute("aria-hidden", "false");
    requestAnimationFrame(() => o.classList.add("is-open"));
    if (m) { m.classList.add("is-open"); m.setAttribute("aria-expanded", "true"); }
  }

  initTheme();
  initPalette();
  bindSpotlight();
  const themeBtn = $("#btn-theme");
  if (themeBtn) themeBtn.addEventListener("click", toggleTheme);
  const palBtn = $("#btn-palette");
  if (palBtn) palBtn.addEventListener("click", cyclePalette);
  const mb = menuBtn();
  if (mb) mb.addEventListener("click", () => { overlay() && overlay().classList.contains("is-open") ? closeMenu() : openMenu(); });
  if (overlay()) overlay().addEventListener("click", (e) => { if (e.target === overlay()) closeMenu(); });

  function onSessionExpired() {
    showLogin();
    const err = $("#login-error");
    err.textContent = "登录已过期（服务端重启后需重新登录），请重新输入密码";
    err.hidden = false;
  }

  async function parseJsonResponse(r) {
    return r.json().catch(() => ({}));
  }

  function authHeaders(extra) {
    const headers = { ...(extra || {}) };
    if (state.token) {
      headers.Authorization = "Bearer " + state.token;
    }
    return headers;
  }

  /** JSON API；401 时退回登录页 */
  function api(path, opts = {}) {
    const headers = { "Content-Type": "application/json", ...(opts.headers || {}) };
    if (state.token) {
      headers.Authorization = "Bearer " + state.token;
    }
    return fetch(path, { ...opts, headers }).then(async (r) => {
      const data = await parseJsonResponse(r);
      if (r.status === 401) {
        onSessionExpired();
        throw new Error("session_expired");
      }
      if (!r.ok) {
        throw new Error(data.error || r.statusText);
      }
      return data;
    });
  }

  /** multipart 上传等；401 时退回登录页 */
  async function parentUpload(path, body, extraHeaders) {
    const r = await fetch(path, {
      method: "POST",
      headers: authHeaders(extraHeaders),
      body,
    });
    const data = await parseJsonResponse(r);
    if (r.status === 401) {
      onSessionExpired();
      throw new Error("session_expired");
    }
    if (!r.ok) {
      throw new Error(data.error || r.statusText);
    }
    return data;
  }

  function formatTime(ts) {
    const d = new Date(ts * 1000);
    const now = new Date();
    const sameDay =
      d.getFullYear() === now.getFullYear() &&
      d.getMonth() === now.getMonth() &&
      d.getDate() === now.getDate();
    const hm = d.toLocaleTimeString("zh-CN", { hour: "2-digit", minute: "2-digit" });
    if (sameDay) return "今天 " + hm;
    return d.toLocaleDateString("zh-CN") + " " + hm;
  }

  function modeLabel(m) {
    return MODE_LABELS[m] || m;
  }

  function renderHistoryList(el, items, limit) {
    el.innerHTML = "";
    const list = limit ? items.slice(0, limit) : items;
    if (!list.length) {
      el.innerHTML = '<li class="muted" style="padding:16px">暂无记录</li>';
      return;
    }
    list.forEach((item) => {
      const li = document.createElement("li");
      li.className = "history-item";
      li.dataset.id = item.id;
      const thumb = item.thumb_url
        ? `<img class="history-thumb" src="${item.thumb_url}" alt="" />`
        : '<div class="history-thumb history-thumb--empty">📷</div>';
      li.innerHTML = `
        ${thumb}
        <div class="history-body">
          <div class="history-meta">
            <span class="mode-badge">${modeLabel(item.mode)}</span>
            <span class="history-time">${formatTime(item.created_at)}</span>
          </div>
          <p class="history-preview">${escapeHtml(item.preview || item.answer_text || "")}</p>
        </div>
      `;
      li.addEventListener("click", () => openDrawer(item));
      el.appendChild(li);
    });
  }

  function escapeHtml(s) {
    const d = document.createElement("div");
    d.textContent = s;
    return d.innerHTML;
  }

  let drawerItem = null;

  function syncDrawerDownload(item) {
    drawerItem = item;
    const btn = $("#drawer-download");
    if (!btn) return;
    btn.hidden = !(item && item.thumb_url);
  }

  async function downloadDrawerPhoto() {
    if (!drawerItem || !drawerItem.thumb_url) return;
    const btn = $("#drawer-download");
    const url = drawerItem.thumb_url;
    const name = "搜题-" + (drawerItem.id || Date.now()) + ".jpg";
    btn.disabled = true;
    try {
      const r = await fetch(url, { headers: authHeaders() });
      if (!r.ok) throw new Error("下载失败");
      const blob = await r.blob();
      const a = document.createElement("a");
      a.href = URL.createObjectURL(blob);
      a.download = name;
      a.click();
      URL.revokeObjectURL(a.href);
    } catch (e) {
      alert(e.message || "下载失败");
    } finally {
      btn.disabled = false;
    }
  }

  function openDrawer(item) {
    const drawer = $("#drawer");
    const thumb = $("#drawer-thumb");
    const body = $("#drawer-body");
    body.textContent = item.answer_text || item.preview || "";
    if (item.thumb_url) {
      thumb.src = item.thumb_url;
      thumb.hidden = false;
    } else {
      thumb.hidden = true;
    }
    drawer.hidden = false;
    drawer.classList.add("drawer--open");
    syncDrawerDownload(item);
    if (item.id) {
      api("/parent/api/history/" + item.id)
        .then((res) => {
          if (res.item) {
            body.textContent = res.item.answer_text || "";
            if (res.item.thumb_url) {
              thumb.src = res.item.thumb_url;
              thumb.hidden = false;
            }
            syncDrawerDownload(Object.assign({}, item, res.item));
          }
        })
        .catch(() => {});
    }
  }

  function closeDrawer() {
    const drawer = $("#drawer");
    if (!drawer) return;
    drawerItem = null;
    const dl = $("#drawer-download");
    if (dl) dl.hidden = true;
    drawer.hidden = true;
    drawer.classList.remove("drawer--open");
  }

  function readTogglesFrom(container) {
    const toggles = {};
    Object.keys(APP_LABELS).forEach((key) => {
      const input = container.querySelector(`input[data-app="${key}"]`);
      toggles[key] = input ? input.checked : true;
    });
    return toggles;
  }

  function renderToggles(container, toggles, onChange) {
    container.innerHTML = "";
    Object.keys(APP_LABELS).forEach((key) => {
      const row = document.createElement("div");
      row.className = "toggle-row";
      const checked = toggles[key] !== false;
      row.innerHTML = `
        <span>${APP_LABELS[key]}</span>
        <label class="switch">
          <input type="checkbox" data-app="${key}" ${checked ? "checked" : ""} />
          <span></span>
        </label>
      `;
      row.querySelector("input").addEventListener("change", (e) => {
        toggles[key] = e.target.checked;
        if (onChange) onChange(key, e.target.checked);
      });
      container.appendChild(row);
    });
  }

  function findStudyRule(policy) {
    const rules = policy.rules || [];
    return (
      rules.find((r) => (r.deny_apps || []).includes("game_2048")) ||
      rules.find((r) => r.name && String(r.name).includes("学习")) ||
      rules[rules.length - 1]
    );
  }

  function hasWeekendFreeRule(policy) {
    return (policy.rules || []).some(
      (r) =>
        (r.days || []).includes(6) &&
        (r.days || []).includes(7) &&
        (r.allow_apps || []).includes("*")
    );
  }

  function updateStudyModeUI() {
    const active = !!(state.policy && state.policy.demo_force_study);
    const btnEnd = $("#btn-end-study");
    const btnForce = $("#btn-force-study");
    if (btnEnd) btnEnd.hidden = !active;
    if (btnForce) btnForce.disabled = active;
    const chip = $("#rule-chip");
    if (chip && active) {
      chip.textContent = "已强制学习时段（开关未改，点「结束学习时段」恢复）";
    }
  }

  function ruleSummary(policy) {
    if (policy.demo_force_study) {
      return "已强制学习时段";
    }
    const study = findStudyRule(policy);
    if (study && $("#study-enabled") && $("#study-enabled").checked) {
      return `${study.name || "学习时段"} · ${study.start}–${study.end}`;
    }
    return "未启用时段限制";
  }

  function syncStudyFormDisabled() {
    const on = $("#study-enabled").checked;
    $("#study-form").classList.toggle("is-disabled", !on);
  }

  function bindPolicyFormInteractions() {
    $("#study-enabled").addEventListener("change", syncStudyFormDisabled);

    $("#study-days").querySelectorAll(".day-chip").forEach((btn) => {
      btn.addEventListener("click", () => {
        btn.classList.toggle("day-chip--on");
      });
    });

    $("#study-deny-apps").querySelectorAll(".check-pill").forEach((label) => {
      const input = label.querySelector("input");
      const sync = () => label.classList.toggle("check-pill--on", input.checked);
      input.addEventListener("change", sync);
      sync();
    });

    syncStudyFormDisabled();
  }

  function fillPolicyForm(policy) {
    const study = findStudyRule(policy) || {};
    const days = study.days || [1, 2, 3, 4, 5];
    $("#study-enabled").checked = (policy.rules || []).length > 0;
    $("#study-start").value = study.start || "19:00";
    $("#study-end").value = study.end || "21:00";
    $("#weekend-free").checked = hasWeekendFreeRule(policy);

    $("#study-days").querySelectorAll(".day-chip").forEach((btn) => {
      const d = parseInt(btn.dataset.day, 10);
      btn.classList.toggle("day-chip--on", days.includes(d));
    });

    const deny = new Set(study.deny_apps || STUDY_DENY_DEFAULT);
    $("#study-deny-apps").querySelectorAll(".check-pill").forEach((label) => {
      const input = label.querySelector("input");
      input.checked = deny.has(input.value);
      label.classList.toggle("check-pill--on", input.checked);
    });

    syncStudyFormDisabled();
  }

  function collectPolicyFromForm() {
    const toggles = readTogglesFrom($("#policy-toggles"));
    const rules = [];

    if ($("#study-enabled").checked) {
      const days = [];
      $("#study-days").querySelectorAll(".day-chip--on").forEach((btn) => {
        days.push(parseInt(btn.dataset.day, 10));
      });
      days.sort((a, b) => a - b);

      const deny = [];
      $("#study-deny-apps").querySelectorAll("input:checked").forEach((inp) => {
        deny.push(inp.value);
      });

      if (days.length) {
        rules.push({
          name: "学习时段",
          days,
          start: $("#study-start").value || "19:00",
          end: $("#study-end").value || "21:00",
          allow_apps: ["*"],
          deny_apps: deny,
        });
      }
    }

    if ($("#weekend-free").checked) {
      rules.unshift({
        name: "周末自由",
        days: [6, 7],
        start: "00:00",
        end: "23:59",
        allow_apps: ["*"],
        deny_apps: [],
      });
    }

    return {
      version: 1,
      timezone: "Asia/Shanghai",
      demo_force_study: !!(state.policy && state.policy.demo_force_study),
      rules,
      device_lock_outside_rules: false,
      app_toggles: toggles,
    };
  }

  function applyDashboard(data) {
    const st = data.stats || {};
    animStatEl($("#stat-today"), st.today ?? "0");
    animStatEl($("#stat-week"), st.week ?? "0");
    $("#stat-policy").textContent = data.policy_name || "—";
    $("#sidebar-online").classList.toggle("dot--online", !!st.online);
    $("#sidebar-online-text").textContent = st.online ? "在线" : "离线";
    if ($("#sidebar-online")) $("#sidebar-online").title = st.online ? "在线" : "离线";

    if (data.stats) {
      const on = $("#chat-online");
      if (on) on.textContent = data.stats.online ? "设备在线" : "设备离线";
    }

    if (data.policy) {
      state.policy = data.policy;
      renderToggles($("#quick-toggles"), state.policy.app_toggles || {}, quickToggleChange);
      renderToggles($("#policy-toggles"), state.policy.app_toggles || {}, policyToggleChange);
      fillPolicyForm(state.policy);
      $("#rule-chip").textContent = ruleSummary(state.policy);
      updateStudyModeUI();
    }

    const hist = data.items || data.history || [];
    state.history = hist;
    renderHistoryList($("#recent-list"), hist, 5);
    renderHistoryList($("#history-list"), filterHistory(hist));
  }

  function filterHistory(items) {
    const mode = $("#filter-mode").value;
    if (!mode) return items;
    return items.filter((i) => i.mode === mode);
  }

  async function savePolicy(policy) {
    await api("/parent/api/policy", {
      method: "PUT",
      body: JSON.stringify({ policy }),
    });
    state.policy = policy;
    renderToggles($("#quick-toggles"), state.policy.app_toggles || {}, quickToggleChange);
    $("#rule-chip").textContent = ruleSummary(state.policy);
    $("#stat-policy").textContent = ruleSummary(state.policy);
    updateStudyModeUI();
  }

  function policyToggleChange(key, enabled) {
    if (!state.policy) return;
    state.policy.app_toggles = state.policy.app_toggles || {};
    state.policy.app_toggles[key] = enabled;
    if (enabled && LOCK_APPS.includes(key)) {
      state.policy.demo_force_study = false;
    }
    const quick = $("#quick-toggles");
    const inp = quick && quick.querySelector(`input[data-app="${key}"]`);
    if (inp) inp.checked = enabled;
    updateStudyModeUI();
  }

  async function quickToggleChange(key, enabled) {
    if (!state.policy) return;
    state.policy.app_toggles = state.policy.app_toggles || {};
    state.policy.app_toggles[key] = enabled;
    if (enabled && LOCK_APPS.includes(key)) {
      state.policy.demo_force_study = false;
    }
    const policyToggles = $("#policy-toggles");
    const inp = policyToggles.querySelector(`input[data-app="${key}"]`);
    if (inp) inp.checked = enabled;
    try {
      await savePolicy(state.policy);
    } catch (e) {
      alert("保存失败: " + e.message);
    }
  }

  async function loadLive() {
    const statsRes = await api("/parent/api/stats");
    const histRes = await api("/parent/api/history?limit=50");
    const polRes = await api("/parent/api/policy?device_id=" + encodeURIComponent(state.deviceId));
    applyDashboard({
      stats: statsRes.stats,
      policy_name: statsRes.policy_name,
      policy: polRes.policy,
      items: histRes.items,
    });
  }

  function showApp() {
    vt(() => {
      const login = $("#view-login");
      const shell = $("#view-app");
      login.classList.remove("view--active");
      login.hidden = true;
      shell.hidden = false;
      $("#sidebar-device").textContent = state.deviceId;
      $("#top-device-pill").textContent = state.deviceId;
    });
  }

  function showLogin() {
    closeMenu();
    closeDrawer();
    stopChatPanel();
    state.token = "";
    localStorage.removeItem(STORAGE_TOKEN);
    vt(() => {
      $("#view-app").hidden = true;
      const login = $("#view-login");
      login.hidden = false;
      login.classList.add("view--active");
    });
  }

  function switchView(name) {
    closeMenu();
    document.querySelectorAll(".nav-item").forEach((n) => {
      n.classList.toggle("nav-item--active", n.dataset.view === name);
    });
    document.querySelectorAll(".panel").forEach((p) => {
      p.classList.remove("panel--active");
      p.hidden = true;
    });
    const panel = $("#panel-" + name);
    panel.hidden = false;
    panel.classList.add("panel--active");
    const titles = {
      dashboard: "总览",
      history: "搜题记录",
      policy: "管控策略",
      chat: "亲子聊天",
      album: "相册传图",
      print: "远程打印",
    };
    $("#page-title").textContent = titles[name] || name;
    if (name === "chat") startChatPanel();
    else stopChatPanel();
    runPanelMotion(name);
  }

  function scrollChatToBottom() {
    const b = $("#chat-messages");
    if (!b) return;
    const s = () => { b.scrollTop = b.scrollHeight; };
    s();
    requestAnimationFrame(() => requestAnimationFrame(s));
  }

  function formatChatTime(ts) {
    const d = new Date(ts * 1000);
    return `${d.getMonth() + 1}月${d.getDate()}日 ${d.toLocaleTimeString("zh-CN", { hour: "2-digit", minute: "2-digit" })}`;
  }

  function renderChatMessages(items, replace) {
    const box = $("#chat-messages");
    if (replace) box.innerHTML = "";
    if (!items.length && replace) {
      box.innerHTML = '<p class="muted" style="padding:12px">还没有消息，发一句打个招呼吧</p>';
      return;
    }
    items.forEach((m) => {
      const row = document.createElement("div");
      const isParent = m.sender === "parent";
      row.className = "chat-row " + (isParent ? "chat-row--parent" : "chat-row--child");
      row.dataset.id = String(m.id);
      const bubble = document.createElement("div");
      bubble.className = "chat-bubble " + (isParent ? "chat-bubble--parent" : "chat-bubble--child");
      bubble.innerHTML =
        `<span class="chat-bubble-meta">${formatChatTime(m.created_at)}</span>` +
        escapeHtml(m.body || "");
      row.appendChild(bubble);
      box.appendChild(row);
    });
    scrollChatToBottom();
  }

  function updateChatBadge(unread) {
    const badge = $("#nav-chat-badge");
    const n = unread && unread.parent_unread ? unread.parent_unread : 0;
    if (badge) badge.hidden = n <= 0;
  }

  async function chatMarkRead(upToId) {
    if (!upToId) return;
    try {
      await api("/parent/api/chat/read", {
        method: "POST",
        body: JSON.stringify({ role: "parent", up_to_id: upToId }),
      });
    } catch (e) {
      /* ignore */
    }
  }

  async function loadChatMessages(initial) {
    const path = initial
      ? "/parent/api/chat/messages?limit=50"
      : "/parent/api/chat/poll?since=" + state.chatLastId + "&limit=50";
    const res = await api(path);
    const items = res.items || [];
    if (items.length) {
      const last = items[items.length - 1];
      if (last.id > state.chatLastId) state.chatLastId = last.id;
    }
    if (initial) {
      renderChatMessages(items, true);
    } else if (items.length) {
      const box = $("#chat-messages");
      const existing = new Set(
        Array.from(box.querySelectorAll(".chat-row")).map((el) => el.dataset.id)
      );
      const fresh = items.filter((m) => !existing.has(String(m.id)));
      if (fresh.length) renderChatMessages(fresh, false);
    }
    if (res.unread) updateChatBadge(res.unread);
    if (state.chatActive && state.chatLastId) {
      await chatMarkRead(state.chatLastId);
      updateChatBadge({ parent_unread: 0 });
    }
    return res;
  }

  async function chatPollTick() {
    if (!state.token) return;
    try {
      await loadChatMessages(false);
    } catch (e) {
      console.warn("chat poll", e);
    }
  }

  function startChatPanel() {
    state.chatActive = true;
    loadChatMessages(true).catch((e) => alert(e.message));
    if (state.chatPollTimer) clearInterval(state.chatPollTimer);
    state.chatPollTimer = setInterval(chatPollTick, 3000);
  }

  function stopChatPanel() {
    state.chatActive = false;
    if (state.chatPollTimer) {
      clearInterval(state.chatPollTimer);
      state.chatPollTimer = null;
    }
  }

  let chatUnreadBgTimer = null;

  function startChatUnreadBackgroundPoll() {
    if (chatUnreadBgTimer) return;
    chatUnreadBgTimer = setInterval(async () => {
      if (!state.token || state.chatActive) return;
      try {
        const res = await api("/parent/api/chat/unread");
        updateChatBadge(res.unread);
      } catch (e) {
        /* ignore */
      }
    }, 12000);
  }

  async function sendChatMessage() {
    const inp = $("#chat-input");
    const body = inp.value.trim();
    if (!body) return;
    const res = await api("/parent/api/chat/send", {
      method: "POST",
      body: JSON.stringify({ body }),
    });
    inp.value = "";
    if (res.message) {
      const box = $("#chat-messages");
      const exists = box && box.querySelector(`.chat-row[data-id="${res.message.id}"]`);
      if (!exists) {
        renderChatMessages([res.message], false);
      }
      if (res.message.id > state.chatLastId) state.chatLastId = res.message.id;
    }
  }

  async function forceStudyNow() {
    if (!state.policy) {
      state.policy = {
        version: 1,
        timezone: "Asia/Shanghai",
        rules: [],
        app_toggles: {},
      };
    }
    state.policy.demo_force_study = true;
    try {
      await savePolicy(state.policy);
      updateStudyModeUI();
      alert(
        "已启用学习时段：设备将暂时锁定 2048/视频/音乐/相机/打印。应用开关不会关闭，点「结束学习时段」或重新打开应用开关即可恢复。"
      );
    } catch (e) {
      alert(e.message);
    }
  }

  async function endStudyNow() {
    if (!state.policy) return;
    state.policy.demo_force_study = false;
    try {
      await savePolicy(state.policy);
      $("#rule-chip").textContent = ruleSummary(state.policy);
      updateStudyModeUI();
      alert("已结束学习时段，设备联网同步后即可打开娱乐应用。");
    } catch (e) {
      alert(e.message);
    }
  }

  $("#login-form").addEventListener("submit", async (e) => {
    e.preventDefault();
    const err = $("#login-error");
    err.hidden = true;
    const device_code = $("#device-code").value.trim().toUpperCase();
    const password = $("#password").value;
    try {
      const res = await api("/parent/api/login", {
        method: "POST",
        body: JSON.stringify({ device_code, password }),
      });
      state.token = res.token;
      state.deviceId = res.device_id;
      if ($("#remember").checked) localStorage.setItem(STORAGE_DEVICE, state.deviceId);
      else localStorage.removeItem(STORAGE_DEVICE);
      if ($("#save-password").checked) {
        localStorage.setItem(STORAGE_PASSWORD, password);
        localStorage.setItem(STORAGE_TOKEN, state.token);
      } else {
        localStorage.removeItem(STORAGE_PASSWORD);
        localStorage.removeItem(STORAGE_TOKEN);
      }
      showApp();
      await loadLive();
      startChatUnreadBackgroundPoll();
    } catch (ex) {
      err.textContent = ex.message;
      err.hidden = false;
    }
  });

  $("#btn-logout").addEventListener("click", showLogin);

  document.querySelectorAll(".nav-item").forEach((btn) => {
    btn.addEventListener("click", () => switchView(btn.dataset.view));
  });

  document.querySelectorAll("[data-goto]").forEach((btn) => {
    btn.addEventListener("click", () => switchView(btn.dataset.goto));
  });

  $("#filter-mode").addEventListener("change", () => {
    renderHistoryList($("#history-list"), filterHistory(state.history));
  });

  $("#btn-refresh-history").addEventListener("click", () => {
    loadLive().catch((e) => alert(e.message));
  });

  $("#btn-save-policy").addEventListener("click", async () => {
    const msg = $("#policy-save-msg");
    msg.hidden = true;
    try {
      const pol = collectPolicyFromForm();
      pol.demo_force_study = false;
      await savePolicy(pol);
      fillPolicyForm(state.policy);
      msg.textContent = "已保存，设备将在下次联网时同步";
      msg.hidden = false;
    } catch (e) {
      alert("保存失败: " + e.message);
    }
  });

  $("#btn-force-study").addEventListener("click", forceStudyNow);
  $("#btn-end-study").addEventListener("click", endStudyNow);

  /** WeChat/phone names often use Chinese; server stores ASCII-safe .jpg only. */
  function safeAlbumFilename(raw) {
    const base = String(raw || "photo.jpg")
      .split(/[/\\]/)
      .pop()
      .trim();
    let stem = base.replace(/\.(jpe?g)$/i, "");
    stem = stem.replace(/[^\w.\-]+/g, "_").replace(/^[\W_]+|[\W_]+$/g, "");
    if (!stem) stem = "photo";
    if (stem.length > 48) stem = stem.slice(0, 48);
    return stem + ".jpg";
  }

  async function uploadAlbumFiles(fileList) {
    const msg = $("#album-upload-msg");
    const list = $("#album-upload-list");
    msg.hidden = true;
    list.innerHTML = "";
    const nameOverride = ($("#album-name").value || "").trim();
    for (let i = 0; i < fileList.length; i++) {
      const file = fileList[i];
      const fd = new FormData();
      fd.append("file", file);
      const rawName = fileList.length === 1 && nameOverride ? nameOverride : file.name;
      fd.append("name", safeAlbumFilename(rawName));
      const data = await parentUpload("/parent/api/album/upload", fd);
      const li = document.createElement("li");
      li.className = "history-item";
      const kb = Math.round((data.item.size || data.item.processed_size || 0) / 1024);
      let detail = escapeHtml(data.item.name) + " (" + kb + " KB)";
      if (data.item.width && data.item.height) {
        detail += " · " + data.item.width + "×" + data.item.height;
      }
      if (data.item.source_width && data.item.source_height) {
        const sw = data.item.source_width;
        const sh = data.item.source_height;
        if (sw !== data.item.width || sh !== data.item.height) {
          detail += "（原图 " + sw + "×" + sh + " 已缩小）";
        }
      }
      li.innerHTML =
        '<div class="history-body"><p class="history-preview">已排队：' + detail + "</p></div>";
      list.appendChild(li);
    }
    msg.textContent = "已上传 " + fileList.length + " 张，设备联网后自动写入 SD 卡";
    msg.hidden = false;
  }

  $("#btn-album-upload").addEventListener("click", () => {
    const input = $("#album-file");
    if (!input.files || !input.files.length) {
      alert("请选择照片（JPG/PNG/WebP）");
      return;
    }
    uploadAlbumFiles(input.files).catch((e) => alert("上传失败: " + e.message));
  });

  function appendPrintUploadResult(data) {
    const msg = $("#print-upload-msg");
    const list = $("#print-upload-list");
    msg.hidden = true;
    const li = document.createElement("li");
    li.className = "history-item";
    const item = data.item || {};
    const kind = item.type === "text" ? "文字" : "图片";
    let detail = kind + " · " + escapeHtml(item.name || "job");
    if (item.size) {
      detail += " (" + Math.round(item.size / 1024) + " KB)";
    }
    if (item.width && item.height) {
      detail += " · " + item.width + "×" + item.height;
    }
    li.innerHTML =
      '<div class="history-body"><p class="history-preview">已排队 #' +
      item.id +
      "：" +
      detail +
      "</p></div>";
    list.prepend(li);
    msg.textContent = "已加入打印队列，请在设备「打印」App 中确认出纸";
    msg.hidden = false;
  }

  async function uploadPrintImage() {
    const input = $("#print-image-file");
    if (!input.files || !input.files.length) {
      alert("请选择照片");
      return;
    }
    const file = input.files[0];
    const fd = new FormData();
    fd.append("type", "image");
    fd.append("file", file);
    const nameOverride = ($("#print-image-name").value || "").trim();
    fd.append("name", safeAlbumFilename(nameOverride || file.name));
    fd.append("binarize", $("#print-binarize").checked ? "1" : "0");
    const data = await parentUpload("/parent/api/print/upload", fd);
    appendPrintUploadResult(data);
    input.value = "";
  }

  async function uploadPrintText() {
    const body = ($("#print-text-body").value || "").trim();
    if (!body) {
      alert("请输入要打印的文字");
      return;
    }
    const fd = new FormData();
    fd.append("type", "text");
    fd.append("body", body);
    const data = await parentUpload("/parent/api/print/upload", fd);
    appendPrintUploadResult(data);
    $("#print-text-body").value = "";
  }

  function alertUploadError(e) {
    if (e && e.message === "session_expired") {
      return;
    }
    alert("上传失败: " + (e && e.message ? e.message : String(e)));
  }

  $("#btn-print-image-upload").addEventListener("click", () => {
    uploadPrintImage().catch(alertUploadError);
  });

  $("#btn-print-text-upload").addEventListener("click", () => {
    uploadPrintText().catch(alertUploadError);
  });

  $("#btn-chat-send").addEventListener("click", () => {
    sendChatMessage().catch((e) => alert(e.message));
  });
  $("#chat-input").addEventListener("keydown", (e) => {
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      sendChatMessage().catch((err) => alert(err.message));
    }
  });

  $("#drawer-close").addEventListener("click", (e) => {
    e.preventDefault();
    e.stopPropagation();
    closeDrawer();
  });
  $("#drawer-download").addEventListener("click", (e) => {
    e.preventDefault();
    e.stopPropagation();
    downloadDrawerPhoto();
  });
  $("#drawer-backdrop").addEventListener("click", closeDrawer);
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") { closeDrawer(); closeMenu(); }
  });

  bindPolicyFormInteractions();
  closeDrawer();

  if (localStorage.getItem(STORAGE_DEVICE)) {
    $("#device-code").value = localStorage.getItem(STORAGE_DEVICE);
    if ($("#remember")) $("#remember").checked = true;
  }
  const savedPwd = localStorage.getItem(STORAGE_PASSWORD);
  if (savedPwd) {
    $("#password").value = savedPwd;
    if ($("#save-password")) $("#save-password").checked = true;
  }

  if (state.token) {
    showApp();
    loadLive()
      .then(() => {
        startChatUnreadBackgroundPoll();
        return api("/parent/api/chat/unread");
      })
      .then((res) => updateChatBadge(res.unread))
      .catch(showLogin);
  }
})();
