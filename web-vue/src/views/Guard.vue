<template>
  <div style="display:grid;gap:18px;">
    <!-- 实时状态 -->
    <el-card shadow="never">
      <template #header>
        <div style="display:flex;align-items:center;gap:8px;">
          <span>实时状态</span>
          <el-select v-model="selSerial" size="small" style="width:220px" @change="onDeviceChange">
            <el-option v-for="d in devices" :key="d.serial" :value="d.serial"
                       :label="(d.enabled ? '🛡 ' : '') + d.serial + (d.current ? '（当前）' : '')" />
          </el-select>
          <el-tag v-if="lastCheckAgo !== null" size="small" type="success" effect="plain">巡检 {{ lastCheckAgo }}s 前</el-tag>
          <div style="flex:1"></div>
          <el-switch v-model="form.enabled" active-text="管控总开关" @change="save" />
        </div>
      </template>
      <el-space wrap size="large">
        <div>
          <div class="muted small">屏幕</div>
          <el-tag :type="screen.on ? 'success' : 'info'" effect="dark">
            {{ screenTag }}
          </el-tag>
        </div>
        <div>
          <div class="muted small">前台应用</div>
          <span class="mono">{{ screen.foreground || '—' }}</span>
        </div>
        <div>
          <div class="muted small">今日已用</div>
          <b :style="{ color: overLimit ? 'var(--el-color-danger)' : 'inherit' }">
            {{ todayUsedMin }} 分钟</b>
          <span class="muted small"> / {{ form.daily_limit_min ? form.daily_limit_min + ' 分钟' : '不限' }}</span>
        </div>
        <div>
          <div class="muted small">时段</div>
          <el-tag :type="inWindow ? 'success' : 'danger'" effect="plain">
            {{ inWindow ? '允许使用' : '非允许时段' }}
          </el-tag>
        </div>
        <div>
          <div class="muted small">外网</div>
          <el-tag :type="form.block_internet ? 'danger' : 'success'" effect="plain">
            {{ form.block_internet ? '已封禁' : '正常' }}
          </el-tag>
        </div>
      </el-space>
      <div v-if="!screen" class="small muted" style="margin-top:10px;">设备离线或未连接——状态将在设备上线后自动恢复</div>
    </el-card>

    <!-- 用量统计 -->
    <el-card shadow="never">
      <template #header>
        <div style="display:flex;align-items:center;gap:8px;">
          <span>使用统计（近 30 天 · 每日亮屏分钟）</span>
          <div style="flex:1"></div>
          <span class="small muted">7 天合计 {{ weekSum }} 分钟 · 30 天合计 {{ monthSum }} 分钟</span>
        </div>
      </template>
      <svg :viewBox="'0 0 ' + (chartBars.length * 22 + 10) + ' 130'" style="width:100%;height:150px;">
        <g v-for="(b, i) in chartBars" :key="b.date">
          <rect :x="5 + i * 22" :y="110 - b.h" width="16" :height="b.h" rx="3"
                :fill="b.today ? '#81c995' : (b.min > 0 ? '#a8c7fa' : 'rgba(255,255,255,0.09)')">
            <title>{{ b.date }}：{{ b.min }} 分钟</title>
          </rect>
          <text v-if="i % 5 === 0 || b.today" :x="13 + i * 22" y="126" text-anchor="middle"
                font-size="9" :fill="b.today ? '#81c995' : 'rgba(255,255,255,0.45)'">{{ b.label }}</text>
          <text v-if="b.min > 0" :x="13 + i * 22" :y="Math.max(110 - b.h - 4, 10)" text-anchor="middle"
                font-size="8" fill="rgba(255,255,255,0.45)">{{ b.min }}</text>
        </g>
        <line x1="0" y1="110" x2="100%" y2="110" stroke="rgba(255,255,255,0.12)" stroke-width="1" />
      </svg>
      <div class="small muted">只统计亮屏时间（待机不计）；绿柱为今天，蓝柱为历史，悬停看具体数值。</div>
    </el-card>

    <!-- 规则设置 -->
    <el-card shadow="never">
      <template #header>管控规则</template>
      <el-form label-width="120px" label-position="left">
        <el-form-item label="允许时段">
          <div style="display:grid;gap:8px;width:100%;">
            <div v-for="(w, i) in windowRows" :key="i" style="display:flex;gap:8px;align-items:center;">
              <el-time-select v-model="w.s" :start="'00:00'" :end="'23:30'" :step="'00:30'" placeholder="开始" style="width:120px" @change="markDirty" />
              <span class="muted">至</span>
              <el-time-select v-model="w.e" :start="'00:00'" :end="'23:30'" :step="'00:30'" placeholder="结束" style="width:120px" @change="markDirty" />
              <el-button text type="danger" @click="windowRows.splice(i, 1)">删除</el-button>
            </div>
            <el-button size="small" @click="windowRows.push({ s: '17:00', e: '20:00' })">+ 添加时段</el-button>
            <div class="small muted">不填 = 全天允许；时段外亮屏将自动待机（支持跨零点，如 20:00-06:30）。待机本身不受限。</div>
          </div>
        </el-form-item>
        <el-form-item label="每日上限">
          <el-space>
            <el-input-number v-model="form.daily_limit_min" :min="0" :max="1440" :step="15" controls-position="right" style="width:140px" @change="markDirty" />
            <span class="small muted">分钟（0 = 不限；只统计亮屏时间，待机不计）</span>
          </el-space>
        </el-form-item>
        <el-form-item label="应用黑名单">
          <el-select v-model="form.blocked_apps" multiple filterable clearable placeholder="选择禁止启动的应用" style="width:100%" @change="markDirty">
            <el-option v-for="a in appOptions" :key="a.package" :value="a.package" :label="(a.name || a.package) + '（' + a.package + '）'" />
          </el-select>
          <div class="small muted">黑名单应用出现在前台即被强制停止并弹系统通知。</div>
        </el-form-item>
        <el-form-item label="外网封禁">
          <el-switch v-model="form.block_internet" @change="markDirty" />
          <div class="small muted">
            开启后电视无法访问互联网（DNS + 代理黑洞方案，内网与远程管理不受影响，可随时关闭恢复）。
            非 root 设备方案的边界：极少数直连 IP 的原生流量不受约束。
          </div>
        </el-form-item>
        <el-form-item>
          <el-button type="primary" :loading="saving" @click="save">保存规则</el-button>
        </el-form-item>
      </el-form>
    </el-card>

    <!-- 事件日志 -->
    <el-card shadow="never">
      <template #header>管控事件（最近 50 条）</template>
      <el-table :data="events" size="small" style="width:100%">
        <el-table-column label="时间" width="170">
          <template #default="{ row }">{{ new Date(row.time).toLocaleString() }}</template>
        </el-table-column>
        <el-table-column label="类型" width="110">
          <template #default="{ row }">
            <el-tag size="small" :type="kindType(row.kind)">{{ kindLabel(row.kind) }}</el-tag>
          </template>
        </el-table-column>
        <el-table-column prop="detail" label="详情" min-width="260" />
      </el-table>
    </el-card>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { ElMessage } from 'element-plus'
import { apiGet, apiPost } from '../api'

const serial = ref('')
const selSerial = ref('')
const devices = ref([])
const lastCheckMs = ref(null)
const screen = ref({})
const history = ref([])
const todayUsedMin = ref(0)
const inWindow = ref(true)
const events = ref([])
const appOptions = ref([])
const saving = ref(false)

const form = ref({
  enabled: false,
  windows: [],
  daily_limit_min: 0,
  blocked_apps: [],
  block_internet: false,
})
const windowRows = ref([])

const overLimit = computed(() =>
  form.value.daily_limit_min > 0 && todayUsedMin.value >= form.value.daily_limit_min)
const screenTag = computed(() => {
  if (!screen.value || screen.value.on === undefined) return '未知'
  if (!screen.value.on) return '待机'
  return screen.value.wakefulness === 'Dreaming' ? '屏保' : '亮屏'
})

function kindLabel(k) {
  return { sleep_forced: '强制待机', wake_detected: '亮屏', app_blocked: '拦截应用',
           internet_on: '外网封禁', internet_off: '外网恢复', config: '配置' }[k] || k
}
function kindType(k) {
  return { sleep_forced: 'danger', wake_detected: 'warning', app_blocked: 'danger',
           internet_on: 'warning', internet_off: 'success', config: 'info' }[k] || 'info'
}

const chartBars = computed(() => {
  const max = Math.max(1, ...history.value.map(h => h.min))
  return history.value.map(h => ({
    date: h.date,
    min: h.min,
    h: Math.round(h.min * 90 / max),
    today: h.date === new Date(Date.now() + 8 * 3600e3).toISOString().slice(0, 10),
    label: h.date.slice(5).replace('-', '/'),
  }))
})
const weekSum = computed(() => history.value.slice(-7).reduce((a, b) => a + b.min, 0))
const monthSum = computed(() => history.value.reduce((a, b) => a + b.min, 0))
const lastCheckAgo = computed(() => {
  if (!lastCheckMs.value) return null
  return Math.max(0, Math.round((Date.now() - lastCheckMs.value) / 1000))
})

function onDeviceChange() { dirty.value = false; load() }

async function load() {
  try {
    const url = selSerial.value ? '/api/guard?serial=' + encodeURIComponent(selSerial.value) : '/api/guard'
    const d = await apiGet(url)
    serial.value = d.serial || ''
    if (!selSerial.value) selSerial.value = d.serial || ''
    devices.value = d.devices || []
    lastCheckMs.value = d.last_check_ms || null
    history.value = d.usage_history || []
    screen.value = d.screen || {}
    todayUsedMin.value = d.today_used_min || 0
    inWindow.value = d.in_window !== false
    events.value = d.events || []
    // 只在非编辑态回填（避免覆盖用户正在编辑的表单）
    if (!dirty.value) {
      form.value = {
        enabled: d.enabled, windows: d.windows || [],
        daily_limit_min: d.daily_limit_min || 0,
        blocked_apps: d.blocked_apps || [], block_internet: !!d.block_internet,
      }
      windowRows.value = (d.windows || []).map(w => {
        const [s, e] = w.split('-')
        return { s, e }
      })
    }
  } catch (e) { /* 设备离线等，静默 */ }
}

let dirty = ref(false)
function markDirty() { dirty.value = true }

async function save() {
  saving.value = true
  const rows = windowRows.value.filter(w => w.s && w.e)
  const payload = {
    serial: selSerial.value || serial.value || undefined,
    enabled: form.value.enabled,
    windows: rows.map(w => `${w.s}-${w.e}`),
    daily_limit_min: form.value.daily_limit_min,
    blocked_apps: form.value.blocked_apps,
    block_internet: form.value.block_internet,
  }
  try {
    await apiPost('/api/guard', payload)
    ElMessage.success('管控规则已保存')
    dirty.value = false
    load()
  } catch (e) {
    ElMessage.error(e.message)
    load() // 失败回滚显示
  }
  saving.value = false
}

async function loadApps() {
  try {
    const d = await apiGet('/api/apps')
    appOptions.value = d.apps || []
  } catch { /* 设备离线 */ }
}

let timer = null
onMounted(() => {
  load(); loadApps()
  timer = setInterval(load, 5000)
})
onUnmounted(() => clearInterval(timer))
</script>
