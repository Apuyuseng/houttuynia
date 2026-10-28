<template>
  <!-- GNOME Preferences 样式：分组标题 + 盒式行（AdwPreferencesPage 观感） -->
  <div class="adw-pref-page">
    <section class="adw-group">
      <h3 class="adw-group-title">常规</h3>
      <div class="adw-boxed">
        <div class="adw-row">
          <div class="adw-row-icon"><MsIcon name="nav_phone" :size="17" /></div>
          <div class="adw-row-main">
            <div class="adw-row-title">当前设备</div>
            <div class="adw-row-sub mono" v-if="deviceSerial">{{ deviceSerial }}</div>
            <div class="adw-row-sub" v-else>未连接</div>
          </div>
          <el-button v-if="!deviceSerial" size="small" @click="openBiz('devices')">前往连接</el-button>
        </div>
        <div class="adw-row">
          <div class="adw-row-icon"><MsIcon name="key" :size="17" /></div>
          <div class="adw-row-main">
            <div class="adw-row-title">访问令牌</div>
            <div class="adw-row-sub">由服务端 MWINE_TOKEN 环境变量控制</div>
          </div>
          <el-button size="small" @click="needLogin = true">更换…</el-button>
        </div>
      </div>
    </section>

    <section class="adw-group">
      <h3 class="adw-group-title">关于</h3>
      <div class="adw-boxed">
        <div class="adw-row">
          <div class="adw-row-icon"><MsIcon name="nav_settings" :size="17" /></div>
          <div class="adw-row-main">
            <div class="adw-row-title">后端</div>
            <div class="adw-row-sub">Rust 网关（mwine-nas）{{ version ? ' · v' + version : '' }}</div>
          </div>
        </div>
        <div class="adw-row">
          <div class="adw-row-icon"><MsIcon name="nav_terminal" :size="17" /></div>
          <div class="adw-row-main">
            <div class="adw-row-title">能力</div>
            <div class="adw-row-sub">投屏、应用库、ADB 终端与文件互传；无内置虚拟机</div>
          </div>
        </div>
      </div>
      <p class="adw-group-desc" style="margin-top:10px">
        NAS 版为纯真机网关：管理局域网 / USB 连接的 Android 设备（手机、电视、盒子等）。
      </p>
    </section>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, inject } from 'vue'
import { apiGet, authState } from '../api'
import MsIcon from '../components/MsIcon.vue'
import { openBiz } from '../windows'

const { deviceSerial } = inject('vmState')
const version = ref('')
const needLogin = computed({
  get: () => authState.needLogin,
  set: (v) => { authState.needLogin = v },
})

onMounted(async () => {
  try {
    const st = await apiGet('/api/status')
    version.value = st.version || ''
  } catch {}
})
</script>
