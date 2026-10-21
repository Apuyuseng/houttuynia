<template>
  <el-container style="height: 100vh">
    <el-aside width="208px" class="side">
      <div class="brand">
        <span class="brand-logo">🌿</span>
        <div>
          <div class="brand-name">鱼腥草</div>
          <div class="brand-sub">NAS 控制台</div>
        </div>
      </div>
      <el-menu :default-active="route.path" router class="nav">
        <el-menu-item index="/overview"><el-icon><Monitor /></el-icon>概览</el-menu-item>
        <el-menu-item index="/apps"><el-icon><Grid /></el-icon>应用库</el-menu-item>
        <el-menu-item index="/devices"><el-icon><Iphone /></el-icon>真机</el-menu-item>
        <el-menu-item index="/store"><el-icon><Shop /></el-icon>在线商店</el-menu-item>
        <el-menu-item index="/images"><el-icon><Film /></el-icon>镜像</el-menu-item>
        <el-menu-item index="/terminal"><el-icon><TerminalIcon /></el-icon>终端</el-menu-item>
        <el-menu-item index="/settings"><el-icon><Setting /></el-icon>设置</el-menu-item>
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
      <el-header class="topbar">
        <h2 style="margin:0">{{ route.meta.title || '概览' }}</h2>
        <el-tag :type="vmRunning || deviceSerial ? 'success' : 'info'" effect="dark" size="large">
          {{ mode === 'device'
            ? (deviceSerial ? '真机 · ' + deviceSerial : '真机模式 · 未连接')
            : (vmRunning ? '运行中 · ' + (serial || '') : '未运行') }}
        </el-tag>
      </el-header>
      <el-main style="padding: 18px 24px 60px">
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
import { ref, computed, onMounted, onUnmounted, provide } from 'vue'
import { useRoute } from 'vue-router'
import { Monitor, Grid, Shop, Film, Setting, Iphone, Connection as TerminalIcon } from '@element-plus/icons-vue'
import { apiGet, authState, saveToken } from './api'

const route = useRoute()

const connected = ref(false)
const version = ref('')
const vmRunning = ref(false)
const serial = ref('')
const mode = ref('vm')
const deviceSerial = ref('')

// 全局状态共享给各页面（注入）
const vmState = { vmRunning, serial, connected, mode, deviceSerial }
provide('vmState', vmState)

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
    vmRunning.value = !!st.vm?.running
    serial.value = st.vm?.serial || ''
    mode.value = st.mode || 'vm'
    deviceSerial.value = st.device_serial || ''
    version.value = (st.version ? 'v' + st.version : '') + (st.backend === 'rust' ? ' · Rust' : '')
  } catch {
    connected.value = false
  }
}

let timer
onMounted(async () => {
  try { await apiGet('/health'); authState.needLogin = false }
  catch (e) { authState.needLogin = e.message === '未授权' }
  refresh()
  timer = setInterval(refresh, 4000)
})
onUnmounted(() => clearInterval(timer))
</script>

<style scoped>
.side {
  background: #161a22;
  border-right: 1px solid var(--el-border-color);
  display: flex; flex-direction: column;
}
.brand { display: flex; gap: 10px; align-items: center; padding: 20px 18px 14px; }
.brand-logo { font-size: 28px; }
.brand-name { font-size: 17px; font-weight: 700; letter-spacing: 1px; }
.brand-sub { font-size: 11px; color: var(--el-text-color-secondary); }
.nav { flex: 1; }
.side-foot { padding: 14px; display: grid; gap: 8px; justify-items: center; border-top: 1px solid var(--el-border-color); }
.topbar {
  display: flex; align-items: center; justify-content: space-between;
  border-bottom: 1px solid var(--el-border-color);
}
</style>
