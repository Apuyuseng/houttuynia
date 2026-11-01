<!-- Android 平板桌面：业务应用与设备应用合并的启动器网格 + 底部任务栏 + 独立窗口层 -->
<template>
  <div class="desk" @pointerdown.self="showDesktop">
    <!-- 安装任务浮签（APK 上传/在线商店安装共用后端任务） -->
    <div class="desk-task" v-if="taskVisible">
      <div class="dt-head">
        <span>{{ taskIcon }} 安装 {{ task.label || task.package || '' }}</span>
        <div style="flex:1"></div>
        <el-button v-if="!task.running" size="small" text @click="taskVisible = false">知道了</el-button>
      </div>
      <div class="small muted" style="margin-bottom:6px;">{{ task.phase }}
        <span v-if="task.progress_total">（{{ fmtBytes(task.progress) }} / {{ fmtBytes(task.progress_total) }}）</span>
      </div>
      <el-progress v-if="task.progress_total" :percentage="taskPct" :stroke-width="6" />
      <el-alert v-if="task.error" :title="task.error" type="error" :closable="false" style="margin-top:8px;" />
    </div>

    <!-- 图标网格：业务应用在前，设备 Android 应用随后 -->
    <div class="desk-grid">
      <div v-for="b in visibleBizApps(running)" :key="b.id" class="desk-item"
           @click="b.special === 'mirror' ? openMirror(deviceSerial) : openBiz(b.id)">
        <div class="di-wrap">
          <div class="desk-icon" :style="{ background: b.tint }">
            <MsIcon :name="b.icon" :size="30" />
          </div>
          <!-- 业务应用角标：鱼腥草叶 logo（类比 Android 工作资料徽章），与设备应用区分 -->
          <img class="biz-badge" src="/logo.svg" alt="">
        </div>
        <div class="desk-name">{{ b.title }}</div>
      </div>
      <el-dropdown v-for="a in apps" :key="a.package" trigger="contextmenu"
                   @command="(cmd) => onCmd(cmd, a)">
        <div class="desk-item" @click="openAndroid(a)">
          <div class="desk-icon and">
            <img v-if="iconOk[a.package]" :src="imgUrl('/api/apps/icon?pkg=' + encodeURIComponent(a.package))"
                 loading="lazy" @error="iconOk[a.package] = false" alt="">
            <template v-else>{{ (a.name || a.package).slice(0, 1).toUpperCase() }}</template>
          </div>
          <div class="desk-name">{{ a.name }}</div>
        </div>
        <template #dropdown>
          <el-dropdown-menu>
            <el-dropdown-item command="run">主屏启动</el-dropdown-item>
            <el-dropdown-item command="uninstall" divided>卸载（清除数据）</el-dropdown-item>
          </el-dropdown-menu>
        </template>
      </el-dropdown>
    </div>
    <div v-if="loading" class="desk-hint">读取中…</div>
    <div v-else-if="!running" class="desk-hint">
      真机未连接 —— 打开「设置 → 真机连接」接入设备（USB / 无线 / 扫码），连接后应用会出现在桌面上
    </div>

    <!-- 任务栏：已打开窗口（Android 平板 taskbar 观感） -->
    <div class="taskbar" v-if="winState.list.length">
      <button v-for="w in winState.list" :key="w.key" class="tb-item"
              :class="{ live: !w.minimized }" :title="w.title" @click="frontWin(w.key)">
        <span v-if="w.kind === 'biz' || w.kind === 'mirror'" class="tb-ico" :style="{ background: bizMeta(w.id).tint }">
          <MsIcon :name="bizMeta(w.id).icon" :size="17" />
          <img class="biz-badge sm" src="/logo.svg" alt="">
        </span>
        <span v-else class="tb-ico and">
          <img :src="imgUrl('/api/apps/icon?pkg=' + encodeURIComponent(w.app.package))" alt=""
               @error="$event.target.style.display = 'none'">
        </span>
      </button>
    </div>

    <!-- 独立窗口层 -->
    <template v-for="w in winState.list" :key="w.key">
      <BizWindow v-if="w.kind === 'biz'" v-show="!w.minimized" :win="w" :offset="w.key"
                 @close="closeWin" @front="frontWin" @minimize="minWin" />
      <AppWindow v-else v-show="!w.minimized" :app="w.app" :offset="w.key" :z="w.z"
                 @close="closeWin(w.key)" @front="frontWin(w.key)" @minimize="minWin(w.key)" />
    </template>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, inject } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { apiGet, apiPost, imgUrl, fmtBytes, authState } from '../api'
import { onDeviceEvent } from '../sseDevice'
import { getFlags } from '../flags'
import MsIcon from './MsIcon.vue'
import BizWindow from './BizWindow.vue'
import AppWindow from '../views/AppWindow.vue'
import {
  BIZ_APPS, bizMeta, visibleBizApps, winState,
  openBiz, openAndroid, openMirror, closeWin, frontWin, minWin, showDesktop,
} from '../windows'

const { deviceSerial } = inject('vmState')

// 设备通知 → 桌面 toast（桥 APK 实时事件，零轮询）
onDeviceEvent('notification', (n) => {
  if (!getFlags().notify_toast) return
  ElNotification({
    title: n.title || (n.pkg ? '通知 · ' + n.pkg : '设备通知'),
    message: n.text || '',
    type: 'info',
    duration: 6000,
  })
})
const running = computed(() => !!deviceSerial.value)

// ---- 设备应用列表（SSE 推送 + 轮询兜底，与原应用库一致） ----
const apps = ref([])
const loading = ref(true)
const refreshing = ref(false)
const iconOk = ref({})
const task = ref({})
const taskVisible = ref(false)

const taskIcon = computed(() => task.value.running ? '⏳' : (task.value.error ? '⚠️' : '✅'))
const taskPct = computed(() =>
  task.value.progress_total ? Math.round(task.value.progress * 100 / task.value.progress_total) : 0)

function applyList(d) {
  const list = d.apps || []
  apps.value = list
  list.forEach(a => {
    if (a.has_icon) iconOk.value[a.package] = true
    else if (iconOk.value[a.package] === undefined) iconOk.value[a.package] = true
  })
}

async function load() {
  if (apps.value.length) refreshing.value = true
  else loading.value = true
  try {
    const d = await apiGet('/api/apps')
    applyList(d)
  } catch { /* 设备离线等，静默 */ }
  loading.value = false
  refreshing.value = false
}

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

async function run(a) {
  try { await apiPost(`/api/apps/${encodeURIComponent(a.package)}/run`); ElMessage.success('已下发启动指令') }
  catch (e) { ElMessage.error(e.message) }
}

async function onCmd(cmd, a) {
  if (cmd === 'run') return run(a)
  if (cmd === 'uninstall') {
    try {
      await ElMessageBox.confirm(`确定卸载 ${a.name}（${a.package}）？应用数据将被清除。`, '卸载应用',
        { type: 'warning', confirmButtonText: '卸载' })
    } catch { return }
    try {
      const r = await apiPost(`/api/apps/${encodeURIComponent(a.package)}/uninstall`)
      if (r.ok) { ElMessage.success('已卸载'); setTimeout(load, 1200) }
      else ElMessage.error('卸载失败：' + (r.output || ''))
    } catch (e) { ElMessage.error(e.message) }
  }
}

// ---- 安装任务跟踪（后端任务存在期间浮签可见） ----
let polling = false
async function pollTask() {
  if (polling) return
  polling = true
  try {
    while (true) {
      const t = await apiGet('/api/apps/install/task')
      if (t.running || t.done || t.error) taskVisible.value = true
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

onMounted(() => {
  startAppsStream()
  load()
  pollTask()
  setTimeout(load, 6000)
  setTimeout(load, 16000)
})
onUnmounted(() => {
  polling = false
  if (appsEs) appsEs.close()
})
</script>

<style scoped>
.desk {
  position: relative; flex: 1; overflow-y: auto;
  padding: 20px 28px 96px;
  align-content: start;
}
.desk-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(88px, 1fr));
  gap: 20px 12px;
  max-width: 980px; margin: 0 auto;
}
.desk-item {
  display: flex; flex-direction: column; align-items: center; gap: 7px;
  padding: 10px 4px; border-radius: 16px;
  cursor: pointer; user-select: none;
  transition: background .15s;
}
.desk-item:hover { background: rgba(255, 255, 255, 0.08); }
.desk-item:active { transform: scale(0.94); }
/* Android 应用图标：大圆角 squircle，文字带阴影压在壁纸上可读 */
/* 图标底托：现代启动器观感——squircle 圆角 + 玻璃底 + 双层阴影 + 按压反馈 */
.desk-icon {
  width: 58px; height: 58px; border-radius: 16px;
  display: flex; align-items: center; justify-content: center;
  color: #fff; font-size: 24px; font-weight: 700;
  overflow: hidden;
  box-shadow:
    0 1px 2px rgba(0, 0, 0, 0.22),
    0 6px 18px rgba(0, 0, 0, 0.32);
  transition: transform 0.16s ease;
}
.desk-item:active .desk-icon { transform: scale(0.9); }
/* 设备应用：图标本体铺满整个圆角框（容器裁出 squircle 圆角）；
   玻璃底仅作为 legacy 透明图标/字母兜底的底色 */
.desk-icon.and {
  background: linear-gradient(160deg, rgba(255, 255, 255, 0.14), rgba(255, 255, 255, 0.05));
  color: rgba(255, 255, 255, 0.85);
}
.desk-icon.and img {
  width: 100%; height: 100%;
  object-fit: cover; display: block;
}
/* 业务应用 logo 角标（Android 工作资料徽章观感：深色圆底 + 细亮边） */
.di-wrap { position: relative; }
.biz-badge {
  position: absolute; right: -4px; bottom: -4px;
  width: 20px; height: 20px; padding: 3px; box-sizing: content-box;
  background: #1a1b1e; border: 1px solid rgba(255, 255, 255, 0.16);
  border-radius: 50%;
  box-shadow: 0 1px 4px rgba(0, 0, 0, 0.5);
  pointer-events: none;
}
.biz-badge.sm {
  width: 12px; height: 12px; padding: 2px;
  right: -3px; bottom: -3px;
}
.desk-name {
  font-size: 12px; color: #fff; max-width: 84px;
  overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
  text-shadow: 0 1px 3px rgba(0, 0, 0, 0.7);
}
.desk-hint {
  position: fixed; left: 0; right: 0; bottom: 88px;
  text-align: center;
  color: rgba(255, 255, 255, 0.75); font-size: 13px;
  text-shadow: 0 1px 3px rgba(0, 0, 0, 0.7);
  pointer-events: none;
}
/* 安装任务浮签 */
.desk-task {
  position: fixed; top: 48px; right: 20px; z-index: 60;
  width: 300px; padding: 12px 14px;
  background: #28292d; border: 1px solid rgba(255, 255, 255, 0.1);
  border-radius: 16px; box-shadow: 0 8px 28px rgba(0, 0, 0, 0.5);
}
.dt-head { display: flex; align-items: center; gap: 8px; font-size: 13px; font-weight: 600; margin-bottom: 4px; }
/* 底部任务栏 */
.taskbar {
  position: fixed; left: 50%; transform: translateX(-50%);
  bottom: 14px; z-index: 50;
  display: flex; gap: 4px; padding: 6px 10px;
  background: rgba(30, 30, 34, 0.88);
  backdrop-filter: blur(16px);
  -webkit-backdrop-filter: blur(16px);
  border: 1px solid rgba(255, 255, 255, 0.1);
  border-radius: 22px;
  box-shadow: 0 8px 28px rgba(0, 0, 0, 0.45);
}
.tb-item {
  background: none; border: none; padding: 4px; cursor: pointer;
  border-radius: 12px; position: relative;
  display: inline-flex; align-items: center; justify-content: center;
}
.tb-item:hover { background: rgba(255, 255, 255, 0.1); }
.tb-ico {
  width: 34px; height: 34px; border-radius: 10px;
  display: flex; align-items: center; justify-content: center;
  color: #fff; overflow: visible; position: relative;
}
.tb-ico.and { overflow: hidden; }
.tb-ico.and img { width: 100%; height: 100%; object-fit: cover; display: block; }
.tb-item.live::after {
  content: ''; position: absolute; bottom: 0; left: 50%; transform: translateX(-50%);
  width: 4px; height: 4px; border-radius: 50%; background: #a8c7fa;
}
</style>
