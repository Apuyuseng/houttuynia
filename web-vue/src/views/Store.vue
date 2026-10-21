<template>
  <div style="display:grid;gap:18px;">
    <el-card shadow="never">
      <el-space wrap style="width:100%">
        <el-select v-model="source" style="width:160px" @change="loadRecommend">
          <el-option v-for="s in sources" :key="s.id" :value="s.id" :label="s.name" />
        </el-select>
        <el-input v-model="q" placeholder="搜索应用，如 Fennec / 微信 / Termux" style="width:320px"
                  clearable @keyup.enter="doSearch" />
        <el-button type="primary" :loading="searching" @click="doSearch">搜索</el-button>
      </el-space>
    </el-card>

    <el-alert v-if="notImpl" type="warning" :closable="false"
              title="Rust 网关暂未实现在线商店——请用桌面客户端的「在线下载」应用，或等待后续版本" />

    <el-card v-if="taskVisible" shadow="never">
      <template #header>
        <div style="display:flex;align-items:center;gap:8px;">
          <span>{{ task.running ? '⏳' : (task.error ? '⚠️' : '✅') }} {{ task.label || task.package || '下载任务' }}</span>
          <div style="flex:1"></div>
          <el-button v-if="task.running" size="small" type="danger" @click="cancelTask">取消</el-button>
          <el-button v-else size="small" @click="taskVisible = false">知道了</el-button>
        </div>
      </template>
      <div class="small muted" style="margin-bottom:6px;">{{ task.phase }}
        <span v-if="task.progress_total">（{{ fmtBytes(task.progress) }} / {{ fmtBytes(task.progress_total) }}）</span>
      </div>
      <el-progress v-if="task.progress_total" :percentage="taskPct" />
      <el-alert v-if="task.error" :title="task.error" type="error" :closable="false" style="margin-top:8px;" />
    </el-card>

    <el-card shadow="never">
      <template #header>{{ listTitle }}</template>
      <el-empty v-if="!list.length && !loading" description="无结果" />
      <div v-if="loading" class="muted small">读取中…</div>
      <el-table v-else :data="list" style="width:100%">
        <el-table-column label="应用" min-width="260">
          <template #default="{ row }">
            <div style="display:flex;align-items:center;gap:10px;">
              <div class="app-icon" style="width:40px;height:40px;font-size:17px;margin:0;">
                <img v-if="row.icon_url" :src="row.icon_url" @error="row.icon_url = ''" alt="">
                <template v-else>{{ (row.name || row.package || '?').slice(0, 1).toUpperCase() }}</template>
              </div>
              <div>
                <div style="font-weight:600">{{ row.name || row.package }}</div>
                <div class="small muted mono">{{ row.package }}</div>
              </div>
            </div>
          </template>
        </el-table-column>
        <el-table-column label="简介" min-width="220">
          <template #default="{ row }">
            <span class="small muted">{{ row.summary || '—' }}</span>
          </template>
        </el-table-column>
        <el-table-column label="版本" width="130">
          <template #default="{ row }">
            <span class="small">{{ row.latest?.version_name || '—' }}</span>
          </template>
        </el-table-column>
        <el-table-column label="操作" width="170" fixed="right">
          <template #default="{ row }">
            <el-button size="small" @click="showVersions(row)">版本</el-button>
            <el-button size="small" type="primary" @click="install(row)">安装</el-button>
          </template>
        </el-table-column>
      </el-table>
    </el-card>

    <el-dialog v-model="verDialog" :title="(verApp?.name || verApp?.package) + ' · 选择版本'" width="520px">
      <div v-if="verLoading" class="muted small">读取版本…</div>
      <el-empty v-else-if="!versions.length" description="无版本信息" />
      <el-table v-else :data="versions" max-height="400">
        <el-table-column label="版本" min-width="160">
          <template #default="{ row }">
            v{{ row.name || row.code || '?' }}
            <el-tag v-if="installedCode && row.code === installedCode" size="small" type="success">已安装</el-tag>
          </template>
        </el-table-column>
        <el-table-column label="大小" width="110">
          <template #default="{ row }">{{ row.size ? fmtBytes(row.size) : '—' }}</template>
        </el-table-column>
        <el-table-column label="时间" width="130">
          <template #default="{ row }"><span class="small muted">{{ row.added || '—' }}</span></template>
        </el-table-column>
        <el-table-column width="90" fixed="right">
          <template #default="{ row }">
            <el-button size="small" type="primary" @click="install(verApp, row)">安装</el-button>
          </template>
        </el-table-column>
      </el-table>
    </el-dialog>
  </div>
</template>

<script setup>
import { ref, computed, onMounted } from 'vue'
import { ElMessage } from 'element-plus'
import { apiGet, apiPost, fmtBytes } from '../api'

const sources = ref([{ id: 'fdroid', name: 'F-Droid' }, { id: 'wandoujia', name: '豌豆荚' }])
const source = ref('fdroid')
const q = ref('')
const list = ref([])
const loading = ref(false)
const searching = ref(false)
const notImpl = ref(false)
const listTitle = ref('推荐应用')
const task = ref({})
const taskVisible = ref(false)
const taskPct = computed(() => task.value.progress_total ? Math.round(task.value.progress * 100 / task.value.progress_total) : 0)

const verDialog = ref(false)
const verApp = ref(null)
const versions = ref([])
const verLoading = ref(false)
const installedCode = ref(0)

async function loadSources() {
  try {
    const d = await apiGet('/api/store/sources')
    if (d.sources?.length) sources.value = d.sources
  } catch (e) {
    if (e.status === 501) notImpl.value = true
  }
}

async function loadRecommend() {
  loading.value = true
  listTitle.value = '推荐应用'
  try {
    const d = await apiGet('/api/store/recommend?source=' + encodeURIComponent(source.value))
    list.value = d.apps || []
  } catch (e) {
    if (e.status === 501) { notImpl.value = true; list.value = [] }
    else ElMessage.error(e.message)
  }
  loading.value = false
}

async function doSearch() {
  if (!q.value.trim() || searching.value) return
  searching.value = true
  loading.value = true
  try {
    const d = await apiGet(`/api/store/search?q=${encodeURIComponent(q.value)}&source=${encodeURIComponent(source.value)}`)
    list.value = d.apps || []
    listTitle.value = `搜索「${q.value}」（${list.value.length} 条）`
  } catch (e) {
    if (e.status === 501) notImpl.value = true
    else ElMessage.error(e.message)
  }
  loading.value = false
  searching.value = false
}

async function install(a, ver) {
  const body = { source: a.source || source.value, app_id: a.app_id || '', package: a.package || '', name: a.name || a.package || '' }
  if (ver) {
    if (ver.code) body.version_code = ver.code
    if (ver.name) body.version_name = ver.name
  }
  try {
    await apiPost('/api/store/download', body)
    ElMessage.success('下载任务已开始')
    taskVisible.value = true
    pollTask()
  } catch (e) { ElMessage.error(e.message) }
}

async function showVersions(a) {
  verApp.value = a
  verDialog.value = true
  verLoading.value = true
  versions.value = []
  try {
    const src = a.source || source.value
    const d = await apiGet(`/api/store/versions?source=${encodeURIComponent(src)}&app_id=${encodeURIComponent(a.app_id || '')}&package=${encodeURIComponent(a.package || '')}`)
    versions.value = d.versions || []
    installedCode.value = d.installed_code || 0
  } catch (e) { ElMessage.error(e.message) }
  verLoading.value = false
}

async function cancelTask() {
  try { await apiPost('/api/store/cancel') } catch {}
}

let polling = false
async function pollTask() {
  if (polling) return
  polling = true
  try {
    while (true) {
      const t = await apiGet('/api/store/task')
      task.value = t
      if (!t.running) {
        polling = false
        if (t.done) ElMessage.success('下载安装完成')
        else if (t.error) ElMessage.error('任务失败：' + t.error)
        return
      }
      await new Promise(r => setTimeout(r, 1200))
    }
  } catch { polling = false }
}

onMounted(() => { loadSources(); loadRecommend() })
</script>
