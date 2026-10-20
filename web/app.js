/* 鱼腥草 NAS 控制台 —— 纯原生 JS SPA，与 mwine-cored REST API 对接 */
'use strict';

// ===================== 基础工具 =====================
const $ = (sel, root) => (root || document).querySelector(sel);
const $$ = (sel, root) => Array.from((root || document).querySelectorAll(sel));

function esc(s) {
  return String(s ?? '').replace(/[&<>"']/g, c =>
    ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function toast(msg, kind = 'ok', ms = 3200) {
  const t = document.createElement('div');
  t.className = 'toast ' + kind;
  t.textContent = msg;
  $('#toast-wrap').appendChild(t);
  setTimeout(() => t.remove(), ms);
}

function fmtMb(mb) {
  if (mb >= 1024) return (mb / 1024).toFixed(1) + ' GB';
  return Math.round(mb) + ' MB';
}
function fmtUptime(sec) {
  sec = Math.floor(sec);
  const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60);
  if (h > 0) return `${h} 小时 ${m} 分`;
  if (m > 0) return `${m} 分 ${sec % 60} 秒`;
  return `${sec} 秒`;
}
function fmtBytes(n) {
  if (n >= 1 << 30) return (n / (1 << 30)).toFixed(2) + ' GB';
  if (n >= 1 << 20) return (n / (1 << 20)).toFixed(1) + ' MB';
  if (n >= 1024) return (n / 1024).toFixed(0) + ' KB';
  return n + ' B';
}

// ===================== 鉴权与 API =====================
const S = {
  token: localStorage.getItem('mw_token') || '',
  page: 'overview',
  status: null,          // /api/status 缓存
  connected: false,
  timers: [],            // 页级轮询定时器
  term: null,            // {id, cursor, alive}
  screen: { live: false, timer: null },
  player: null,          // ScrcpyWebPlayer 投屏实例
};

function saveToken(t) {
  S.token = t || '';
  if (t) localStorage.setItem('mw_token', t);
  else localStorage.removeItem('mw_token');
}

function showLogin(tip) {
  $('#login-overlay').classList.remove('hidden');
  if (tip) $('#login-tip').textContent = tip;
  setTimeout(() => $('#login-token').focus(), 50);
}
function hideLogin() { $('#login-overlay').classList.add('hidden'); }

// 统一 API 调用：自动带令牌；401 弹登录；返回解析后的 JSON（抛错含 error 文案）
async function api(method, path, body, opts = {}) {
  const headers = {};
  if (S.token) headers['Authorization'] = 'Bearer ' + S.token;
  let payload;
  if (body !== undefined && body !== null) {
    headers['Content-Type'] = 'application/json';
    payload = JSON.stringify(body);
  }
  let resp;
  try {
    resp = await fetch(path, { method, headers, body: payload, signal: opts.signal });
  } catch (e) {
    setConn(false);
    throw new Error('无法连接服务器');
  }
  setConn(true);
  if (resp.status === 401) {
    showLogin('令牌无效或已过期，请重新输入');
    throw new Error('未授权');
  }
  let data = null;
  const text = await resp.text();
  try { data = text ? JSON.parse(text) : {}; } catch { data = { raw: text }; }
  if (!resp.ok) {
    const err = new Error(data.error || `请求失败 (${resp.status})`);
    err.status = resp.status;
    err.data = data;
    throw err;
  }
  return data;
}
const apiGet = p => api('GET', p);
const apiPost = (p, b) => api('POST', p, b ?? {});

// 需要 <img> 直出的 URL 拼接 token
function imgUrl(path) {
  return path + (path.includes('?') ? '&' : '?') + 'token=' + encodeURIComponent(S.token);
}

function setConn(on) {
  if (S.connected === on) return;
  S.connected = on;
  const pill = $('#conn-pill');
  pill.className = 'conn-pill ' + (on ? 'online' : 'offline');
  pill.textContent = on ? '已连接' : '连接断开';
}

// ===================== 页级轮询管理 =====================
function clearPolls() {
  S.timers.forEach(clearInterval);
  S.timers = [];
  stopScreenLive();
  if (S.player) { const p = S.player; S.player = null; p.stop(); }
}
function addPoll(fn, ms) {
  fn();
  S.timers.push(setInterval(fn, ms));
}
document.addEventListener('visibilitychange', () => {
  // 切后台页签时清轮询，回来重渲染
  if (document.hidden) clearPolls();
  else renderPage(S.page);
});

// ===================== 路由 =====================
const PAGES = {
  overview: { title: '概览', render: renderOverview },
  apps: { title: '应用库', render: renderApps },
  store: { title: '在线商店', render: renderStore },
  images: { title: '镜像与运行时', render: renderImages },
  terminal: { title: 'ADB 终端', render: renderTerminal },
  settings: { title: '设置', render: renderSettings },
};

function renderPage(page) {
  if (!PAGES[page]) page = 'overview';
  clearPolls();
  if (S.term) { apiPost(`/api/term/${S.term.id}/close`).catch(() => {}); S.term = null; }
  S.page = page;
  $$('#nav a').forEach(a => a.classList.toggle('active', a.dataset.page === page));
  $('#page-title').textContent = PAGES[page].title;
  const view = $('#view');
  view.innerHTML = '';
  PAGES[page].render(view);
}
window.addEventListener('hashchange', () => renderPage(location.hash.slice(1)));

// ===================== 顶栏状态 =====================
async function refreshTopbar() {
  try {
    const st = await apiGet('/api/status');
    S.status = st;
    const vm = st.vm || {};
    const pill = $('#vm-pill');
    pill.className = 'vm-pill ' + (vm.running ? 'on' : 'off');
    pill.textContent = vm.running ? ('运行中 · ' + (vm.serial || '')) : '未运行';
    $('#daemon-ver').textContent = st.version ? 'v' + st.version : '';
  } catch { /* 连接错误已由 api() 上报 */ }
}
setInterval(refreshTopbar, 4000);

// ===================== 概览 =====================
async function renderOverview(view) {
  const canStream = window.ScrcpyWebPlayer && ScrcpyWebPlayer.supported();
  view.innerHTML = `
    <div class="overview-grid">
      <div class="card">
        <h3>设备屏幕
          <span class="spacer"></span>
          <button class="btn sm" id="ov-shot">保存截图</button>
          ${canStream
            ? '<button class="btn sm primary" id="ov-stream">▶ 投屏</button>'
            : '<button class="btn sm" id="ov-live">实时画面</button>'}
        </h3>
        <div class="screen-box" id="ov-screen">
          <div class="placeholder">虚拟机未运行<br><span class="small muted">启动后此处显示设备画面</span></div>
        </div>
        <div id="ov-stream-bar" class="stream-bar hidden">
          <div class="btn-row">
            <button class="btn sm" data-key="4">返回</button>
            <button class="btn sm" data-key="3">主页</button>
            <button class="btn sm" data-key="187">多任务</button>
            <button class="btn sm" data-key="26">电源</button>
            <button class="btn sm" data-key="24">音量+</button>
            <button class="btn sm" data-key="25">音量−</button>
          </div>
          <div class="term-input-row" style="margin-top:8px;">
            <input class="form-input" id="ov-text" placeholder="输入文本发送到设备（中文请用设备内输入法）">
            <button class="btn sm" id="ov-text-send">发送</button>
          </div>
          <div class="small muted" id="ov-stream-status" style="margin-top:6px;"></div>
        </div>
      </div>
      <div style="display:grid;gap:18px;">
        <div class="card">
          <h3>虚拟机</h3>
          <div id="ov-status" class="muted small">读取中…</div>
          <div class="btn-row" style="margin-top:14px;">
            <button class="btn primary" id="ov-start">▶ 启动</button>
            <button class="btn" id="ov-stop">■ 停止（自动存档）</button>
          </div>
          <div id="ov-error" class="small" style="color:var(--red);margin-top:10px;"></div>
        </div>
        <div class="card">
          <h3>性能监控</h3>
          <div class="stat-grid" id="ov-perf">
            <div class="stat-box"><div class="stat-label">虚拟机未运行</div></div>
          </div>
        </div>
      </div>
    </div>`;

  $('#ov-start').onclick = () => vmAction('start');
  $('#ov-stop').onclick = () => vmAction('stop');
  $('#ov-shot').onclick = downloadScreenshot;
  const streamBtn = $('#ov-stream');
  if (streamBtn) streamBtn.onclick = () => S.player ? stopStream() : startStream();
  const liveBtn = $('#ov-live');
  if (liveBtn) liveBtn.onclick = () => S.screen.live ? stopScreenLive() : startScreenLive();
  $$('#ov-stream-bar [data-key]').forEach(btn => {
    btn.onclick = () => S.player && S.player.key(+btn.dataset.key);
  });
  $('#ov-text-send').onclick = () => {
    const inp = $('#ov-text');
    if (S.player && inp.value) { S.player.text(inp.value); inp.value = ''; }
  };
  $('#ov-text').onkeydown = e => {
    if (e.key === 'Enter') $('#ov-text-send').click();
    e.stopPropagation();
  };

  addPoll(updateOverviewStatus, 3000);
  addPoll(updateOverviewPerf, 2500);
}

// ---- scrcpy Web 投屏 ----
async function startStream() {
  if (S.player) return;
  stopScreenLive();
  const box = screenBox();
  if (!box) return;
  box.innerHTML = '<div class="placeholder"><span class="spin">⏳</span> 正在建立投屏…</div>';
  const player = new ScrcpyWebPlayer({
    container: box,
    apiPost,
    imgUrl,
    onStatus: (text, isError) => {
      const el = $('#ov-stream-status');
      if (el) {
        el.textContent = text;
        el.style.color = isError ? 'var(--red)' : '';
      }
    },
    onEnd: () => {
      S.player = null;
      const bar = $('#ov-stream-bar');
      if (bar) bar.classList.add('hidden');
      const btn = $('#ov-stream');
      if (btn) { btn.textContent = '▶ 投屏'; btn.classList.add('primary'); }
      if (screenBox()) loadScreenshotOnce();
    },
  });
  S.player = player;
  const bar = $('#ov-stream-bar');
  if (bar) bar.classList.remove('hidden');
  const btn = $('#ov-stream');
  if (btn) { btn.textContent = '■ 停止投屏'; btn.classList.remove('primary'); }
  try {
    await player.start();
  } catch (e) {
    // 会话建立失败（编码器不可用/设备未运行等）：复位 UI + 提示
    S.player = null;
    player._finish();
    toast('投屏失败：' + e.message, 'err', 5000);
    const b2 = $('#ov-stream');
    if (b2) { b2.textContent = '▶ 投屏'; b2.classList.add('primary'); }
    const bar2 = $('#ov-stream-bar');
    if (bar2) bar2.classList.add('hidden');
    if (screenBox()) loadScreenshotOnce();
  }
}

async function stopStream() {
  if (!S.player) return;
  const p = S.player;
  S.player = null;
  await p.stop();
  if (screenBox()) loadScreenshotOnce();
}

async function vmAction(act) {
  const btn = act === 'start' ? $('#ov-start') : $('#ov-stop');
  btn.disabled = true;
  btn.innerHTML = '<span class="spin">⏳</span> ' + (act === 'start' ? '启动中…' : '存档停止中…');
  try {
    const r = await apiPost('/api/vm/' + act);
    toast(act === 'start'
      ? (r.already ? '虚拟机已在运行' : '启动指令已下发，快照恢复约 5 秒')
      : '停止指令已下发（正在存档）');
  } catch (e) { toast(e.message, 'err'); }
  setTimeout(() => { renderPage('overview'); }, act === 'start' ? 6000 : 3000);
}

async function updateOverviewStatus() {
  await refreshTopbar();
  const st = S.status;
  if (!st || !$('#ov-status')) return;
  const vm = st.vm || {};
  $('#ov-status').innerHTML = vm.running
    ? `状态：<b style="color:var(--accent)">运行中</b> · 设备 <span class="mono">${esc(vm.serial || '-')}</span>`
    : '状态：<b>未运行</b>';
  $('#ov-error').textContent = st.error || '';
  if (vm.running && !S.screen.live && !S.player) loadScreenshotOnce();
}

async function updateOverviewPerf() {
  const box = $('#ov-perf');
  if (!box) return;
  let p;
  try { p = await apiGet('/api/perf'); } catch { return; }
  if (!p.running) {
    box.innerHTML = '<div class="stat-box"><div class="stat-label">虚拟机未运行</div></div>';
    return;
  }
  const cpu = p.cpu_pct ?? 0;
  const mem = p.mem || {};
  const memUsed = (mem.total_mb || 0) - (mem.available_mb || 0);
  const memPct = mem.total_mb ? (memUsed * 100 / mem.total_mb) : 0;
  const host = p.host || {};
  const barCls = v => v > 85 ? 'bar crit' : (v > 65 ? 'bar warn' : 'bar');
  box.innerHTML = `
    <div class="stat-box"><div class="stat-label">CPU 占用</div>
      <div class="stat-value">${cpu.toFixed(0)}%</div>
      <div class="${barCls(cpu)}"><i style="width:${Math.min(cpu, 100)}%"></i></div></div>
    <div class="stat-box"><div class="stat-label">内存</div>
      <div class="stat-value">${fmtMb(memUsed)} <span class="small muted">/ ${fmtMb(mem.total_mb || 0)}</span></div>
      <div class="${barCls(memPct)}"><i style="width:${Math.min(memPct, 100)}%"></i></div></div>
    <div class="stat-box"><div class="stat-label">已运行</div>
      <div class="stat-value">${fmtUptime(p.uptime_s || 0)}</div></div>
    <div class="stat-box"><div class="stat-label">宿主 qemu 开销</div>
      <div class="stat-value">${(host.qemu_cpu_pct || 0).toFixed(0)}%</div>
      <div class="stat-label">CPU · ${fmtMb(host.qemu_mem_mb || 0)} 内存</div></div>`;
}

function screenBox() { return $('#ov-screen'); }

async function loadScreenshotOnce() {
  const box = screenBox();
  if (!box) return;
  const url = imgUrl('/api/screenshot.png') + '&t=' + Date.now();
  const img = new Image();
  img.onload = () => {
    if (!screenBox()) return;
    box.innerHTML = '';
    box.appendChild(img);
  };
  img.onerror = () => {};
  img.src = url;
}

function startScreenLive() {
  S.screen.live = true;
  const btn = $('#ov-live');
  if (btn) btn.textContent = '暂停画面';
  const tick = () => {
    if (!S.screen.live || !screenBox()) return stopScreenLive();
    const box = screenBox();
    const url = imgUrl('/api/screenshot.png') + '&t=' + Date.now();
    const img = new Image();
    img.onload = () => {
      if (!S.screen.live || !screenBox()) return;
      box.innerHTML = '<div class="screen-live-dot">● 实时</div>';
      box.appendChild(img);
    };
    img.src = url;
  };
  tick();
  S.screen.timer = setInterval(tick, 1200);
}
function stopScreenLive() {
  S.screen.live = false;
  if (S.screen.timer) { clearInterval(S.screen.timer); S.screen.timer = null; }
  const btn = $('#ov-live');
  if (btn) btn.textContent = '实时画面';
}

async function downloadScreenshot() {
  try {
    const resp = await fetch(imgUrl('/api/screenshot.png'));
    if (!resp.ok) throw new Error('截图失败（虚拟机在运行吗？）');
    const blob = await resp.blob();
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'mwine-' + new Date().toISOString().replace(/[:.]/g, '-') + '.png';
    a.click();
    URL.revokeObjectURL(a.href);
    toast('截图已下载');
  } catch (e) { toast(e.message, 'err'); }
}

// ===================== 应用库 =====================
async function renderApps(view) {
  view.innerHTML = `
    <div class="card">
      <div class="toolbar">
        <button class="btn primary" id="ap-upload">⬆ 上传 APK 安装</button>
        <input type="file" id="ap-file" accept=".apk" hidden>
        <button class="btn" id="ap-refresh">刷新列表</button>
        <span class="spacer"></span>
        <input class="search-input" id="ap-filter" placeholder="筛选应用名 / 包名">
      </div>
    </div>
    <div id="ap-task"></div>
    <div class="card">
      <h3>已安装应用</h3>
      <div class="app-grid" id="ap-grid"><div class="empty">读取中…</div></div>
    </div>`;

  $('#ap-upload').onclick = () => $('#ap-file').click();
  $('#ap-file').onchange = e => { if (e.target.files[0]) uploadApk(e.target.files[0]); e.target.value = ''; };
  $('#ap-refresh').onclick = loadApps;
  $('#ap-filter').oninput = () => filterApps();
  loadApps();
  pollInstallTask();
}

let APPS_CACHE = [];
async function loadApps() {
  const grid = $('#ap-grid');
  try {
    const d = await apiGet('/api/apps');
    APPS_CACHE = d.apps || [];
    renderAppGrid(APPS_CACHE);
  } catch (e) {
    if (grid) grid.innerHTML = `<div class="empty">${esc(e.message)}</div>`;
  }
}

function renderAppGrid(apps) {
  const grid = $('#ap-grid');
  if (!grid) return;
  if (!apps.length) {
    grid.innerHTML = '<div class="empty">暂无应用 —— 上传 APK 或到在线商店安装</div>';
    return;
  }
  grid.innerHTML = apps.map(a => `
    <div class="app-card" data-pkg="${esc(a.package)}">
      <div class="app-icon" data-pkg="${esc(a.package)}">${esc((a.name || a.package).slice(0, 1).toUpperCase())}</div>
      <div class="app-name" title="${esc(a.name)}">${esc(a.name)}</div>
      <div class="app-pkg" title="${esc(a.package)}">${esc(a.package)}</div>
      <div class="app-actions">
        <button class="btn sm primary" data-act="run">启动</button>
        <button class="btn sm ghost" data-act="un">卸载</button>
      </div>
    </div>`).join('');
  // 图标异步填充
  apps.forEach(a => {
    const holder = $(`.app-icon[data-pkg="${CSS.escape(a.package)}"]`, grid);
    if (!holder) return;
    const img = new Image();
    img.onload = () => { holder.innerHTML = ''; holder.appendChild(img); };
    img.src = imgUrl('/api/apps/icon?pkg=' + encodeURIComponent(a.package));
  });
  $$('.app-card [data-act]', grid).forEach(btn => {
    btn.onclick = async () => {
      const pkg = btn.closest('.app-card').dataset.pkg;
      if (btn.dataset.act === 'run') {
        try { await apiPost(`/api/apps/${encodeURIComponent(pkg)}/run`); toast('已下发启动指令'); }
        catch (e) { toast(e.message, 'err'); }
      } else {
        if (!confirm(`确定卸载 ${pkg}？（应用数据将被清除）`)) return;
        try {
          const r = await apiPost(`/api/apps/${encodeURIComponent(pkg)}/uninstall`);
          if (r.ok) { toast('已卸载'); setTimeout(loadApps, 1200); }
          else toast('卸载失败：' + (r.output || ''), 'err');
        } catch (e) { toast(e.message, 'err'); }
      }
    };
  });
}

function filterApps() {
  const q = ($('#ap-filter').value || '').trim().toLowerCase();
  renderAppGrid(!q ? APPS_CACHE :
    APPS_CACHE.filter(a => (a.name || '').toLowerCase().includes(q) || a.package.toLowerCase().includes(q)));
}

// XHR 上传（fetch 无上传进度）
function uploadApk(file) {
  if (file.size > 512 * 1024 * 1024) return toast('APK 超过 512MB 上限', 'err');
  const box = $('#ap-task');
  box.innerHTML = `
    <div class="card task-card">
      <h3>上传 ${esc(file.name)}</h3>
      <div class="progress-line"><span>上传到服务器…</span><span id="ap-up-pct">0%</span></div>
      <div class="bar"><i id="ap-up-bar" style="width:0%"></i></div>
    </div>`;
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/apps/install-upload?name=' + encodeURIComponent(file.name));
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');
  if (S.token) xhr.setRequestHeader('Authorization', 'Bearer ' + S.token);
  xhr.upload.onprogress = e => {
    if (!e.lengthComputable) return;
    const pct = Math.round(e.loaded * 100 / e.total);
    const bar = $('#ap-up-bar'), lab = $('#ap-up-pct');
    if (bar) bar.style.width = pct + '%';
    if (lab) lab.textContent = pct + '%（' + fmtBytes(e.loaded) + ' / ' + fmtBytes(e.total) + '）';
  };
  xhr.onload = () => {
    let d = {};
    try { d = JSON.parse(xhr.responseText); } catch {}
    if (xhr.status >= 200 && xhr.status < 300) {
      toast('上传完成，开始安装');
      pollInstallTask(true);
    } else {
      box.innerHTML = '';
      toast(d.error || '上传失败', 'err');
    }
  };
  xhr.onerror = () => { box.innerHTML = ''; toast('上传失败：网络错误', 'err'); };
  xhr.send(file);
}

let INSTALL_POLLING = false;
async function pollInstallTask(force) {
  if (INSTALL_POLLING && !force) return;
  INSTALL_POLLING = true;
  const box = $('#ap-task');
  try {
    while (S.page === 'apps') {
      const t = await apiGet('/api/apps/install/task');
      if (!t.running && !t.done && !t.error) break; // 无任务
      if (box) {
        const pct = t.progress_total ? Math.round(t.progress * 100 / t.progress_total) : 0;
        box.innerHTML = `
          <div class="card task-card">
            <h3>${t.running ? '<span class="spin">⏳</span>' : (t.error ? '⚠' : '✅')}
              安装 ${esc(t.label || t.package || '')}</h3>
            <div class="progress-line"><span>${esc(t.phase || '')}</span>
              <span>${t.progress_total ? pct + '%（' + fmtBytes(t.progress) + ' / ' + fmtBytes(t.progress_total) + '）' : ''}</span></div>
            ${t.progress_total ? `<div class="bar"><i style="width:${pct}%"></i></div>` : ''}
            ${t.error ? `<div class="small" style="color:var(--red);margin-top:8px;">${esc(t.error)}</div>` : ''}
            ${!t.running ? '<button class="btn sm" style="margin-top:10px;" id="ap-task-close">知道了</button>' : ''}
          </div>`;
        const close = $('#ap-task-close');
        if (close) close.onclick = () => { box.innerHTML = ''; INSTALL_POLLING = false; };
      }
      if (!t.running) {
        INSTALL_POLLING = false;
        if (t.done) { toast('安装完成'); setTimeout(loadApps, 1500); }
        else if (t.error) toast('安装失败：' + t.error, 'err', 5000);
        break;
      }
      await new Promise(r => setTimeout(r, 1200));
    }
  } catch { INSTALL_POLLING = false; }
}

// ===================== 在线商店 =====================
let STORE = { sources: [], source: '', results: [], searching: false };

async function renderStore(view) {
  view.innerHTML = `
    <div class="card">
      <div class="toolbar">
        <select class="form-select" id="st-source"></select>
        <input class="search-input" id="st-q" placeholder="搜索应用，如 Fennec / 微信 / Termux" style="flex:1;min-width:200px;">
        <button class="btn primary" id="st-search">搜索</button>
      </div>
    </div>
    <div id="st-task"></div>
    <div class="card">
      <h3 id="st-list-title">推荐应用</h3>
      <div id="st-list"><div class="empty">读取中…</div></div>
    </div>`;

  try {
    const d = await apiGet('/api/store/sources');
    STORE.sources = d.sources || [];
  } catch { STORE.sources = [{ id: 'fdroid', name: 'F-Droid' }, { id: 'wandoujia', name: '豌豆荚' }]; }
  const sel = $('#st-source');
  sel.innerHTML = STORE.sources.map(s => `<option value="${esc(s.id)}">${esc(s.name)}</option>`).join('');
  STORE.source = STORE.sources[0]?.id || 'fdroid';
  sel.value = STORE.source;
  sel.onchange = () => { STORE.source = sel.value; loadRecommend(); };
  $('#st-search').onclick = doStoreSearch;
  $('#st-q').onkeydown = e => { if (e.key === 'Enter') doStoreSearch(); };
  loadRecommend();
  pollStoreTask();
}

async function loadRecommend() {
  $('#st-list-title').textContent = '推荐应用（首次需拉取索引，约十几秒）';
  $('#st-list').innerHTML = '<div class="empty"><span class="spin">⏳</span> 读取中…</div>';
  try {
    const d = await apiGet('/api/store/recommend?source=' + encodeURIComponent(STORE.source));
    STORE.results = d.apps || [];
    $('#st-list-title').textContent = '推荐应用';
    renderStoreList();
  } catch (e) {
    $('#st-list').innerHTML = `<div class="empty">${esc(e.message)}</div>`;
  }
}

async function doStoreSearch() {
  const q = $('#st-q').value.trim();
  if (!q || STORE.searching) return;
  STORE.searching = true;
  $('#st-list-title').textContent = `搜索「${q}」…`;
  $('#st-list').innerHTML = '<div class="empty"><span class="spin">⏳</span> 搜索中（首次可能拉取索引）…</div>';
  try {
    const d = await apiGet(`/api/store/search?q=${encodeURIComponent(q)}&source=${encodeURIComponent(STORE.source)}`);
    STORE.results = d.apps || [];
    $('#st-list-title').textContent = `搜索「${q}」（${STORE.results.length} 条）`;
    renderStoreList();
  } catch (e) {
    $('#st-list').innerHTML = `<div class="empty">${esc(e.message)}</div>`;
  }
  STORE.searching = false;
}

function renderStoreList() {
  const list = $('#st-list');
  if (!list) return;
  if (!STORE.results.length) { list.innerHTML = '<div class="empty">无结果</div>'; return; }
  list.innerHTML = STORE.results.map((a, i) => `
    <div class="store-row">
      <div class="store-icon" data-i="${i}">${esc((a.name || a.package || '?').slice(0, 1).toUpperCase())}</div>
      <div class="store-info">
        <div class="store-name">${esc(a.name || a.package)}</div>
        <div class="store-meta">${esc(a.package || '')}${a.install_count ? ' · ' + esc(a.install_count) : ''}${a.latest?.version_name ? ' · v' + esc(a.latest.version_name) : ''}</div>
        ${a.summary ? `<div class="store-summary">${esc(a.summary)}</div>` : ''}
      </div>
      <button class="btn sm" data-i="${i}" data-act="vers">版本</button>
      <button class="btn sm primary" data-i="${i}" data-act="inst">安装</button>
    </div>`).join('');
  // 远程图标（失败保持字母块）
  STORE.results.forEach((a, i) => {
    if (!a.icon_url) return;
    const holder = $(`.store-icon[data-i="${i}"]`, list);
    const img = new Image();
    img.onload = () => { holder.innerHTML = ''; holder.appendChild(img); };
    img.src = a.icon_url;
  });
  $$('[data-act]', list).forEach(btn => {
    btn.onclick = () => {
      const a = STORE.results[+btn.dataset.i];
      if (btn.dataset.act === 'inst') storeInstall(a);
      else showVersions(a);
    };
  });
}

async function storeInstall(a, ver) {
  const body = {
    source: a.source || STORE.source,
    app_id: a.app_id || '',
    package: a.package || '',
    name: a.name || a.package || '',
  };
  if (ver) {
    if (ver.code) body.version_code = ver.code;
    if (ver.name) body.version_name = ver.name;
  }
  try {
    await apiPost('/api/store/download', body);
    toast('下载任务已开始');
    pollStoreTask(true);
  } catch (e) { toast(e.message, 'err', 4500); }
}

async function showVersions(a) {
  const mask = document.createElement('div');
  mask.className = 'modal-mask';
  mask.innerHTML = `
    <div class="modal">
      <button class="close-x">✕</button>
      <h3>${esc(a.name || a.package)} · 选择版本</h3>
      <div id="ver-list"><div class="empty"><span class="spin">⏳</span> 读取版本…</div></div>
    </div>`;
  mask.querySelector('.close-x').onclick = () => mask.remove();
  mask.onclick = e => { if (e.target === mask) mask.remove(); };
  document.body.appendChild(mask);
  try {
    const src = a.source || STORE.source;
    const d = await apiGet(`/api/store/versions?source=${encodeURIComponent(src)}&app_id=${encodeURIComponent(a.app_id || '')}&package=${encodeURIComponent(a.package || '')}`);
    const vs = d.versions || [];
    const listEl = mask.querySelector('#ver-list');
    if (!vs.length) { listEl.innerHTML = '<div class="empty">无版本信息</div>'; return; }
    listEl.innerHTML = vs.map((v, i) => `
      <div class="ver-row">
        <div style="flex:1;min-width:0;">
          <div>v${esc(v.name || v.code || '?')}${d.installed_code && v.code === d.installed_code ? ' <span class="badge">已安装</span>' : ''}</div>
          <div class="small muted">${v.size ? fmtBytes(v.size) : ''}${v.added ? ' · ' + esc(v.added) : ''}</div>
        </div>
        <button class="btn sm primary" data-i="${i}">安装</button>
      </div>`).join('');
    $$('#ver-list [data-i]', mask).forEach(btn => {
      btn.onclick = () => { mask.remove(); storeInstall(a, vs[+btn.dataset.i]); };
    });
  } catch (e) {
    mask.querySelector('#ver-list').innerHTML = `<div class="empty">${esc(e.message)}</div>`;
  }
}

let STORE_POLLING = false;
async function pollStoreTask(force) {
  if (STORE_POLLING && !force) return;
  STORE_POLLING = true;
  const box = $('#st-task');
  try {
    while (S.page === 'store') {
      const t = await apiGet('/api/store/task');
      if (!t.running && !t.done && !t.error) break;
      if (box) {
        const pct = t.progress_total ? Math.round(t.progress * 100 / t.progress_total) : 0;
        box.innerHTML = `
          <div class="card task-card">
            <h3>${t.running ? '<span class="spin">⏳</span>' : (t.error ? '⚠' : '✅')}
              ${esc(t.label || t.package || '下载任务')} ${t.version ? 'v' + esc(t.version) : ''}
              <span class="spacer"></span>
              ${t.running ? '<button class="btn sm danger" id="st-cancel">取消</button>' : '<button class="btn sm" id="st-close">知道了</button>'}
            </h3>
            <div class="progress-line"><span>${esc(t.phase || '')}</span>
              <span>${t.progress_total ? pct + '%（' + fmtBytes(t.progress) + ' / ' + fmtBytes(t.progress_total) + '）' : ''}</span></div>
            ${t.progress_total ? `<div class="bar"><i style="width:${pct}%"></i></div>` : ''}
            ${t.error ? `<div class="small" style="color:var(--red);margin-top:8px;">${esc(t.error)}</div>` : ''}
          </div>`;
        const cancel = $('#st-cancel');
        if (cancel) cancel.onclick = () => apiPost('/api/store/cancel').catch(() => {});
        const close = $('#st-close');
        if (close) close.onclick = () => { box.innerHTML = ''; STORE_POLLING = false; };
      }
      if (!t.running) {
        STORE_POLLING = false;
        if (t.done) toast('下载安装完成');
        else if (t.error) toast('任务失败：' + t.error, 'err', 5000);
        break;
      }
      await new Promise(r => setTimeout(r, 1200));
    }
  } catch { STORE_POLLING = false; }
}

// ===================== 镜像与运行时 =====================
async function renderImages(view) {
  view.innerHTML = `
    <div class="card" id="rt-card"><h3>模拟器运行时</h3><div class="muted small">读取中…</div></div>
    <div id="img-task"></div>
    <div class="card">
      <h3>本地镜像 <span class="spacer"></span><button class="btn sm" id="img-refresh">刷新</button></h3>
      <div id="img-list"><div class="empty">读取中…</div></div>
    </div>
    <div class="card">
      <h3>在线镜像 <span class="spacer"></span><button class="btn sm" id="img-load-official">加载列表</button></h3>
      <div id="img-official"><div class="empty">点击「加载列表」查看可拉取的官方 / 社区镜像</div></div>
    </div>`;
  $('#img-refresh').onclick = loadLocalImages;
  $('#img-load-official').onclick = loadOfficialImages;
  loadRuntimeStatus();
  loadLocalImages();
  pollImageTask();
}

async function loadRuntimeStatus() {
  const card = $('#rt-card');
  try {
    const s = await apiGet('/api/runtime/status');
    card.innerHTML = `
      <h3>模拟器运行时</h3>
      ${s.present
        ? `<div class="small">✅ 已安装（${esc(s.source || '')}）${s.path ? ' · <span class="mono">' + esc(s.path) + '</span>' : ''}</div>`
        : `<div class="small muted">未安装运行时（约 330MB），虚拟机无法启动。</div>
           <div class="btn-row" style="margin-top:10px;"><button class="btn primary sm" id="rt-dl">在线下载运行时</button></div>`}`;
    const dl = $('#rt-dl');
    if (dl) dl.onclick = async () => {
      try { await apiPost('/api/runtime/download'); toast('运行时下载已开始'); pollImageTask(true); }
      catch (e) { toast(e.message, 'err'); }
    };
  } catch (e) {
    card.innerHTML = `<h3>模拟器运行时</h3><div class="small muted">${esc(e.message)}</div>`;
  }
}

async function loadLocalImages() {
  const box = $('#img-list');
  try {
    const d = await apiGet('/api/image/list');
    const imgs = d.images || [];
    if (!imgs.length) { box.innerHTML = '<div class="empty">暂无本地镜像 —— 从下方在线拉取</div>'; return; }
    box.innerHTML = imgs.map((im, i) => `
      <div class="img-row">
        <div style="flex:1;min-width:0;">
          <div>${esc(im.display || im.api)} · ${esc(im.name)} ${im.in_use ? '<span class="badge">使用中</span>' : ''}</div>
          <div class="small muted mono">${esc(im.rel)} · ${im.size_mb ? fmtMb(im.size_mb) : ''}</div>
        </div>
        ${!im.in_use ? `<button class="btn sm primary" data-act="use" data-i="${i}">使用</button>
                        <button class="btn sm danger" data-act="del" data-i="${i}">删除</button>` : ''}
      </div>`).join('');
    $$('[data-act]', box).forEach(btn => {
      btn.onclick = async () => {
        const im = imgs[+btn.dataset.i];
        if (btn.dataset.act === 'use') {
          if (!confirm(`切换到镜像 ${im.display || im.rel}？${S.status?.vm?.running ? '（虚拟机会先停止）' : ''}`)) return;
          try {
            await apiPost('/api/image/use', { path: im.path, stop_vm: true });
            toast('已切换镜像'); loadLocalImages();
          } catch (e) { toast(e.message, 'err', 4500); }
        } else {
          if (!confirm(`确定删除镜像 ${im.display || im.rel}？此操作不可恢复！`)) return;
          try {
            await apiPost('/api/image/delete', { path: im.path });
            toast('已删除'); loadLocalImages();
          } catch (e) { toast(e.message, 'err', 4500); }
        }
      };
    });
  } catch (e) {
    box.innerHTML = `<div class="empty">${esc(e.message)}</div>`;
  }
}

async function loadOfficialImages() {
  const box = $('#img-official');
  box.innerHTML = '<div class="empty"><span class="spin">⏳</span> 读取官方仓库…</div>';
  try {
    const d = await apiGet('/api/image/official');
    const items = d.images || d.items || [];
    if (!items.length) { box.innerHTML = '<div class="empty">仓库无可用镜像</div>'; return; }
    box.innerHTML = items.map((im, i) => `
      <div class="img-row">
        <div style="flex:1;min-width:0;">
          <div>${esc(im.display || im.path)}</div>
          <div class="small muted mono">${esc(im.path)}${im.size_mb ? ' · ' + fmtMb(im.size_mb) : ''}</div>
        </div>
        <button class="btn sm primary" data-i="${i}">拉取</button>
      </div>`).join('');
    $$('[data-i]', box).forEach(btn => {
      btn.onclick = async () => {
        const im = items[+btn.dataset.i];
        if (!confirm(`拉取镜像 ${im.display || im.path}？体积较大，请耐心等待。`)) return;
        try {
          await apiPost('/api/image/official-pull', { target: im.path });
          toast('拉取任务已开始'); pollImageTask(true);
        } catch (e) { toast(e.message, 'err', 4500); }
      };
    });
  } catch (e) {
    box.innerHTML = `<div class="empty">${esc(e.message)}</div>`;
  }
}

let IMG_POLLING = false;
async function pollImageTask(force) {
  if (IMG_POLLING && !force) return;
  IMG_POLLING = true;
  const box = $('#img-task');
  try {
    while (S.page === 'images') {
      // 镜像任务与运行时任务二选一展示（镜像优先）
      let t = await apiGet('/api/image/task');
      let isRuntime = false;
      if (!t.running && !t.done && !t.error) {
        t = await apiGet('/api/runtime/task');
        isRuntime = true;
      }
      if (!t.running && !t.done && !t.error) break;
      if (box) {
        const pct = t.progress_total ? Math.round(t.progress * 100 / t.progress_total) : 0;
        box.innerHTML = `
          <div class="card task-card">
            <h3>${t.running ? '<span class="spin">⏳</span>' : (t.error ? '⚠' : '✅')} ${esc(t.phase || '任务')}
              <span class="spacer"></span>
              ${t.running ? `<button class="btn sm danger" id="it-cancel">取消</button>`
                          : '<button class="btn sm" id="it-ack">知道了</button>'}
            </h3>
            <div class="progress-line"><span>${esc((t.lines || []).slice(-1)[0] || '')}</span>
              <span>${t.progress_total ? pct + '%（' + fmtBytes(t.progress) + ' / ' + fmtBytes(t.progress_total) + '）' : ''}</span></div>
            ${t.progress_total ? `<div class="bar"><i style="width:${pct}%"></i></div>` : ''}
            ${t.error ? `<div class="small" style="color:var(--red);margin-top:8px;">${esc(t.error)}</div>` : ''}
          </div>`;
        const cancel = $('#it-cancel');
        if (cancel) cancel.onclick = () =>
          apiPost(isRuntime ? '/api/runtime/cancel' : '/api/image/cancel').catch(() => {});
        const ack = $('#it-ack');
        if (ack) ack.onclick = async () => {
          if (!isRuntime) await apiPost('/api/image/task/ack').catch(() => {});
          box.innerHTML = ''; IMG_POLLING = false;
          loadLocalImages(); loadRuntimeStatus();
        };
      }
      if (!t.running) {
        IMG_POLLING = false;
        if (t.done) { toast('任务完成'); loadLocalImages(); loadRuntimeStatus(); }
        else if (t.error) toast('任务失败：' + t.error, 'err', 5000);
        break;
      }
      await new Promise(r => setTimeout(r, 1500));
    }
  } catch { IMG_POLLING = false; }
}

// ===================== 终端 =====================
async function renderTerminal(view) {
  view.innerHTML = `
    <div class="card">
      <div class="toolbar">
        <button class="btn primary" id="tm-open">打开会话</button>
        <button class="btn" id="tm-ctrlc" disabled>Ctrl-C</button>
        <button class="btn" id="tm-clear">清屏</button>
        <button class="btn danger" id="tm-close" disabled>关闭会话</button>
        <span class="spacer"></span>
        <span class="small muted" id="tm-state">未连接</span>
      </div>
      <div class="term-box" id="tm-box" style="margin-top:12px;">点击「打开会话」连接设备 adb shell。</div>
      <div class="term-input-row">
        <input class="form-input" id="tm-input" placeholder="输入命令回车执行（logcat、top 等长驻命令实时输出）" disabled>
        <button class="btn primary" id="tm-send" disabled>发送</button>
      </div>
    </div>`;
  $('#tm-open').onclick = termOpen;
  $('#tm-clear').onclick = () => { $('#tm-box').innerHTML = ''; };
  $('#tm-close').onclick = termClose;
  $('#tm-ctrlc').onclick = () => termWrite('\x03');
  $('#tm-send').onclick = termSend;
  $('#tm-input').onkeydown = e => { if (e.key === 'Enter') termSend(); };
}

function termSetUi(open, label) {
  $('#tm-open').disabled = open;
  $('#tm-close').disabled = !open;
  $('#tm-ctrlc').disabled = !open;
  $('#tm-send').disabled = !open;
  $('#tm-input').disabled = !open;
  $('#tm-state').textContent = label;
  if (open) $('#tm-input').focus();
}

async function termOpen() {
  try {
    const r = await apiPost('/api/term/open');
    S.term = { id: r.id, cursor: 0, alive: true };
    $('#tm-box').innerHTML = '';
    termSetUi(true, `会话 #${r.id} · ${r.serial}${r.pty ? ' · pty' : ''}`);
    addPoll(termRead, 900);
  } catch (e) { toast(e.message, 'err'); }
}

async function termRead() {
  if (!S.term) return;
  try {
    const r = await apiGet(`/api/term/${S.term.id}/read?cursor=${S.term.cursor}`);
    S.term.cursor = r.cursor;
    if (r.data) {
      const box = $('#tm-box');
      if (box) {
        box.appendChild(document.createTextNode(r.data));
        box.scrollTop = box.scrollHeight;
      }
    }
    if (!r.alive) {
      termSetUi(false, '会话已断开');
      S.term = null;
      clearPolls();
    }
  } catch { /* 会话失效 */ }
}

async function termWrite(data) {
  if (!S.term) return;
  try { await apiPost(`/api/term/${S.term.id}/write`, { data }); }
  catch (e) { toast(e.message, 'err'); }
}

function termSend() {
  const inp = $('#tm-input');
  const cmd = inp.value;
  if (!cmd) return;
  inp.value = '';
  termWrite(cmd + '\n');
}

async function termClose() {
  if (!S.term) return;
  const id = S.term.id;
  S.term = null;
  clearPolls();
  termSetUi(false, '未连接');
  try { await apiPost(`/api/term/${id}/close`); } catch {}
}

// ===================== 设置 =====================
const CFG_FIELDS = [
  { key: 'mode', label: '设备形态', type: 'select', options: [['phone', '手机 1080×2400'], ['tablet', '平板 2560×1600'], ['custom', '自定义']] },
  { key: 'width', label: '自定义宽', type: 'number' },
  { key: 'height', label: '自定义高', type: 'number' },
  { key: 'dpi', label: '自定义 DPI', type: 'number' },
  { key: 'memory_mb', label: '内存 MB（0=默认）', type: 'number' },
  { key: 'cpu_cores', label: 'CPU 核数（0=默认 4）', type: 'number' },
  { key: 'gpu_mode', label: 'GPU 后端', type: 'select', options: [['host', 'host（硬件加速）'], ['swiftshader_indirect', 'swiftshader（软渲兼容）'], ['swangle', 'swangle']] },
  { key: 'vsync_rate', label: '刷新率 Hz（0=默认 60）', type: 'number' },
  { key: 'proxy', label: 'HTTP 代理（如 http://192.168.1.2:7890）', type: 'text' },
  { key: 'dns_servers', label: '自定义 DNS（逗号分隔）', type: 'text' },
  { key: 'share_dir', label: '宿主共享目录（9p 挂载到 /mnt/host）', type: 'text' },
  { key: 'sdcard_path', label: 'sdcard 镜像路径（空=AVD 默认）', type: 'text' },
  { key: 'idle_stop_min', label: '空闲自动关停（分钟，0=从不）', type: 'number' },
  { key: 'locale', label: '语言覆盖（如 zh-CN；空=跟随宿主）', type: 'text' },
  { key: 'no_snapshot', label: '禁用快照（每次冷启动）', type: 'bool' },
  { key: 'no_boot_anim', label: '禁用启动动画', type: 'bool' },
  { key: 'hw_keyboard', label: '宿主键盘直通', type: 'bool' },
  { key: 'allow_host_audio', label: '允许使用宿主麦克风', type: 'bool' },
];

async function renderSettings(view) {
  view.innerHTML = `
    <div class="card">
      <h3>虚拟机配置</h3>
      <div class="form-grid" id="cfg-grid"><div class="empty">读取中…</div></div>
      <div class="btn-row" style="margin-top:16px;">
        <button class="btn primary" id="cfg-save">保存配置</button>
        <span class="small muted" style="align-self:center;">屏幕 / 内存 / GPU 修改后需重启虚拟机生效</span>
      </div>
    </div>
    <div class="card">
      <h3>存档点（快照）</h3>
      <div class="toolbar" style="margin-bottom:10px;">
        <input class="form-input" id="snap-name" placeholder="存档名（字母/数字/_/-）">
        <button class="btn sm primary" id="snap-save">保存当前状态</button>
      </div>
      <div id="snap-list"><div class="empty">读取中…</div></div>
    </div>
    <div class="card">
      <h3>数据盘</h3>
      <div id="disk-info" class="small muted">读取中…</div>
    </div>
    <div class="card danger-zone">
      <h3>危险操作</h3>
      <div class="btn-row">
        <button class="btn" id="dz-identity">重置设备身份</button>
        <button class="btn danger" id="dz-reset">重置虚拟机（清空全部数据）</button>
      </div>
      <div class="small muted" style="margin-top:8px;">重置设备身份：更换 android_id / 广告 ID，应用视作全新设备。</div>
    </div>`;

  let cfg = {};
  try { cfg = await apiGet('/api/config'); } catch (e) { toast(e.message, 'err'); }
  const grid = $('#cfg-grid');
  grid.innerHTML = CFG_FIELDS.map(f => {
    if (f.type === 'select') {
      return `<div class="form-item"><label>${f.label}</label>
        <select class="form-select" data-key="${f.key}">
          ${f.options.map(([v, t]) => `<option value="${v}">${t}</option>`).join('')}
        </select></div>`;
    }
    if (f.type === 'bool') {
      return `<div class="form-item"><label>&nbsp;</label>
        <span class="form-check"><input type="checkbox" data-key="${f.key}" id="ck-${f.key}">
        <label for="ck-${f.key}" style="color:var(--text);">${f.label}</label></span></div>`;
    }
    return `<div class="form-item"><label>${f.label}</label>
      <input class="form-input" type="${f.type}" data-key="${f.key}"></div>`;
  }).join('');
  $$('[data-key]', grid).forEach(inp => {
    const k = inp.dataset.key;
    const v = cfg[k];
    if (inp.type === 'checkbox') inp.checked = !!v;
    else if (v !== undefined && v !== null) inp.value = v;
  });
  $('#cfg-save').onclick = async () => {
    const out = { ...cfg };
    $$('[data-key]', grid).forEach(inp => {
      const k = inp.dataset.key;
      if (inp.type === 'checkbox') out[k] = inp.checked;
      else if (inp.type === 'number') out[k] = parseInt(inp.value || '0', 10) || 0;
      else out[k] = inp.value;
    });
    try {
      await apiPost('/api/config', out);
      toast('配置已保存');
    } catch (e) { toast(e.message, 'err', 4500); }
  };

  // 快照
  const loadSnaps = async () => {
    const box = $('#snap-list');
    try {
      const d = await apiGet('/api/snapshots');
      const ss = d.snapshots || [];
      if (!ss.length) { box.innerHTML = '<div class="empty">无存档点</div>'; return; }
      box.innerHTML = ss.map(s => `
        <div class="ver-row">
          <div style="flex:1;">${esc(s.name)} <span class="small muted">${s.mtime_ms ? new Date(s.mtime_ms).toLocaleString() : ''}</span></div>
          <button class="btn sm" data-act="load" data-n="${esc(s.name)}">恢复</button>
          <button class="btn sm danger" data-act="del" data-n="${esc(s.name)}">删除</button>
        </div>`).join('');
      $$('[data-act]', box).forEach(btn => {
        btn.onclick = async () => {
          const n = btn.dataset.n;
          if (btn.dataset.act === 'load') {
            if (!confirm(`恢复到存档「${n}」？当前运行状态将被替换。`)) return;
            try { await apiPost('/api/snapshot/load', { name: n }); toast('已恢复存档'); }
            catch (e) { toast(e.message, 'err'); }
          } else {
            if (!confirm(`删除存档「${n}」？`)) return;
            try { await apiPost('/api/snapshot/delete', { name: n }); toast('已删除'); loadSnaps(); }
            catch (e) { toast(e.message, 'err'); }
          }
        };
      });
    } catch (e) { box.innerHTML = `<div class="empty">${esc(e.message)}</div>`; }
  };
  $('#snap-save').onclick = async () => {
    const name = $('#snap-name').value.trim();
    if (!name) return toast('请输入存档名', 'err');
    try { await apiPost('/api/snapshot/save', { name }); toast('存档已保存'); $('#snap-name').value = ''; loadSnaps(); }
    catch (e) { toast(e.message, 'err'); }
  };
  loadSnaps();

  // 数据盘
  (async () => {
    try {
      const d = await apiGet('/api/vm/disk');
      $('#disk-info').innerHTML = `数据盘容量：<b>${d.size_gb || (d.size_bytes ? Math.round(d.size_bytes / (1 << 30)) : '?')} GB</b>
        <span class="muted">（扩容需在 Mac 客户端操作或联系管理员）</span>`;
    } catch { $('#disk-info').textContent = '虚拟机尚未初始化'; }
  })();

  $('#dz-identity').onclick = async () => {
    if (!confirm('重置设备身份（android_id / 广告 ID 换新）？')) return;
    try { await apiPost('/api/identity/reset'); toast('设备身份已重置'); }
    catch (e) { toast(e.message, 'err'); }
  };
  $('#dz-reset').onclick = async () => {
    if (!confirm('⚠ 确定重置虚拟机？全部应用、数据、快照都会被清空，不可恢复！')) return;
    if (!confirm('再次确认：真的要清空这台虚拟机的所有数据吗？')) return;
    try { await apiPost('/api/vm/reset'); toast('虚拟机已重置'); }
    catch (e) { toast(e.message, 'err'); }
  };
}

// ===================== 启动 =====================
$('#login-btn').onclick = async () => {
  const t = $('#login-token').value.trim();
  if (!t) return;
  saveToken(t);
  try {
    await apiGet('/health');
    hideLogin();
    refreshTopbar();
    renderPage(S.page);
  } catch (e) {
    saveToken('');
    $('#login-tip').textContent = '令牌错误，请重试';
  }
};
$('#login-token').addEventListener('keydown', e => { if (e.key === 'Enter') $('#login-btn').click(); });
$('#btn-logout').onclick = () => { saveToken(''); showLogin('请输入访问令牌'); };

(async function boot() {
  try {
    await apiGet('/health');   // 无令牌时（服务器未设 MWINE_TOKEN）直接通过
    hideLogin();
  } catch (e) {
    if (e.message === '未授权') showLogin();
    else { setConn(false); showLogin('无法连接服务器，请确认 mwine-cored 已启动'); }
  }
  refreshTopbar();
  renderPage(location.hash.slice(1) || 'overview');
})();
