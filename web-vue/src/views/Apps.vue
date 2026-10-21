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

    <!-- 启动器桌面：设备壁纸背景 + 图标网格 -->
    <div class="launcher" :style="launcherStyle">
      <div v-if="loading" class="launcher-msg">读取中…</div>
      <div v-else-if="!running" class="launcher-msg">虚拟机未运行 —— 启动后此处展示设备桌面</div>
      <el-empty v-else-if="!filtered.length" description="暂无应用" style="padding:60px 0" />
      <div v-else class="launcher-grid">
        <el-dropdown v-for="a in filtered" :key="a.package" trigger="contextmenu"
                     @command="(cmd) => onCmd(cmd, a)">
          <div class="launcher-item" @click="run(a)">
            <div class="launcher-icon">
              <img v-if="iconOk[a.package]" :src="imgUrl('/api/apps/icon?pkg=' + encodeURIComponent(a.package))"
                   @error="iconOk[a.package] = false" alt="">
              <template v-else>{{ (a.name || a.package).slice(0, 1).toUpperCase() }}</template>
            </div>
            <div class="launcher-name">{{ a.name }}</div>
          </div>
          <template #dropdown>
            <el-dropdown-menu>
              <el-dropdown-item command="run">启动</el-dropdown-item>
              <el-dropdown-item command="uninstall" divided>卸载（清除数据）</el-dropdown-item>
            </el-dropdown-menu>
          </template>
        </el-dropdown>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { apiGet, apiPost, imgUrl, fmtBytes, authState } from '../api'
import { inject } from 'vue'

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

const filtered = computed(() => {
  const q = filter.value.trim().toLowerCase()
  if (!q) return apps.value
  return apps.value.filter(a =>
    (a.name || '').toLowerCase().includes(q) || a.package.toLowerCase().includes(q))
})

const launcherStyle = computed(() => {
  if (wallpaperUrl.value) {
    return {
      backgroundImage: `url(${wallpaperUrl.value})`,
      backgroundSize: 'cover',
      backgroundPosition: 'center',
    }
  }
  // 无壁纸（非 root 镜像）时的渐变兜底
  return { background: 'linear-gradient(155deg, #232a3b 0%, #10141c 60%, #0c0f16 100%)' }
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
  loadWallpaper()
}

async function loadWallpaper() {
  // 壁纸就绪则换真壁纸；404 保持渐变兜底
  const url = imgUrl('/api/wallpaper.png') + '&t=' + Date.now()
  const img = new Image()
  img.onload = () => { wallpaperUrl.value = url }
  img.src = url
}

async function run(a) {
  try { await apiPost(`/api/apps/${encodeURIComponent(a.package)}/run`); ElMessage.success('已下发启动指令') }
  catch (e) { ElMessage.error(e.message) }
}

async function onCmd(cmd, a) {
  if (cmd === 'run') return run(a)
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

<style scoped>
.launcher {
  border-radius: 14px;
  border: 1px solid var(--el-border-color);
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
</style>
