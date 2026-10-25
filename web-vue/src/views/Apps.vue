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
      <div v-if="loading" class="launcher-msg">读取中…</div>
      <div v-else-if="!running" class="launcher-msg">'真机未连接 —— 到「真机」页连接后此处展示设备桌面'</div>
      <el-empty v-else-if="!filtered.length" description="暂无应用" style="padding:60px 0" />
      <div v-else class="launcher-grid">
        <el-dropdown v-for="a in filtered" :key="a.package" trigger="contextmenu"
                     @command="(cmd) => onCmd(cmd, a)">
          <div class="launcher-item" @click="openWindow(a)">
            <div class="launcher-icon" :data-pkg="a.package">
              <img v-if="iconOk[a.package]" :src="imgUrl('/api/apps/icon?pkg=' + encodeURIComponent(a.package))"
                   loading="lazy" @error="iconOk[a.package] = false" alt="">
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

const { deviceSerial } = inject('vmState')
// NAS 版纯真机：目标在线与否看真机连接
const running = computed(() => !!deviceSerial.value)

const apps = ref([])
const loading = ref(true)      // 仅首载（无内容时）显占位
const refreshing = ref(false)  // 后台刷新标识（不清空现有内容）
const iconOk = ref({})
const task = ref({})
const taskVisible = ref(false)
const wallpaperUrl = ref('')
const wallpaperBlur = ref(false)

// 无图标的应用也要展示（字母占位块兜底），不能过滤——
// 真机模式下图标缓存可能整体为空，过滤会导致列表全空
const filtered = computed(() => apps.value)

const launcherStyle = computed(() => ({
  background: 'linear-gradient(155deg, #232a3b 0%, #10141c 60%, #0c0f16 100%)',
}))

const taskIcon = computed(() => task.value.running ? '⏳' : (task.value.error ? '⚠️' : '✅'))
const taskPct = computed(() =>
  task.value.progress_total ? Math.round(task.value.progress * 100 / task.value.progress_total) : 0)

function applyList(d) {
  // 只读成功响应；无图标的应用保留展示（字母块兜底）
  const list = d.apps || []
  apps.value = list
  list.forEach(a => {
    // 图标渲染不被 has_icon 门控：has_icon=false 只说明「缓存未落盘」，
    // 渲染 img 请求图标接口本身就会触发/等待后端提取（在途合并）——
    // 若按 has_icon 门控，全新环境永远是字母块（不请求→不提取死锁）。
    // @error 置 false 后靠 has_icon=true（提取落盘）恢复渲染；
    // 确认无图标（404）的包保持 false 不再重复请求。
    if (a.has_icon) iconOk.value[a.package] = true
    else if (iconOk.value[a.package] === undefined) iconOk.value[a.package] = true
  })
}

async function load() {
  // 有内容时转后台刷新标识，绝不清空
  if (apps.value.length) refreshing.value = true
  else loading.value = true
  try {
    const d = await apiGet('/api/apps')
    applyList(d)
  } catch (e) {
    ElMessage.error('应用列表读取失败：' + e.message + '（已保留现有内容）')
  }
  loading.value = false
  refreshing.value = false
  loadWallpaper()
}

// SSE 应用流：标签/图标落缓存后服务端主动推新列表，免轮询
let appsEs = null
function startAppsStream() {
  try {
    appsEs = new EventSource('/api/events/apps?token=' + encodeURIComponent(authState.token))
    appsEs.onmessage = (e) => {
      try {
        applyList(JSON.parse(e.data))
        loading.value = false
        refreshing.value = false
      } catch {}
    }
    appsEs.onerror = () => { /* 网络抖动由浏览器自动重连 */ }
  } catch {}
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

// 图标懒加载交给浏览器原生 loading="lazy"；手动 IntersectionObserver 方案
// 在页面被遮挡的环境（内嵌 webview 面板等）可能永不触发，导致图标永远不加载。
// 并发防护由后端提取槽 + 在途合并承担（详见 icons.rs）。

onMounted(() => {
  startAppsStream()   // 图标/标签落盘即推新列表（配合后端预热，图标渐出）
  load()
  setTimeout(load, 6000)
  setTimeout(load, 16000)
})
onUnmounted(() => {
  polling = false
  if (appsEs) appsEs.close()
})
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
  display: flex; flex-direction: column; gap: 12px;
  margin: -18px -24px -60px;
  padding: 0 0 20px;
  height: calc(100vh - 52px);
  box-sizing: border-box;
}
/* 任务卡在整页贴合下保持呼吸感 */
.apps-page > .el-card { margin: 18px 24px 0; }
/* launcher 吃满剩余高度 */
.apps-page > .launcher { flex: 1; }
.is-spin { animation: mwspin 1s linear infinite; }
@keyframes mwspin { to { transform: rotate(360deg); } }
</style>
