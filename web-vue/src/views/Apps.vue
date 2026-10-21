<template>
  <div class="apps-page">
    <!-- 桌面上沿工具栏（贴合面板左右上三边）：搜索/上传/刷新 -->
    <!-- 结构移进 .launcher 内顶部 -->

    <el-card v-if="taskVisible" shadow="never">
      <template #header>
        <div style="display:flex;align-items:center;gap:8px;">
          <span>{{ taskIcon }} 安装 {{ task.label || task.package || '' }}</span>
          <div style="flex:1"></div>
          <el-button v-if="!task.running" size="small" @click="taskVisible = false">知道了</el-button>
        </div>
      </template>
      <div class="small muted" style="margin-bottom:6px;">{{ task.phase }}
        <span v-if="task.progress_total">（{{ fmtBytes(task.progress) }} / {{ fmtBytes(task.progress_total) }}）</span>
      </div>
      <el-progress v-if="task.progress_total" :percentage="taskPct" />
      <el-alert v-if="task.error" :title="task.error" type="error" :closable="false" style="margin-top:8px;" />
    </el-card>

    <!-- 启动器桌面：设备壁纸背景 + 图标网格 -->
    <div class="launcher" :style="launcherStyle">
      <img v-if="wallpaperUrl" class="launcher-bg" :class="{ blur: wallpaperBlur }" :src="wallpaperUrl" alt="">
      <div class="macbar">
        <div class="macbar-actions">
          <button class="macbar-btn" :class="{ active: searchOn }" title="搜索" @click="toggleSearch">
            <el-icon><Search /></el-icon>
          </button>
          <button class="macbar-btn" title="上传 APK 安装" @click="fileInput.click()">
            <el-icon><Upload /></el-icon>
          </button>
          <button class="macbar-btn" title="刷新列表" @click="load">
            <el-icon><Refresh /></el-icon>
          </button>
        </div>
        <input ref="fileInput" type="file" accept=".apk" style="display:none" @change="onFile" />
        <transition name="el-zoom-in-center">
          <input v-if="searchOn" v-model="filter" class="macbar-search"
                 placeholder="筛选应用名 / 包名" autofocus />
        </transition>
      </div>
      <div v-if="loading" class="launcher-msg">读取中…</div>
      <div v-else-if="!running" class="launcher-msg">虚拟机未运行 —— 启动后此处展示设备桌面</div>
      <el-empty v-else-if="!filtered.length" description="暂无应用" style="padding:60px 0" />
      <div v-else class="launcher-grid">
        <el-dropdown v-for="a in filtered" :key="a.package" trigger="contextmenu"
                     @command="(cmd) => onCmd(cmd, a)">
          <div class="launcher-item" @click="openWindow(a)">
            <div class="launcher-icon">
              <img v-if="iconOk[a.package]" :src="imgUrl('/api/apps/icon?pkg=' + encodeURIComponent(a.package))"
                   @error="iconOk[a.package] = false" alt="">
              <template v-else>{{ (a.name || a.package).slice(0, 1).toUpperCase() }}</template>
            </div>
            <div class="launcher-name">{{ a.name }}</div>
          </div>
          <template #dropdown>
            <el-dropdown-menu>
              <el-dropdown-item command="run">主屏启动</el-dropdown-item>
              <el-dropdown-item command="window">独立窗口打开</el-dropdown-item>
              <el-dropdown-item command="uninstall" divided>卸载（清除数据）</el-dropdown-item>
            </el-dropdown-menu>
          </template>
        </el-dropdown>
      </div>
    </div>

    <!-- 应用独立窗口层（scrcpy 副屏直通） -->
    <AppWindow v-for="(w, i) in windows" :key="w.key" :app="w.app" :offset="i"
               :z="w.z" @close="closeWindow(w.key)" @front="bringFront(w.key)" />
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { apiGet, apiPost, imgUrl, fmtBytes, authState } from '../api'
import { Search, Upload, Refresh } from '@element-plus/icons-vue'
import { inject } from 'vue'
import AppWindow from './AppWindow.vue'

const { vmRunning } = inject('vmState')
const running = vmRunning

const apps = ref([])
const loading = ref(true)
const filter = ref('')
const fileInput = ref(null)
const iconOk = ref({})
const task = ref({})
const taskVisible = ref(false)
const wallpaperUrl = ref('')
const wallpaperBlur = ref(false)
const searchOn = ref(false)
function toggleSearch() { searchOn.value = !searchOn.value; if (!searchOn.value) filter.value = '' }

const filtered = computed(() => {
  const q = filter.value.trim().toLowerCase()
  // 无图标应用不展示（图标 404 = 自适应 XML 等无可提取 PNG；后端已负缓存）
  let list = apps.value.filter(a => iconOk.value[a.package] !== false)
  if (q) {
    list = list.filter(a =>
      (a.name || '').toLowerCase().includes(q) || a.package.toLowerCase().includes(q))
  }
  return list
})

const launcherStyle = computed(() => ({
  background: 'linear-gradient(155deg, #232a3b 0%, #10141c 60%, #0c0f16 100%)',
}))

const taskIcon = computed(() => task.value.running ? '⏳' : (task.value.error ? '⚠️' : '✅'))
const taskPct = computed(() =>
  task.value.progress_total ? Math.round(task.value.progress * 100 / task.value.progress_total) : 0)

async function load() {
  loading.value = true
  try {
    const d = await apiGet('/api/apps')
    apps.value = d.apps || []
    apps.value.forEach(a => { iconOk.value[a.package] = true })
  } catch (e) {
    ElMessage.error(e.message)
  }
  loading.value = false
  loadWallpaper()
}

async function loadWallpaper() {
  // 壁纸就绪则换真壁纸；404 保持渐变兜底。
  // 来源为截图兜底（screenshot）时高斯模糊——视觉上即氛围壁纸
  try {
    const resp = await fetch(imgUrl('/api/wallpaper.png'))
    if (!resp.ok) return
    const source = resp.headers.get('x-wallpaper-source') || ''
    const blob = await resp.blob()
    wallpaperBlur.value = source === 'screenshot'
    wallpaperUrl.value = URL.createObjectURL(blob)
  } catch { /* 保持渐变 */ }
}

async function run(a) {
  try { await apiPost(`/api/apps/${encodeURIComponent(a.package)}/run`); ElMessage.success('已下发启动指令') }
  catch (e) { ElMessage.error(e.message) }
}

async function onCmd(cmd, a) {
  if (cmd === 'run') return run(a)
  if (cmd === 'window') return openWindow(a)
  if (cmd === 'uninstall') {
    try {
      await ElMessageBox.confirm(`确定卸载 ${a.name}（${a.package}）？应用数据将被清除。`, '卸载应用', { type: 'warning', confirmButtonText: '卸载' })
    } catch { return }
    try {
      const r = await apiPost(`/api/apps/${encodeURIComponent(a.package)}/uninstall`)
      if (r.ok) { ElMessage.success('已卸载'); setTimeout(load, 1200) }
      else ElMessage.error('卸载失败：' + (r.output || ''))
    } catch (e) { ElMessage.error(e.message) }
  }
}

// ---- 应用独立窗口（scrcpy 副屏直通，与桌面版「每应用独立窗口」同能力） ----
const windows = ref([])
let winSeq = 0
let zTop = 100

function openWindow(a) {
  // 同应用已在窗口中 → 提到最前，不重复开
  const exist = windows.value.find(w => w.app.package === a.package)
  if (exist) return bringFront(exist.key)
  winSeq += 1
  windows.value.push({ key: winSeq, app: a, z: ++zTop })
}

function closeWindow(key) {
  windows.value = windows.value.filter(w => w.key !== key)
}

function bringFront(key) {
  const w = windows.value.find(w => w.key === key)
  if (w) w.z = ++zTop
}

function onFile(e) {
  const f = e.target.files[0]
  e.target.value = ''
  if (!f) return
  if (f.size > 512 * 1024 * 1024) return ElMessage.error('APK 超过 512MB 上限')
  const xhr = new XMLHttpRequest()
  xhr.open('POST', '/api/apps/install-upload?name=' + encodeURIComponent(f.name))
  xhr.setRequestHeader('Content-Type', 'application/octet-stream')
  if (authState.token) xhr.setRequestHeader('Authorization', 'Bearer ' + authState.token)
  xhr.onload = () => {
    let d = {}
    try { d = JSON.parse(xhr.responseText) } catch {}
    if (xhr.status >= 200 && xhr.status < 300) {
      ElMessage.success('上传完成，开始安装')
      taskVisible.value = true
      pollTask()
    } else {
      ElMessage.error(d.error || '上传失败')
    }
  }
  xhr.onerror = () => ElMessage.error('上传失败：网络错误')
  xhr.send(f)
  taskVisible.value = true
  task.value = { running: true, phase: '上传到服务器…' }
  pollTask()
}

let polling = false
async function pollTask() {
  if (polling) return
  polling = true
  try {
    while (true) {
      const t = await apiGet('/api/apps/install/task')
      task.value = t
      if (!t.running) {
        polling = false
        if (t.done) { ElMessage.success('安装完成'); setTimeout(load, 1500) }
        else if (t.error) ElMessage.error('安装失败：' + t.error)
        return
      }
      await new Promise(r => setTimeout(r, 1200))
    }
  } catch { polling = false }
}

// 标签后台补抓：首次加载后延迟再拉两次，名称自动变真名
onMounted(() => {
  load()
  setTimeout(load, 6000)
  setTimeout(load, 16000)
})
onUnmounted(() => { polling = false })
</script>

<style scoped>
.launcher {
  /* 整页贴合主区，直角无边框 */
  border-radius: 0;
  border: none;
  min-height: 56vh;
  padding: 26px 30px;
  position: relative;
  overflow: hidden;
}
.launcher::before {
  content: '';
  position: absolute; inset: 0;
  background: linear-gradient(180deg, #0002, #0004);  /* 压暗壁纸，图标可读 */
  pointer-events: none;
}
.launcher-msg {
  position: relative;
  color: #fff;
  text-align: center;
  padding: 80px 0;
  text-shadow: 0 1px 4px #000a;
}
.launcher-grid {
  position: relative;
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(88px, 1fr));
  gap: 20px 12px;
  align-items: start;
}
.launcher-item {
  text-align: center;
  cursor: pointer;
  border-radius: 12px;
  padding: 8px 2px;
  transition: background .15s, transform .1s;
}
.launcher-item:hover {
  background: #ffffff24;
  transform: translateY(-2px);
}
.launcher-icon {
  width: 56px; height: 56px; border-radius: 14px; margin: 0 auto 6px;
  display: flex; align-items: center; justify-content: center;
  background: linear-gradient(135deg, #f8fafc, #cbd5e1);
  font-size: 24px; font-weight: 700; color: #475569;
  overflow: hidden;
  box-shadow: 0 2px 8px #0006;
}
.launcher-icon img { width: 100%; height: 100%; object-fit: cover; }
.launcher-name {
  color: #fff;
  font-size: 12px;
  text-shadow: 0 1px 3px #000c;
  overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
}
/* 桌面上沿工具栏：与 launcher 面板左右上三边贴合（负 margin 吃掉内边距） */
.macbar {
  position: relative;
  display: flex; align-items: center; justify-content: flex-end;
  height: 34px;
  margin: -26px -30px 18px;   /* 贴合面板顶/左/右，下缘与内容留 18px */
  background: rgba(20, 23, 30, 0.45);   /* 更高透明度，壁纸透出来 */
  backdrop-filter: blur(18px) saturate(1.3);
  -webkit-backdrop-filter: blur(18px) saturate(1.3);
  border-bottom: 1px solid #ffffff10;
  padding: 0 10px;
}
.macbar-actions { display: flex; gap: 6px; z-index: 1; }
.macbar-btn {
  border: none; background: transparent; color: #9aa3b2;
  width: 28px; height: 22px; border-radius: 6px; cursor: pointer;
  display: inline-flex; align-items: center; justify-content: center;
  font-size: 13px;
}
.macbar-btn:hover { background: #ffffff14; color: #e6e9ef; }
.macbar-btn.active { background: #4ade8033; color: #86efac; }
.macbar-search {
  position: absolute; right: 100px; top: 4px;
  z-index: 2;
  width: 220px; height: 26px;
  background: #14171e; border: 1px solid #3a4152; border-radius: 8px;
  color: #e6e9ef; padding: 0 10px; outline: none; font-size: 12.5px;
}
.macbar-search:focus { border-color: #4ade80; }

/* 壁纸背景图（截图兜底时模糊化） */
.launcher-bg {
  position: absolute; inset: 0;
  width: 100%; height: 100%; object-fit: cover;
}
.launcher-bg.blur {
  filter: blur(26px) brightness(.6) saturate(1.2);
  transform: scale(1.12);  /* 模糊边缘出血消除 */
}
/* 应用库整页贴合 el-main（抵消其内边距，仅本页生效） */
.apps-page {
  display: flex; flex-direction: column; gap: 18px;
  margin: -18px -24px -60px;
  min-height: calc(100vh - 60px - 60px); /* el-header 60px + el-main 原下边距 */
}
/* 任务卡在整页贴合下保持呼吸感 */
.apps-page > .el-card { margin: 18px 24px 0; }
/* launcher 吃满剩余高度 */
.apps-page > .launcher { flex: 1; }
</style>
