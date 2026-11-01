<template>
  <div class="overview-grid" :class="{ landscape: isLandscape }">
    <div class="screen-panel">
      <div class="screen-head">
        <div style="display:flex;align-items:center;gap:8px;">
          <div style="flex:1"></div>
          <el-select v-model="quality" size="small" style="width:112px" title="画质档位"
                     @change="onQualityChange" :disabled="qualityChanging">
            <el-option value="low" label="流畅 720·15fps" />
            <el-option value="medium" label="均衡 1080·24fps" />
            <el-option value="high" label="高清 1440·30fps" />
            <el-option value="auto" label="⚡ 自动（推荐）" />
          </el-select>
          <el-button size="small" @click="downloadScreenshot">保存截图</el-button>
          <el-button v-if="canStream" size="small" :type="streaming ? 'danger' : 'primary'" @click="toggleStream">
            {{ streaming ? '■ 停止投屏' : '▶ 投屏' }}
          </el-button>
          <el-button v-else size="small" @click="toggleLive">
            {{ live ? '暂停画面' : '实时画面' }}
          </el-button>
        </div>
      </div>
      <div class="phone-frame" :class="{ landscape: isLandscape }">
        <span v-if="!isLandscape" class="phone-btn phone-btn-vol"></span>
        <span v-if="!isLandscape" class="phone-btn phone-btn-power"></span>
        <div class="phone-panel">
          <div class="screen-box" ref="screenBox">
            <div class="placeholder" v-if="!imgShown">
              {{ deviceSerial ? '真机已连接' : '真机未连接' }}<br>
              <span class="small muted">{{ deviceSerial ? '点击投屏查看实时画面' : '到「真机」页连接设备后此处显示画面' }}</span>
            </div>
            <img v-if="imgUrlNow && !streaming" :src="imgUrlNow" @load="onImgLoad" @error="imgShown = false; imgUrlNow = ''" alt="">
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
          <el-button size="small" @click="toggleMute">{{ muted ? '🔇 已静音' : '🔊 本页声音' }}</el-button>
        </el-space>
        <el-input v-model="textToSend" size="small" placeholder="输入文本发送到设备" style="margin-top:8px"
                  @keyup.enter="sendText">
          <template #append><el-button @click="sendText">发送</el-button></template>
        </el-input>
        <div class="small muted" style="margin-top:6px">{{ streamStatus }}</div>
      </div>
        </div>

    <div style="display:grid;gap:18px;">
      <el-card shadow="never">
        <template #header>真机</template>
        <div class="muted small">
          <template v-if="deviceSerial">设备 <span class="mono">{{ deviceSerial }}</span></template>
          <template v-else>未连接 —— 到「真机」页连接手机 / 电视等 Android 设备</template>
          <template v-if="devInfo?.battery?.present">
            <br>电量 <b>{{ devInfo.battery.level }}%</b>{{ devInfo.battery.status === 'charging' ? ' ⚡充电中' : '' }}
            <template v-if="devInfo.battery.temp_c"> · 电池 {{ devInfo.battery.temp_c.toFixed(1) }}°C</template>
          </template>
          <template v-else-if="devInfo"><br>供电正常（无电池/固定电源）</template>
        </div>
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
        <el-text v-else type="info" size="small">真机未连接</el-text>
      </el-card>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, inject } from 'vue'
import { ElMessage } from 'element-plus'
import { apiGet, apiPost, imgUrl, fmtMb, fmtUptime } from '../api'
import { ScrcpyWebPlayer } from '../player'

const { deviceSerial } = inject('vmState')
const devInfo = ref(null)
async function loadDevInfo() {
  try {
    const m = await apiGet('/api/device/info')
    devInfo.value = m[deviceSerial.value] || null
  } catch { devInfo.value = null }
}
const running = computed(() => !!deviceSerial.value)
const targetSerial = computed(() => deviceSerial.value)

const screenBox = ref(null)
const perf = ref({ running: false, cpu_pct: 0, mem: {}, uptime_s: 0 })
const imgUrlNow = ref('')
const imgShown = ref(false)
const isLandscape = ref(false)   // 横屏内容（TV/平板）：去掉手机边框
function onImgLoad(e) {
  imgShown.value = true
  const img = e.target
  if (img && img.naturalWidth > img.naturalHeight) isLandscape.value = true
  else isLandscape.value = false
}
const live = ref(false)
const streaming = ref(false)
const streamStatus = ref('')
const textToSend = ref('')
const quality = ref(localStorage.getItem('mw_quality') || 'auto')
const qualityChanging = ref(false)
async function onQualityChange(v) {
  qualityChanging.value = true
  try {
    if (player) await player.setQuality(v)
    else if (v !== 'auto') {
      // 未投屏时只落偏好（下次开流生效）
      localStorage.setItem('mw_auto_tier', v)
    }
  } finally { qualityChanging.value = false }
}
const muted = ref(false)
function toggleMute() {
  muted.value = !muted.value
  if (player) player.setMuted(muted.value)
}
const canStream = ScrcpyWebPlayer.supported()

let player = null

const memUsed = computed(() => (perf.value.mem?.total_mb || 0) - (perf.value.mem?.available_mb || 0))
const memPct = computed(() => perf.value.mem?.total_mb ? memUsed.value * 100 / perf.value.mem.total_mb : 0)
const gaugeColor = (p) => p > 85 ? '#f28b82' : p > 65 ? '#fdd663' : '#81c995'

async function refreshPerf() {
  if (streaming.value) return
  try { perf.value = await apiGet('/api/perf') } catch {}
  try {
    const st = await apiGet('/api/status')
    if (st.target_serial && !live.value && !streaming.value) loadShotOnce()
  } catch {}
}

async function loadShotOnce() {
  // 直接换 src：@load 隐 imgShown（双探测加载会导致占位符滞留）
  imgUrlNow.value = imgUrl('/api/screenshot.png') + '&t=' + Date.now()
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
    openQuery: targetSerial.value ? `&serial=${encodeURIComponent(targetSerial.value)}` : '',
    onStatus: (t, isErr) => { streamStatus.value = t },
    onFirstFrame: () => {
      // 首帧到手即知真实宽高比：横屏（TV/平板）切无边框样式
      if (player.devW > player.devH) isLandscape.value = true
      else isLandscape.value = false
    },
    onEnd: () => {
      player = null
      streaming.value = false
      muted.value = false
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
    if (!resp.ok) throw new Error('截图失败（设备已连接？）')
    const blob = await resp.blob()
    const a = document.createElement('a')
    a.href = URL.createObjectURL(blob)
    a.download = 'mwine-' + new Date().toISOString().replace(/[:.]/g, '-') + '.png'
    a.click()
    URL.revokeObjectURL(a.href)
    ElMessage.success('截图已下载')
  } catch (e) { ElMessage.error(e.message) }
}

let timer, infoTimer
onMounted(() => { refreshPerf(); loadDevInfo(); timer = setInterval(refreshPerf, 2500); infoTimer = setInterval(loadDevInfo, 20000) })
onUnmounted(() => {
  clearInterval(timer)
  if (infoTimer) clearInterval(infoTimer)
  if (liveTimer) clearInterval(liveTimer)
  if (player) { const p = player; player = null; p.stop() }
})
</script>
