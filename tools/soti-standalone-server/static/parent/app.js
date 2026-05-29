(function () {
  "use strict";

  const STORAGE_TOKEN = "parent_token";
  const STORAGE_DEVICE = "parent_device";
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
    if (item.id) {
      api("/parent/api/history/" + item.id)
        .then((res) => {
          if (res.item) {
            body.textContent = res.item.answer_text || "";
            if (res.item.thumb_url) {
              thumb.src = res.item.thumb_url;
              thumb.hidden = false;
            }
          }
        })
        .catch(() => {});
    }
  }

  function closeDrawer() {
    const drawer = $("#drawer");
    if (!drawer) return;
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
    $("#stat-today").textContent = st.today ?? "0";
    $("#stat-week").textContent = st.week ?? "0";
    $("#stat-policy").textContent = data.policy_name || "—";
    $("#sidebar-online").classList.toggle("dot--online", !!st.online);
    $("#sidebar-online-text").textContent = st.online ? "在线" : "离线";

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
    const login = $("#view-login");
    const shell = $("#view-app");
    login.classList.remove("view--active");
    login.hidden = true;
    login.style.display = "";
    shell.hidden = false;
    $("#sidebar-device").textContent = state.deviceId;
    $("#top-device-pill").textContent = state.deviceId;
  }

  function showLogin() {
    closeDrawer();
    stopChatPanel();
    state.token = "";
    localStorage.removeItem(STORAGE_TOKEN);
    $("#view-app").hidden = true;
    const login = $("#view-login");
    login.hidden = false;
    login.classList.add("view--active");
    login.style.display = "";
  }

  function switchView(name) {
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
    if (name === "chat") {
      startChatPanel();
    } else {
      stopChatPanel();
    }
  }

  function formatChatTime(ts) {
    const d = new Date(ts * 1000);
    return d.toLocaleTimeString("zh-CN", { hour: "2-digit", minute: "2-digit" });
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
    box.scrollTop = box.scrollHeight;
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
      if ($("#remember").checked) {
        localStorage.setItem(STORAGE_TOKEN, state.token);
        localStorage.setItem(STORAGE_DEVICE, state.deviceId);
      } else {
        localStorage.removeItem(STORAGE_TOKEN);
        localStorage.setItem(STORAGE_DEVICE, state.deviceId);
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
    const title = ($("#print-text-name").value || "").trim();
    if (title) {
      fd.append("name", title);
    }
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
  $("#drawer-backdrop").addEventListener("click", closeDrawer);
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") closeDrawer();
  });

  bindPolicyFormInteractions();
  closeDrawer();

  if (localStorage.getItem(STORAGE_DEVICE)) {
    $("#device-code").value = localStorage.getItem(STORAGE_DEVICE);
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
