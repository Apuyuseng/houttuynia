<template>
  <div class="app-root">
    <!-- 桌面背景：默认为 logo 壁纸（public/wallpaper-default.svg）；
         连上设备拿到 Android 壁纸后替换（截图兜底来源做高斯模糊氛围化） -->
    <div class="app-bg" :class="{ blur: wallpaperBlur }"
         :style="{ backgroundImage: `url('${wallpaperUrl}')` }"></div>

    <!-- Android 状态栏：透明浮于壁纸，左侧品牌，右侧设备指标 + 时间 -->
    <header class="statusbar" @pointerdown.self="showDesktop">
      <div class="sb-left" @click="showDesktop" title="回到桌面">
        <img class="sb-logo" src="/logo.svg" alt="Houttuynia" />
        <span class="sb-app muted" v-if="topWin">· {{ topWin.title }}</span>
      </div>
      <div class="sb-right">
        <template v-if="dev.model">
          <span class="si" v-if="dev.cpu_pct != null" :class="{ warn: dev.cpu_pct > 85 }"
                :title="'CPU ' + dev.cpu_pct + '%'">
            <MsIcon name="speed" :size="14" /><span class="v">{{ dev.cpu_pct }}%</span>
          </span>
          <span class="si" v-if="dev.mem" title="内存">
            <MsIcon name="memory" :size="14" /><span class="v">{{ fmtMem }}</span>
          </span>
          <span class="si" v-if="dev.battery?.present" title="电量"
                :class="{ warn: dev.battery.level <= 20 && dev.battery.status !== 'charging' }">
            <MsIcon :name="dev.battery.status === 'charging' ? 'battery_charging_full' : 'battery_5_bar'" :size="14" />
            <span class="v">{{ dev.battery.level }}%</span>
          </span>
          <span class="si" v-if="dev.battery?.present && dev.battery.temp_c != null" title="电池温度"
                :class="{ warn: dev.battery.temp_c > 40 }">
            <MsIcon name="device_thermostat" :size="14" /><span class="v">{{ dev.battery.temp_c.toFixed(1) }}°</span>
          </span>
          <span class="si" v-if="dev.uptime_s" title="设备运行时长">
            <MsIcon name="schedule" :size="14" /><span class="v">{{ fmtUptime }}</span>
          </span>
          <span class="si" :title="'设备 ' + dev.serial + '（' + dev.model + '）'">
            <MsIcon name="smartphone" :size="14" />
          </span>
        </template>
        <button class="si sb-bell" :class="{ has: unread > 0 }" :title="unread > 0 ? '有新通知' : '通知'"
                @click.stop="notifOpen = !notifOpen">
          <MsIcon name="notifications" :size="15" />
          <span class="bell-dot" v-if="unread > 0"></span>
        </button>
        <span class="sb-clock">{{ clock }}</span>
      </div>

      <!-- 通知中心（macOS 风格：右侧滑出卡片列表，按时间倒序） -->
      <transition name="nc">
        <div class="notif-center" v-if="notifOpen" @click.stop>
          <div class="nc-head">
            <span class="nc-title">通知</span>
            <button class="nc-clear" v-if="notifications.length" @click="clearNotifs">全部清除</button>
          </div>
          <div class="nc-list" v-if="notifGroups.length">
            <div class="nc-group" v-for="g in notifGroups" :key="g.pkg">
              <!-- 组头：应用图标 + 应用名 + 计数，点击折叠/展开 -->
              <div class="nc-item nc-group-head" @click="toggleGroup(g.pkg)">
                <div class="nc-ico" :class="{ fallback: !g.hasIcon }">
                  <img v-if="g.hasIcon" :src="iconUrl(g.pkg)" @error="iconFailed = { ...iconFailed, [g.pkg]: true }" alt="">
                  <span v-else class="nc-letter">{{ letterOf(g) }}</span>
                </div>
                <div class="nc-main">
                  <div class="nc-row1">
                    <span class="nc-title2">{{ g.appName || shortPkg(g.pkg) }}</span>
                    <span class="nc-time">{{ g.last.timeAgo }}</span>
                  </div>
                  <div class="nc-text">{{ g.last.text || (g.last.title ? '' : '新通知') }}</div>
                </div>
                <span class="nc-count" v-if="g.items.length > 1">{{ g.items.length }}</span>
                <MsIcon name="chevron_right" :size="16" class="nc-fold"
                        :class="{ open: !folded[g.pkg] }" />
              </div>
              <!-- 展开的组内通知（点击 = 打开应用） -->
              <template v-if="!folded[g.pkg]">
                <div class="nc-item nc-sub" v-for="n in g.items" :key="n.id"
                     @click="openApp(n.pkg)" :title="'在设备上打开 ' + (n.appName || n.pkg)">
                  <div class="nc-ico" :class="{ fallback: !g.hasIcon }" style="width:22px;height:22px">
                    <img v-if="g.hasIcon" :src="iconUrl(g.pkg)" @error="iconFailed = { ...iconFailed, [g.pkg]: true }" alt="">
                    <span v-else class="nc-letter sm">{{ letterOf(g) }}</span>
                  </div>
                  <div class="nc-main">
                    <div class="nc-row1">
                      <span class="nc-title2">{{ n.title || g.appName || shortPkg(g.pkg) }}</span>
                      <span class="nc-time">{{ n.timeAgo }}</span>
                    </div>
                    <div class="nc-text">{{ n.text || '（无内容）' }}</div>
                  </div>
                </div>
              </template>
            </div>
          </div>
          <div class="nc-empty" v-else>暂无通知</div>
        </div>
      </transition>
    </header>

    <!-- 桌面（应用网格 + 窗口层） -->
    <Desktop v-if="!needLogin" />

    <el-dialog v-model="needLogin" title="访问令牌" width="380px" :close-on-click-modal="false" :show-close="false">
      <p class="muted small" style="margin-top:0">服务器设置了 MWINE_TOKEN，请输入访问令牌（仅保存在本浏览器）</p>
      <el-input v-model="tokenInput" type="password" show-password placeholder="访问令牌"
                @keyup.enter="doLogin" autofocus />
      <template #footer>
        <el-button type="primary" style="width:100%" @click="doLogin" :loading="loginLoading">进入桌面</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, provide, watch } from 'vue'
import { apiGet, apiPost, authState, saveToken, imgUrl } from './api'
import MsIcon from './components/MsIcon.vue'
import Desktop from './components/Desktop.vue'
import { ElMessage, ElNotification } from 'element-plus'
import { topWin, showDesktop } from './windows'
import { onDeviceEvent } from './sseDevice'

const connected = ref(false)
const version = ref('')
const deviceSerial = ref('')
const dev = ref({})

// 全局状态共享给各页面（注入）；NAS 版纯真机，无虚拟机概念
const vmState = { connected, deviceSerial }
provide('vmState', vmState)

// ---- 桌面壁纸：默认 logo 壁纸，设备壁纸到达后替换；获取失败保持默认 ----
const DEFAULT_WALL = '/wallpaper-default.svg'
const wallpaperUrl = ref(DEFAULT_WALL)
const wallpaperBlur = ref(false)
async function loadWall() {
  try {
    const resp = await fetch(imgUrl('/api/wallpaper.png'))
    if (!resp.ok) { wallpaperUrl.value = DEFAULT_WALL; wallpaperBlur.value = false; return }
    const source = resp.headers.get('x-wallpaper-source') || ''
    const blob = await resp.blob()
    wallpaperBlur.value = source === 'screenshot'  // 截图兜底时模糊化为氛围壁纸
    wallpaperUrl.value = URL.createObjectURL(blob)
  } catch { wallpaperUrl.value = DEFAULT_WALL; wallpaperBlur.value = false }
}

// ---- 状态栏时钟 ----
const now = ref(new Date())
let clockTimer = null
const clock = computed(() => now.value.toLocaleTimeString('zh-CN', { hour: '2-digit', minute: '2-digit', hour12: false }))

function applyDev(st) {
  dev.value = {
    serial: st.device_serial || st.serial || '',
    model: st.model || '',
    cpu_pct: st.cpu_pct,
    mem: st.mem,
    battery: st.battery,
    uptime_s: st.uptime_s,
  }
}
const fmtMem = computed(() => {
  const m = dev.value.mem
  if (!m?.total_mb) return ''
  const used = m.total_mb - (m.available_mb || 0)
  const f = (mb) => mb >= 1024 ? (mb / 1024).toFixed(1) + 'G' : mb + 'M'
  return `${f(used)}/${f(m.total_mb)}`
})
const fmtUptime = computed(() => {
  const s = dev.value.uptime_s || 0
  const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60)
  return d > 0 ? `${d}天${h}时` : h > 0 ? `${h}时${m}分` : `${m}分`
})

// ---- 通知中心：SSE 设备通知 → 按应用分组卡片（最多 50 条，新在上） ----
const notifOpen = ref(false)
const notifications = ref([])
const unread = ref(0)
const folded = ref({})       // pkg → true 折叠
let notifSeq = 0
const appNameCache = ref({}) // pkg → 应用名（/api/apps 取）
const iconFailed = ref({})   // pkg → true（图标 404 后回退字母块）

const notifOff = onDeviceEvent('notification', (n) => {
  // 空通知（握手帧/无内容系统事件）直接忽略
  if (!n.title && !n.text && !n.pkg) return
  notifSeq += 1
  notifications.value.unshift({
    id: notifSeq,
    pkg: n.pkg || '',
    title: n.title || '',
    text: n.text || '',
    at: Date.now(),
    timeAgo: '刚刚',
  })
  if (notifications.value.length > 50) notifications.value.pop()
  if (!notifOpen.value) unread.value += 1
  if (n.pkg) ensureAppName(n.pkg)
})

// 按应用分组（组内时间倒序已由 unshift 保证）
const notifGroups = computed(() => {
  const map = new Map()
  for (const n of notifications.value) {
    if (!map.has(n.pkg)) map.set(n.pkg, [])
    map.get(n.pkg).push(n)
  }
  return [...map.entries()].map(([pkg, items]) => ({
    pkg,
    items,
    last: items[0],
    appName: appNameCache.value[pkg] || '',
    hasIcon: !iconFailed.value[pkg],
  }))
})
function toggleGroup(pkg) {
  folded.value = { ...folded.value, [pkg]: !folded.value[pkg] }
}
function iconUrl(pkg) {
  return `/api/apps/icon?pkg=${encodeURIComponent(pkg)}`
}
// 应用展示名（组头用）：无应用名时取包名尾段（com.android.shell → shell）
function shortPkg(pkg) {
  if (!pkg) return '设备通知'
  const parts = pkg.split('.')
  return parts[parts.length - 1] || pkg
}
// 字母块内容：应用名/包名尾段首字母大写
function letterOf(g) {
  const name = g.appName || shortPkg(g.pkg)
  return (name[0] || '?').toUpperCase()
}
async function ensureAppName(pkg) {
  if (appNameCache.value[pkg] !== undefined) return
  try {
    const d = await apiGet('/api/apps')
    const m = {}
    for (const a of d.apps || []) m[a.package] = a.name || a.package
    appNameCache.value = { ...m, ...(pkg in m ? {} : { [pkg]: pkg }) }
  } catch { appNameCache.value = { ...appNameCache.value, [pkg]: pkg } }
}
async function openApp(pkg) {
  try {
    await apiPost(`/api/apps/${encodeURIComponent(pkg)}/run`, {})
    ElMessage.success(`已在设备上打开 ${appNameCache.value[pkg] || pkg}`)
    notifOpen.value = false
  } catch (e) {
    ElMessage.error(e.message || '打开失败')
  }
}
// 打开面板：清未读；时间显示每 30s 刷新（刚刚/x分钟前）
watch(notifOpen, (v) => { if (v) unread.value = 0 })
setInterval(() => {
  const now = Date.now()
  for (const n of notifications.value) {
    const dt = Math.floor((now - n.at) / 1000)
    n.timeAgo = dt < 60 ? '刚刚' : dt < 3600 ? Math.floor(dt / 60) + ' 分钟前' : Math.floor(dt / 3600) + ' 小时前'
  }
}, 30000)
function clearNotifs() { notifications.value = []; unread.value = 0 }

const needLogin = computed({
  get: () => authState.needLogin,
  set: (v) => { authState.needLogin = v },
})
const tokenInput = ref('')
const loginLoading = ref(false)

async function doLogin() {
  if (!tokenInput.value.trim()) return
  loginLoading.value = true
  saveToken(tokenInput.value.trim())
  try {
    await apiGet('/health')
    authState.needLogin = false
    tokenInput.value = ''
    refresh()
  } catch {
    saveToken('')
  }
  loginLoading.value = false
}

async function refresh() {
  try {
    const st = await apiGet('/api/status')
    connected.value = true
    deviceSerial.value = st.device_serial || ''
    applyDev(st)
    version.value = st.version || ''
  } catch {
    connected.value = false
  }
}

let es = null
let fallbackTimer = null
function startStatusStream() {
  // SSE 状态流：变化即推（服务端 2s 巡检）；失败回退 4s 轮询
  try {
    es = new EventSource('/api/events/status?token=' + encodeURIComponent(authState.token))
    es.onmessage = (e) => {
      try {
        const st = JSON.parse(e.data)
        connected.value = true
        deviceSerial.value = st.device_serial || ''
        applyDev(st)
        version.value = st.version || ''
      } catch {}
    }
    es.onerror = () => {
      es && es.close()
      es = null
      connected.value = false
      if (!fallbackTimer) fallbackTimer = setInterval(refresh, 4000)
    }
  } catch {
    fallbackTimer = setInterval(refresh, 4000)
  }
}
onMounted(async () => {
  try { await apiGet('/health'); authState.needLogin = false }
  catch (e) { authState.needLogin = e.message === '未授权' }
  startStatusStream()
  loadWall()
  clockTimer = setInterval(() => { now.value = new Date() }, 5000)
})
watch(deviceSerial, () => { loadWall() })
onUnmounted(() => {
  if (es) es.close()
  if (fallbackTimer) clearInterval(fallbackTimer)
  if (clockTimer) clearInterval(clockTimer)
})
</script>

<style scoped>
.app-root {
  position: relative; height: 100vh; overflow: hidden;
  display: flex; flex-direction: column;
  background: #141218;
}
.app-bg {
  position: absolute; inset: 0; z-index: 0; pointer-events: none;
  background-size: cover; background-position: center;
}
.app-bg.blur { filter: blur(22px) brightness(0.85); transform: scale(1.08); }

/* Android 状态栏（平板：透明浮于壁纸上方） */
.statusbar {
  position: relative; z-index: 10;
  height: 36px; padding: 0 14px; flex-shrink: 0;
  display: flex; align-items: center; justify-content: space-between; gap: 12px;
  color: #fff;
  text-shadow: 0 1px 3px rgba(0, 0, 0, 0.6);
  user-select: none;
}
.sb-left { display: flex; align-items: center; gap: 8px; cursor: pointer; }
.sb-logo { width: 20px; height: 20px; }
.sb-brand { font-size: 13px; font-weight: 700; letter-spacing: 0.5px; }
.sb-app { font-size: 12px; }
.sb-right { display: flex; align-items: center; gap: 15px; overflow: hidden; white-space: nowrap; }
.sb-clock { font-size: 13px; font-weight: 600; font-variant-numeric: tabular-nums; }

/* 通知铃铛 + 未读蓝点（只表示"有新消息"，不显示具体数字；图标本身不换形不变色） */
.sb-bell {
  position: relative;
  background: none; border: none; cursor: pointer;
  color: rgba(255,255,255,0.85);
  width: 26px; height: 26px; border-radius: 50%;
  display: inline-flex; align-items: center; justify-content: center;
}
.sb-bell:hover { background: rgba(255,255,255,0.12); }
.sb-bell.has { color: #fff; }
.bell-dot {
  position: absolute; top: 5px; right: 5px;
  width: 7px; height: 7px; border-radius: 50%;
  background: #8ab4f8;
  /* 深色细描边把点从图标/壁纸上托起，外圈淡蓝光晕增强可见性 */
  box-shadow: 0 0 0 1.5px rgba(20, 22, 28, 0.65), 0 0 6px rgba(138, 180, 248, 0.7);
  animation: bell-dot-in 0.22s cubic-bezier(0.2, 0.9, 0.3, 1.4);
}
@keyframes bell-dot-in { from { transform: scale(0); } to { transform: scale(1); } }
/* macOS 风格通知中心：右上滑出，毛玻璃卡片列表 */
.notif-center {
  position: fixed; top: 30px; right: 8px; z-index: 3000;
  width: 340px; max-height: 70vh;
  display: flex; flex-direction: column;
  background: rgba(28, 28, 30, 0.72);
  backdrop-filter: blur(28px) saturate(160%);
  -webkit-backdrop-filter: blur(28px) saturate(160%);
  border: 1px solid rgba(255,255,255,0.14);
  border-radius: 18px;
  box-shadow: 0 18px 50px rgba(0,0,0,0.45);
  overflow: hidden;
}
.nc-enter-active { transition: all 0.25s cubic-bezier(0.2, 0.9, 0.3, 1); }
.nc-leave-active { transition: all 0.18s ease; }
.nc-enter-from, .nc-leave-to { opacity: 0; transform: translateY(-12px) scale(0.98); }
.nc-head {
  display: flex; align-items: center; justify-content: space-between;
  padding: 12px 16px 8px;
}
.nc-title { font-size: 14px; font-weight: 700; color: #fff; }
.nc-clear {
  background: none; border: none; cursor: pointer;
  color: #8ab4f8; font-size: 12px;
}
.nc-list { overflow-y: auto; padding: 0 8px 10px; }
.nc-item {
  display: flex; gap: 10px; align-items: flex-start;
  padding: 10px 12px; margin-bottom: 8px;
  background: rgba(255,255,255,0.09);
  border-radius: 14px;
}
.nc-item:hover { background: rgba(255,255,255,0.14); }
.nc-ico {
  width: 30px; height: 30px; border-radius: 9px;
  background: linear-gradient(160deg, #4f8ef7, #2f6fe0); color: #fff; flex-shrink: 0;
  display: inline-flex; align-items: center; justify-content: center;
  box-shadow: inset 0 0 0 0.5px rgba(255, 255, 255, 0.22), 0 1px 3px rgba(0, 0, 0, 0.3);
  overflow: hidden;
}
.nc-ico img { object-fit: contain; padding: 2px; box-sizing: border-box; }
.nc-main { flex: 1; min-width: 0; }
.nc-row1 { display: flex; align-items: baseline; gap: 8px; }
.nc-title2 { font-size: 13px; font-weight: 600; color: #fff; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.nc-time { margin-left: auto; font-size: 11px; color: rgba(255,255,255,0.45); flex-shrink: 0; }
.nc-text { font-size: 12px; color: rgba(255,255,255,0.75); margin-top: 2px; line-height: 1.45; word-break: break-word; }
.nc-empty { padding: 28px 0 34px; text-align: center; color: rgba(255,255,255,0.4); font-size: 13px; }
.nc-count {
  flex-shrink: 0; min-width: 18px; height: 18px; padding: 0 5px;
  background: rgba(138,180,248,0.25); color: #8ab4f8;
  font-size: 11px; font-weight: 700; line-height: 18px;
  border-radius: 999px; text-align: center;
}
.nc-fold { color: rgba(255,255,255,0.4); flex-shrink: 0; transition: transform 0.18s; }
.nc-fold.open { transform: rotate(90deg); }
.nc-group { margin-bottom: 8px; }
.nc-group-head { background: rgba(255,255,255,0.12); }
.nc-sub {
  margin: 4px 0 0 26px;
  background: rgba(255,255,255,0.06);
  cursor: pointer;
}
.nc-ico img { width: 100%; height: 100%; border-radius: inherit; }
.nc-ico.fallback { background: #5f6368; }
.nc-letter {
  font-size: 14px; font-weight: 700; color: #fff;
  display: inline-flex; align-items: center; justify-content: center;
  width: 100%; height: 100%;
}
.nc-letter.sm { font-size: 11px; }

/* 状态栏指标：扁平图标+数字，仅异常态着色 */
.si { display: inline-flex; align-items: center; gap: 4px; color: inherit; }
.si .v { font-variant-numeric: tabular-nums; font-weight: 500; font-size: 12px; }
.si.warn { color: #f28b82; }
</style>
