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
        <span class="sb-brand">鱼腥草</span>
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
        <span class="si" :class="{ warn: !connected }"
              :title="connected ? '网关已连接' : '网关离线'">
          <MsIcon :name="connected ? 'biz_wifi' : 'biz_wifi_off'" :size="14" />
        </span>
        <span class="sb-clock">{{ clock }}</span>
      </div>
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
import { apiGet, authState, saveToken, imgUrl } from './api'
import MsIcon from './components/MsIcon.vue'
import Desktop from './components/Desktop.vue'
import { topWin, showDesktop } from './windows'

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

/* 状态栏指标：扁平图标+数字，仅异常态着色 */
.si { display: inline-flex; align-items: center; gap: 4px; color: inherit; }
.si .v { font-variant-numeric: tabular-nums; font-weight: 500; font-size: 12px; }
.si.warn { color: #f28b82; }
</style>
