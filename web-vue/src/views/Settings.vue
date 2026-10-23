<template>
  <div style="display:grid;gap:18px;">
    <el-card shadow="never">
      <template #header>网关信息</template>
      <el-descriptions :column="1" border>
        <el-descriptions-item label="后端">Rust 网关（mwine-nas）{{ version ? ' · v' + version : '' }}</el-descriptions-item>
        <el-descriptions-item label="当前设备">
          <span v-if="deviceSerial" class="mono">{{ deviceSerial }}</span>
          <span v-else class="muted">未连接 —— 到「真机」页连接设备</span>
        </el-descriptions-item>
        <el-descriptions-item label="访问令牌">
          由服务端 MWINE_TOKEN 环境变量控制，<el-link type="primary" class="small" @click="needLogin = true">点此更换本浏览器令牌</el-link>
        </el-descriptions-item>
      </el-descriptions>
      <div class="small muted" style="margin-top:10px;">
        NAS 版为纯真机网关：管理局域网 / USB 连接的 Android 设备（手机、电视、盒子等），
        支持投屏、应用库、终端与文件互传；无内置虚拟机。
      </div>
    </el-card>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, inject } from 'vue'
import { apiGet, authState } from '../api'

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
