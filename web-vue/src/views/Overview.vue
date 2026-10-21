<template>
  <div class="overview-grid">
    <el-card shadow="never">
      <template #header>
        <div style="display:flex;align-items:center;gap:8px;">
          <span>设备屏幕</span>
          <div style="flex:1"></div>
          <el-button size="small" @click="downloadScreenshot">保存截图</el-button>
          <el-button v-if="canStream" size="small" :type="streaming ? 'danger' : 'primary'" @click="toggleStream">
            {{ streaming ? '■ 停止投屏' : '▶ 投屏' }}
          </el-button>
          <el-button v-else size="small" @click="toggleLive">
            {{ live ? '暂停画面' : '实时画面' }}
          </el-button>
        </div>
      </template>
      <div class="phone-frame">
        <span class="phone-btn phone-btn-vol"></span>
        <span class="phone-btn phone-btn-power"></span>
        <div class="phone-panel">
          <div class="screen-box" ref="screenBox">
            <div class="placeholder" v-if="!imgShown">
              虚拟机{{ running ? '运行中' : '未运行' }}<br>
              <span class="small muted">{{ running ? '点击投屏查看实时画面' : '启动后此处显示设备画面' }}</span>
            </div>
            <img v-if="imgUrlNow && !streaming" :src="imgUrlNow" @load="imgShown = true" alt="">
          </div>
        </div>
      </div>
      <div v-if="streaming" style="margin-top:10px;">
        <el-space wrap>
          <el-button size="small" @click="sendKey(4)">返回</el-button>
          <el-button size="small" @click="sendKey(3)">主页</el-button>
          <el-button size="small" @click="sendKey(187)">多任务</el-button>
          <el-button size="small" @click="sendKey(26)">电源</el-button>
          <el-button size="small" @click="sendKey(24)">音量+</el-button>
          <el-button size="small" @click="sendKey(25)">音量−</el-button>
        </el-space>
        <el-input v-model="textToSend" size="small" placeholder="输入文本发送到设备" style="margin-top:8px"
                  @keyup.enter="sendText">
          <template #append><el-button @click="sendText">发送</el-button></template>
        </el-input>
        <div class="small muted" style="margin-top:6px">{{ streamStatus }}</div>
      </div>
    </el-card>

    <div style="display:grid;gap:18px;">
      <el-card shadow="never">
        <template #header>虚拟机</template>
        <div class="muted small">
          状态：<b :style="{color: running ? 'var(--el-color-success)' : 'inherit'}">{{ running ? '运行中' : '未运行' }}</b>
          <template v-if="serial"> · 设备 <span class="mono">{{ serial }}</span></template>
        </div>
        <el-space style="margin-top:14px;">
          <el-button type="primary" :loading="busy === 'start'" @click="vmAction('start')">▶ 启动</el-button>
          <el-button :loading="busy === 'stop'" @click="vmAction('stop')">■ 停止（自动存档）</el-button>
        </el-space>
        <div v-if="vmError" class="small" style="color:var(--el-color-danger);margin-top:10px;">{{ vmError }}</div>
      </el-card>

      <el-card shadow="never">
        <template #header>性能监控</template>
        <el-row :gutter="14" v-if="perf.running">
          <el-col :span="8">
            <el-progress type="dashboard" :percentage="Math.min(perf.cpu_pct, 100)" :color="gaugeColor">
              <template #default><div><b>{{ perf.cpu_pct.toFixed(0) }}%</b><div class="small muted">CPU</div></div></template>
            </el-progress>
          </el-col>
          <el-col :span="8">
            <el-progress type="dashboard" :percentage="Math.min(memPct, 100)" :color="gaugeColor">
              <template #default>
                <div><b>{{ fmtMb(memUsed) }}</b><div class="small muted">内存 / {{ fmtMb(perf.mem?.total_mb || 0) }}</div></div>
              </template>
            </el-progress>
          </el-col>
          <el-col :span="8">
            <el-statistic title="已运行" :value="fmtUptime(perf.uptime_s || 0)" />
          </el-col>
        </el-row>
        <el-text v-else type="info" size="small">虚拟机未运行</el-text>
      </el-card>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, inject } from 'vue'
import { ElMessage } from 'element-plus'
import { apiGet, apiPost, imgUrl, fmtMb, fmtUptime } from '../api'
import { ScrcpyWebPlayer } from '../player'

const { vmRunning, serial } = inject('vmState')
const running = vmRunning

const screenBox = ref(null)
const busy = ref('')
const vmError = ref('')
const perf = ref({ running: false, cpu_pct: 0, mem: {}, uptime_s: 0 })
const imgUrlNow = ref('')
const imgShown = ref(false)
const live = ref(false)
const streaming = ref(false)
const streamStatus = ref('')
const textToSend = ref('')
const canStream = ScrcpyWebPlayer.supported()

let player = null

const memUsed = computed(() => (perf.value.mem?.total_mb || 0) - (perf.value.mem?.available_mb || 0))
const memPct = computed(() => perf.value.mem?.total_mb ? memUsed.value * 100 / perf.value.mem.total_mb : 0)
const gaugeColor = (p) => p > 85 ? '#f87171' : p > 65 ? '#fbbf24' : '#4ade80'

async function vmAction(act) {
  busy.value = act
  try {
    const r = await apiPost('/api/vm/' + act)
    ElMessage.success(act === 'start' ? (r.already ? '虚拟机已在运行' : '启动指令已下发') : '停止指令已下发（正在存档）')
  } catch (e) {
    ElMessage.error(e.message)
  }
  busy.value = ''
}

async function refreshPerf() {
  if (streaming.value) return
  try { perf.value = await apiGet('/api/perf') } catch {}
  try {
    const st = await apiGet('/api/status')
    vmError.value = st.error || ''
    if (st.vm?.running && !live.value && !streaming.value) loadShotOnce()
  } catch {}
}

async function loadShotOnce() {
  const url = imgUrl('/api/screenshot.png') + '&t=' + Date.now()
  const img = new Image()
  img.onload = () => { imgUrlNow.value = url; imgShown.value = true }
  img.src = url
}

let liveTimer = null
function toggleLive() {
  if (live.value) {
    clearInterval(liveTimer); liveTimer = null; live.value = false
  } else {
    live.value = true
    loadShotOnce()
    liveTimer = setInterval(loadShotOnce, 1200)
  }
}

async function toggleStream() {
  if (player) { await stopStream(); return }
  if (live.value) toggleLive()
  imgUrlNow.value = ''
  imgShown.value = false
  player = new ScrcpyWebPlayer({
    onStatus: (t, isErr) => { streamStatus.value = t },
    onEnd: () => {
      player = null
      streaming.value = false
      loadShotOnce()
    },
  })
  streaming.value = true
  // 挂载 canvas
  const box = screenBox.value
  box.innerHTML = ''
  const badge = document.createElement('div')
  badge.className = 'screen-live-dot'
  badge.textContent = '● 投屏中'
  box.appendChild(badge)
  box.appendChild(player.canvas)
  try {
    await player.start()
  } catch (e) {
    ElMessage.error('投屏失败：' + e.message)
    player._finish()
    player = null
    streaming.value = false
    loadShotOnce()
  }
}

async function stopStream() {
  if (!player) return
  const p = player
  player = null
  await p.stop()
  streaming.value = false
  loadShotOnce()
}

function sendKey(k) { player && player.key(k) }
function sendText() {
  if (player && textToSend.value) { player.text(textToSend.value); textToSend.value = '' }
}

async function downloadScreenshot() {
  try {
    const resp = await fetch(imgUrl('/api/screenshot.png'))
    if (!resp.ok) throw new Error('截图失败（虚拟机在运行吗？）')
    const blob = await resp.blob()
    const a = document.createElement('a')
    a.href = URL.createObjectURL(blob)
    a.download = 'mwine-' + new Date().toISOString().replace(/[:.]/g, '-') + '.png'
    a.click()
    URL.revokeObjectURL(a.href)
    ElMessage.success('截图已下载')
  } catch (e) { ElMessage.error(e.message) }
}

let timer
onMounted(() => { refreshPerf(); timer = setInterval(refreshPerf, 2500) })
onUnmounted(() => {
  clearInterval(timer)
  if (liveTimer) clearInterval(liveTimer)
  if (player) { const p = player; player = null; p.stop() }
})
</script>
