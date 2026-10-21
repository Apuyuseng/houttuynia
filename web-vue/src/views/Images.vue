<template>
  <div style="display:grid;gap:18px;">
    <el-card shadow="never">
      <template #header>模拟器运行时</template>
      <div v-if="runtime.present" class="small">
        ✅ 已安装（{{ runtime.source }}）<span v-if="runtime.path" class="muted mono"> · {{ runtime.path }}</span>
      </div>
      <div v-else>
        <el-alert type="warning" :closable="false" title="未安装运行时，虚拟机无法启动" />
        <el-button style="margin-top:10px" size="small" type="primary" @click="downloadRuntime">在线下载运行时</el-button>
      </div>
    </el-card>

    <el-card v-if="taskVisible" shadow="never">
      <template #header>
        <div style="display:flex;align-items:center;gap:8px;">
          <span>{{ task.running ? '⏳' : (task.error ? '⚠️' : '✅') }} {{ task.phase || '任务' }}</span>
          <div style="flex:1"></div>
          <el-button v-if="!task.running" size="small" @click="ackTask">知道了</el-button>
        </div>
      </template>
      <div class="small muted" style="margin-bottom:6px;">
        {{ (task.lines || []).slice(-1)[0] || '' }}
        <span v-if="task.progress_total">（{{ fmtBytes(task.progress) }} / {{ fmtBytes(task.progress_total) }}）</span>
      </div>
      <el-progress v-if="task.progress_total" :percentage="taskPct" />
      <el-alert v-if="task.error" :title="task.error" type="error" :closable="false" style="margin-top:8px;" />
    </el-card>

    <el-card shadow="never">
      <template #header>
        <div style="display:flex;align-items:center;">
          本地镜像
          <div style="flex:1"></div>
          <el-button size="small" @click="load">刷新</el-button>
        </div>
      </template>
      <el-empty v-if="!images.length" description="暂无本地镜像" />
      <el-table v-else :data="images" style="width:100%">
        <el-table-column label="版本" min-width="160">
          <template #default="{ row }">
            {{ row.display || row.api }} · {{ row.name }}
            <el-tag v-if="row.in_use" size="small" type="success" style="margin-left:6px">使用中</el-tag>
          </template>
        </el-table-column>
        <el-table-column label="路径" min-width="220">
          <template #default="{ row }"><span class="small muted mono">{{ row.rel }}</span></template>
        </el-table-column>
        <el-table-column label="大小" width="110">
          <template #default="{ row }">{{ fmtMb(row.size_mb || 0) }}</template>
        </el-table-column>
        <el-table-column width="180" fixed="right">
          <template #default="{ row }">
            <template v-if="!row.in_use">
              <el-button size="small" type="primary" @click="useImage(row)">使用</el-button>
              <el-popconfirm title="删除后不可恢复，确定？" @confirm="delImage(row)">
                <template #reference><el-button size="small" type="danger" text>删除</el-button></template>
              </el-popconfirm>
            </template>
          </template>
        </el-table-column>
      </el-table>
    </el-card>

    <el-card shadow="never">
      <template #header>
        <div style="display:flex;align-items:center;">
          在线镜像
          <div style="flex:1"></div>
          <el-button size="small" @click="loadOfficial">加载列表</el-button>
        </div>
      </template>
      <el-empty v-if="!official.length" description="点击「加载列表」查看可拉取的官方镜像" />
      <el-table v-else :data="official" style="width:100%">
        <el-table-column label="镜像" min-width="200">
          <template #default="{ row }">{{ row.display || row.path }}</template>
        </el-table-column>
        <el-table-column label="大小" width="120">
          <template #default="{ row }">{{ row.size ? fmtBytes(row.size) : '—' }}</template>
        </el-table-column>
        <el-table-column width="100" fixed="right">
          <template #default="{ row }">
            <el-button size="small" type="primary" @click="pull(row)">拉取</el-button>
          </template>
        </el-table-column>
      </el-table>
    </el-card>
  </div>
</template>

<script setup>
import { ref, computed, onMounted } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { apiGet, apiPost, fmtBytes, fmtMb } from '../api'

const runtime = ref({})
const images = ref([])
const official = ref([])
const task = ref({})
const taskVisible = ref(false)
const taskPct = computed(() => task.value.progress_total ? Math.round(task.value.progress * 100 / task.value.progress_total) : 0)

async function loadRuntime() {
  try { runtime.value = await apiGet('/api/runtime/status') } catch {}
}

async function load() {
  try {
    const d = await apiGet('/api/image/list')
    images.value = d.images || []
  } catch (e) { ElMessage.error(e.message) }
}

async function useImage(im) {
  try {
    await ElMessageBox.confirm(`切换到镜像 ${im.display || im.rel}？虚拟机将停止。`, '切换镜像', { type: 'warning' })
  } catch { return }
  try {
    await apiPost('/api/image/use', { path: im.path, stop_vm: true })
    ElMessage.success('已切换镜像')
    load()
  } catch (e) { ElMessage.error(e.message) }
}

async function delImage(im) {
  try {
    await apiPost('/api/image/delete', { path: im.path })
    ElMessage.success('已删除')
    load()
  } catch (e) { ElMessage.error(e.message) }
}

async function loadOfficial() {
  try {
    const d = await apiGet('/api/image/official')
    official.value = d.images || d.items || []
  } catch (e) {
    if (e.status === 501) ElMessage.warning('Rust 网关暂未实现在线拉取，请用桌面端导入镜像包')
    else ElMessage.error(e.message)
  }
}

async function pull(im) {
  try {
    await ElMessageBox.confirm(`拉取镜像 ${im.display || im.path}？体积较大请耐心等待。`, '拉取镜像', { type: 'info' })
  } catch { return }
  try {
    await apiPost('/api/image/official-pull', { target: im.path })
    ElMessage.success('拉取任务已开始')
    taskVisible.value = true
    pollTask()
  } catch (e) { ElMessage.error(e.message) }
}

async function downloadRuntime() {
  try {
    await apiPost('/api/runtime/download')
    taskVisible.value = true
    pollTask()
  } catch (e) {
    if (e.status === 501) ElMessage.warning('Rust 网关暂未实现运行时下载，请用桌面端安装')
    else ElMessage.error(e.message)
  }
}

async function ackTask() {
  try { await apiPost('/api/image/task/ack') } catch {}
  taskVisible.value = false
  load(); loadRuntime()
}

let polling = false
async function pollTask() {
  if (polling) return
  polling = true
  try {
    while (true) {
      let t = await apiGet('/api/image/task')
      if (!t.running && !t.done && !t.error) {
        t = await apiGet('/api/runtime/task')
      }
      task.value = t
      if (!t.running) {
        polling = false
        if (t.done) { ElMessage.success('任务完成'); load(); loadRuntime() }
        else if (t.error) ElMessage.error('任务失败：' + t.error)
        return
      }
      await new Promise(r => setTimeout(r, 1500))
    }
  } catch { polling = false }
}

onMounted(() => { load(); loadRuntime() })
</script>
