<template>
  <el-container class="app-root" :style="rootBg">
    <div class="app-veil"></div>
    <el-aside width="208px" class="side" style="position:relative; z-index:2">
      <div class="brand">
        <img class="brand-logo" src="/logo.svg" alt="Houttuynia" />
        <div>
          <div class="brand-name">鱼腥草</div>
        </div>
      </div>
      <el-menu :default-active="route.path" router class="nav">
        <el-menu-item index="/overview"><el-icon class="mi" style="color:#4ade80"><Monitor /></el-icon>概览</el-menu-item>
        <el-menu-item index="/apps"><el-icon class="mi" style="color:#60a5fa"><Grid /></el-icon>应用库</el-menu-item>
        <el-menu-item index="/devices"><el-icon class="mi" style="color:#f472b6"><Iphone /></el-icon>真机</el-menu-item>
        <el-menu-item index="/store"><el-icon class="mi" style="color:#fbbf24"><Shop /></el-icon>在线商店</el-menu-item>
        <el-menu-item index="/guard"><el-icon class="mi" style="color:#f87171"><Lock /></el-icon>家长管控</el-menu-item>
        <el-menu-item index="/files"><el-icon class="mi" style="color:#a78bfa"><Folder /></el-icon>文件</el-menu-item>
        <el-menu-item index="/terminal"><el-icon class="mi" style="color:#34d399"><TerminalIcon /></el-icon>终端</el-menu-item>
        <el-menu-item index="/settings"><el-icon class="mi" style="color:#94a3b8"><Setting /></el-icon>设置</el-menu-item>
      </el-menu>
      <div class="side-foot">
        <el-tag :type="connected ? 'success' : 'danger'" size="small" effect="dark" style="width: 100%">
          {{ connected ? '已连接' : '连接断开' }}
        </el-tag>
        <div class="muted small" style="text-align:center">{{ version }}</div>
        <el-link type="info" class="small" @click="needLogin = true">更换访问令牌</el-link>
      </div>
    </el-aside>

    <el-container>
      <el-header class="topbar" height="52px" style="position:relative; z-index:2">
        <h2 style="margin:0">{{ route.meta.title || '概览' }}</h2>
        <!-- 设备状态条：SSE 实时推送。Android SystemUI 风格——扁平
             图标+数字、无底色、右对齐、等宽数字，仅异常态着色 -->
        <div class="devbar" v-if="dev.model">
          <span class="si" :title="'设备 ' + dev.serial">
            <MsIcon name="smartphone" :size="13" /><span class="v">{{ dev.model }}</span>
          </span>
          <span class="si" v-if="dev.cpu_pct != null" :class="{ warn: dev.cpu_pct > 85 }"
                :title="'CPU ' + dev.cpu_pct + '%'">
            <MsIcon name="speed" :size="13" /><span class="v">{{ dev.cpu_pct }}%</span>
          </span>
          <span class="si" v-if="dev.mem" title="内存">
            <MsIcon name="memory" :size="13" /><span class="v">{{ fmtMem }}</span>
          </span>
          <span class="si" v-if="dev.battery?.present" title="电量"
                :class="{ warn: dev.battery.level <= 20 && dev.battery.status !== 'charging' }">
            <MsIcon :name="dev.battery.status === 'charging' ? 'battery_charging_full' : 'battery_5_bar'" :size="13" />
            <span class="v">{{ dev.battery.level }}%</span>
          </span>
          <span class="si" v-if="dev.battery?.present && dev.battery.temp_c != null" title="电池温度"
                :class="{ warn: dev.battery.temp_c > 40 }">
            <MsIcon name="device_thermostat" :size="13" /><span class="v">{{ dev.battery.temp_c.toFixed(1) }}°</span>
          </span>
          <span class="si" v-if="dev.uptime_s" title="设备运行时长">
            <MsIcon name="schedule" :size="13" /><span class="v">{{ fmtUptime }}</span>
          </span>
        </div>
      </el-header>
      <el-main class="main-area" style="position:relative; z-index:1">
        <router-view v-if="!needLogin" />
      </el-main>
    </el-container>

    <el-dialog v-model="needLogin" title="🔑 访问令牌" width="380px" :close-on-click-modal="false" :show-close="false">
      <p class="muted small" style="margin-top:0">服务器设置了 MWINE_TOKEN，请输入访问令牌（仅保存在本浏览器）</p>
      <el-input v-model="tokenInput" type="password" show-password placeholder="访问令牌"
                @keyup.enter="doLogin" autofocus />
      <template #footer>
        <el-button type="primary" style="width:100%" @click="doLogin" :loading="loginLoading">进入控制台</el-button>
      </template>
    </el-dialog>
  </el-container>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, provide, watch } from 'vue'
import { useRoute } from 'vue-router'
import { Monitor, Grid, Shop, Setting, Iphone, Lock, Folder, Connection as TerminalIcon } from '@element-plus/icons-vue'
import { apiGet, authState, saveToken, imgUrl } from './api'
import MsIcon from './components/MsIcon.vue'

const route = useRoute()

const connected = ref(false)
const version = ref('')
const deviceSerial = ref('')
const dev = ref({})

// 全局状态共享给各页面（注入）；NAS 版纯真机，无虚拟机概念
const vmState = { connected, deviceSerial }
provide('vmState', vmState)

// main 背景：当前设备的 Android 壁纸（/api/wallpaper.png；无设备/失败
// 回退默认纯色）。压暗叠层在 .main-veil，卡片本体不透明不受影响。
const wallUrl = ref('')
async function loadWall() {
  try {
    const resp = await fetch(imgUrl('/api/wallpaper.png'))
    wallUrl.value = resp.ok ? imgUrl('/api/wallpaper.png') : ''
  } catch { wallUrl.value = '' }
}
const rootBg = computed(() => wallUrl.value
  ? {
      backgroundImage: `url('${wallUrl.value}')`,
      backgroundSize: 'cover',
      backgroundPosition: 'center',
    }
  : {})

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
    version.value = (st.version ? 'v' + st.version : '') + (st.backend === 'rust' ? ' · Rust' : '')
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
        version.value = (st.version ? 'v' + st.version : '') + (st.backend === 'rust' ? ' · Rust' : '')
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
})
watch(deviceSerial, () => { loadWall() })
onUnmounted(() => {
  if (es) es.close()
  if (fallbackTimer) clearInterval(fallbackTimer)
})
</script>

<style scoped>
.side {
  /* Liquid Glass：亮面渐变玻璃体 + 顶部高光边（与卡片同语言，更厚重） */
  background: linear-gradient(180deg,
    rgba(255, 255, 255, 0.11) 0%,
    rgba(255, 255, 255, 0.055) 45%,
    rgba(150, 160, 185, 0.08) 100%);
  backdrop-filter: blur(28px) saturate(1.8);
  -webkit-backdrop-filter: blur(28px) saturate(1.8);
  border-right: 1px solid rgba(255, 255, 255, 0.12);
  box-shadow: inset 1px 0 0 rgba(255, 255, 255, 0.12);
  display: flex; flex-direction: column;
}
.brand { display: flex; gap: 10px; align-items: center; padding: 20px 18px 14px; }
.brand-logo { width: 30px; height: 30px; display: block; }
.brand-name { font-size: 17px; font-weight: 700; letter-spacing: 1px; }
.nav { flex: 1; }
.side-foot { padding: 14px; display: grid; gap: 8px; justify-items: center; border-top: 1px solid var(--el-border-color); }
.app-root { position: relative; height: 100vh; overflow: hidden; }
/* 壁纸压暗纱：一层铺满，sidebar/topbar/main 以玻璃或半透明材质浮在其上 */
.app-veil {
  position: absolute; inset: 0; pointer-events: none; z-index: 0;
  background: linear-gradient(180deg, rgba(10,12,18,.66), rgba(10,12,18,.78));
}
.side .mi { font-size: 19px; }
.topbar {
  display: flex; align-items: center; justify-content: space-between; gap: 12px;
  background: rgba(24, 28, 36, 0.5);
  backdrop-filter: blur(16px) saturate(1.4);
  -webkit-backdrop-filter: blur(16px) saturate(1.4);
  border-bottom: 1px solid rgba(255,255,255,.05);
}
/* 设备状态条：SSE 实时推送的紧凑指标 */
.devbar {
  display: flex; align-items: center; gap: 18px; flex: 1; justify-content: flex-end;
  margin-right: 4px;
  overflow: hidden; white-space: nowrap;
  color: var(--el-text-color-secondary);
  font-size: 12px;
  letter-spacing: 0.1px;
}
.si {
  display: inline-flex; align-items: center; gap: 5px;
  color: inherit;
}
.si .v {
  font-variant-numeric: tabular-nums;
  font-weight: 500;
}
.si.warn { color: var(--el-color-danger); }
/* main 区透明（壁纸由根容器统一提供），内容卡片不透明可读 */
</style>
