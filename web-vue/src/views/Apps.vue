<template>
  <div style="display:grid;gap:18px;">
    <el-card shadow="never">
      <el-space wrap>
        <el-button type="primary" @click="fileInput.click()">⬆ 上传 APK 安装</el-button>
        <input ref="fileInput" type="file" accept=".apk" style="display:none" @change="onFile" />
        <el-button @click="load">刷新列表</el-button>
        <div style="flex:1"></div>
        <el-input v-model="filter" placeholder="筛选应用名 / 包名" clearable style="width:240px" />
      </el-space>
    </el-card>

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

    <el-card shadow="never">
      <template #header>已安装应用（{{ filtered.length }}）</template>
      <div v-if="loading" class="muted small">读取中…</div>
      <el-empty v-else-if="!filtered.length" description="暂无应用 —— 上传 APK 安装" />
      <div v-else class="app-grid">
        <div v-for="a in filtered" :key="a.package" class="app-card">
          <div class="app-icon">
            <img v-if="iconOk[a.package]" :src="imgUrl('/api/apps/icon?pkg=' + encodeURIComponent(a.package))" @error="iconOk[a.package] = false" alt="">
            <template v-else>{{ (a.name || a.package).slice(0, 1).toUpperCase() }}</template>
          </div>
          <div class="app-name" :title="a.name">{{ a.name }}</div>
          <div class="app-pkg" :title="a.package">{{ a.package }}</div>
          <el-space>
            <el-button size="small" type="primary" @click="run(a)">启动</el-button>
            <el-popconfirm title="确定卸载？应用数据将被清除" @confirm="uninstall(a)">
              <template #reference><el-button size="small" text type="danger">卸载</el-button></template>
            </el-popconfirm>
          </el-space>
        </div>
      </div>
    </el-card>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { ElMessage } from 'element-plus'
import { apiGet, apiPost, imgUrl, fmtBytes, authState } from '../api'

const apps = ref([])
const loading = ref(true)
const filter = ref('')
const fileInput = ref(null)
const iconOk = ref({})
const task = ref({})
const taskVisible = ref(false)

const filtered = computed(() => {
  const q = filter.value.trim().toLowerCase()
  if (!q) return apps.value
  return apps.value.filter(a =>
    (a.name || '').toLowerCase().includes(q) || a.package.toLowerCase().includes(q))
})

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
}

async function run(a) {
  try { await apiPost(`/api/apps/${encodeURIComponent(a.package)}/run`); ElMessage.success('已下发启动指令') }
  catch (e) { ElMessage.error(e.message) }
}

async function uninstall(a) {
  try {
    const r = await apiPost(`/api/apps/${encodeURIComponent(a.package)}/uninstall`)
    if (r.ok) { ElMessage.success('已卸载'); setTimeout(load, 1200) }
    else ElMessage.error('卸载失败：' + (r.output || ''))
  } catch (e) { ElMessage.error(e.message) }
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

onMounted(load)
onUnmounted(() => { polling = false })
</script>
