<template>
  <div style="display:grid;gap:18px;">
    <!-- 已连接设备 -->
    <el-card shadow="never">
      <template #header>
        <div style="display:flex;align-items:center;gap:10px;">
          <span>已连接设备</span>
          <div style="flex:1"></div>
          <el-button type="primary" @click="drawer = true">
            <MsIcon name="add_link" :size="15" style="margin-right:5px" />连接
          </el-button>
          <el-button @click="load">刷新</el-button>
        </div>
      </template>
      <el-empty v-if="!devices.length" description="暂无连接 —— 点「连接」选择 USB / 无线 / 扫码方式" />
      <el-table v-else :data="devices" style="width:100%">
        <el-table-column prop="serial" label="设备" min-width="200" />
        <el-table-column label="通道" width="110">
          <template #default="{ row }">
            <el-tag size="small" :type="row.kind === 'usb' ? 'primary' : 'success'">
              {{ row.kind === 'usb' ? 'USB' : row.kind === 'wifi' ? '无线' : '模拟器' }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column prop="state" label="状态" width="110" />
        <el-table-column label="电量 / 温度" min-width="130">
          <template #default="{ row }">
            <span v-if="info[row.serial]?.battery?.present">
              <el-tag size="small" :type="battType(row)" effect="plain">{{ info[row.serial].battery.level }}%</el-tag>
              <span v-if="info[row.serial].battery.status === 'charging'">⚡</span>
              <span class="small muted" style="margin-left:4px" v-if="info[row.serial].battery.temp_c">{{ info[row.serial].battery.temp_c.toFixed(1) }}°C</span>
            </span>
            <span v-else-if="info[row.serial]" class="small muted">供电正常</span>
            <span v-else class="small muted">…</span>
          </template>
        </el-table-column>
        <el-table-column label="管理目标" width="120">
          <template #default="{ row }">
            <el-tag v-if="row.current" size="small" type="success" effect="dark">当前</el-tag>
            <el-button v-else-if="row.state === 'device'" size="small" text type="primary"
                       @click="selectDevice(row)">设为当前</el-button>
          </template>
        </el-table-column>
        <el-table-column width="200" fixed="right">
          <template #default="{ row }">
            <el-button size="small" type="primary" @click="mirror(row)">投屏</el-button>
            <el-popconfirm title="断开该设备？" @confirm="disconnect(row)">
              <template #reference><el-button size="small" text type="danger">断开</el-button></template>
            </el-popconfirm>
          </template>
        </el-table-column>
      </el-table>
    </el-card>

    <!-- 历史连接：一键快连 -->
    <el-card shadow="never">
      <template #header>
        <div style="display:flex;align-items:center;gap:8px;">
          <span>历史连接</span>
          <span class="small muted">点击即可快速回连</span>
        </div>
      </template>
      <el-empty v-if="!known.length" description="暂无历史 —— 连接过的设备会出现在这里" :image-size="60" />
      <div v-else class="hist">
        <div v-for="s in known" :key="s" class="hchip" :class="{ on: connectedSet.has(s) }"
             @click="quickConnect(s)">
          <MsIcon :name="s.includes(':') ? 'wifi' : 'usb'" :size="15" />
          <span class="hserial">{{ s }}</span>
          <el-tag v-if="connectedSet.has(s)" size="small" type="success" effect="dark">在线</el-tag>
          <span class="hx" title="从历史中移除" @click.stop="forget(s)">
            <MsIcon name="close" :size="13" />
          </span>
        </div>
      </div>
    </el-card>

    <!-- 连接抽屉：USB / 扫码 / 配对码 / 手动 IP -->
    <el-drawer v-model="drawer" title="连接新设备" size="500px">
      <div style="display:grid;gap:20px;">
        <!-- USB -->
        <div>
          <div class="dsec"><MsIcon name="usb" :size="16" /> USB 设备 <span class="small muted">（插入即出现）</span></div>
          <el-empty v-if="!usbDevices.length" description="未检测到 USB 设备" :image-size="50" />
          <div v-else class="usblist">
            <div v-for="u in usbDevices" :key="u.serial" class="usbrow">
              <span class="mono small">{{ u.serial }}</span>
              <el-tag size="small" :type="u.state === 'device' ? 'success' : u.state === 'unauthorized' ? 'warning' : 'info'"
                      effect="plain">
                {{ u.state === 'device' ? '已连接' : u.state === 'unauthorized' ? '待授权' : u.state }}
              </el-tag>
              <div style="flex:1"></div>
              <span v-if="u.state === 'unauthorized'" class="small muted">请在手机屏幕允许 USB 调试</span>
            </div>
          </div>
          <div class="small muted" style="margin-top:6px">插入数据线后稍候即自动出现；首次使用需在手机上允许 USB 调试授权。</div>
        </div>

        <el-divider style="margin:4px 0" />

        <!-- 扫码配对 -->
        <div>
          <div class="dsec"><MsIcon name="qr_code_scanner" :size="16" /> 扫码配对（Android 11+：手机「无线调试 → 使用二维码配对设备」扫这里）</div>
          <div v-if="!hp.state || hp.state === 'idle' || hp.state === 'failed' || hp.state === 'connected' || hp.state === 'cancelled'">
            <el-button type="primary" @click="beginQr">生成二维码</el-button>
            <el-alert v-if="hp.state === 'failed'" :title="hp.message" type="error" :closable="false" style="margin-top:10px;" />
          </div>
          <div v-else style="display:flex;gap:20px;align-items:flex-start;">
            <div class="qr-box" v-if="hp.rows?.length">
              <div v-for="(row, i) in hp.rows" :key="i" class="qr-row">
                <span v-for="(c, j) in row.split('')" :key="j" class="qr-cell" :class="{ on: c === '1' }"></span>
              </div>
            </div>
            <div>
              <el-tag :type="hp.state === 'waiting' ? 'warning' : 'primary'" effect="dark">{{ hp.message }}</el-tag>
              <div class="small muted" style="margin-top:10px;">
                扫码后自动完成配对与连接，全程无需相机权限。<br>
                同一 Wi-Fi 下配对一次，之后自动回连。
              </div>
              <el-button v-if="hp.state === 'waiting'" size="small" style="margin-top:10px" @click="cancelQr">取消</el-button>
            </div>
          </div>
        </div>

        <el-divider style="margin:4px 0" />

        <!-- 配对码 + 手动 IP -->
        <el-row :gutter="16">
          <el-col :span="12">
            <div class="dsec"><MsIcon name="pin" :size="16" /> 配对码</div>
            <el-form label-width="70px" label-position="left" style="margin-top:8px">
              <el-form-item label="IP"><el-input v-model="pairForm.host" placeholder="192.168.1.5" /></el-form-item>
              <el-form-item label="端口"><el-input-number v-model="pairForm.port" :min="1" :max="65535" controls-position="right" style="width:130px" /></el-form-item>
              <el-form-item label="配对码"><el-input v-model="pairForm.code" placeholder="6 位数字" maxlength="6" /></el-form-item>
              <el-form-item>
                <el-button type="primary" :loading="pairing" @click="doPair">配对并连接</el-button>
              </el-form-item>
            </el-form>
          </el-col>
          <el-col :span="12">
            <div class="dsec"><MsIcon name="settings_ethernet" :size="16" /> 手动 IP</div>
            <el-form label-width="70px" label-position="left" style="margin-top:8px">
              <el-form-item label="IP:端口"><el-input v-model="manualAddr" placeholder="192.168.1.5:5555" /></el-form-item>
              <el-form-item>
                <el-button type="primary" :loading="connecting" @click="doConnect">连接</el-button>
              </el-form-item>
            </el-form>
            <el-divider style="margin:10px 0" />
            <div style="display:flex;align-items:center;margin-bottom:6px;">
              <span class="small">局域网发现</span>
              <div style="flex:1"></div>
              <el-button size="small" text @click="loadDiscovered">刷新</el-button>
            </div>
            <el-empty v-if="!discovered.length" description="未发现" :image-size="40" />
            <div v-for="s in discovered" :key="s.addr" class="disc-row">
              <span class="mono small">{{ s.addr }}</span>
              <el-tag size="small" type="info" style="margin-left:6px">{{ s.type.startsWith('_adb-tls-connect') ? '无线调试' : 'tcpip' }}</el-tag>
              <div style="flex:1"></div>
              <el-button size="small" @click="connectTo(s.addr)">连接</el-button>
            </div>
          </el-col>
        </el-row>
      </div>
    </el-drawer>

    <!-- 投屏窗口层 -->
    <AppWindow v-for="(w, i) in windows" :key="w.key" :deviceSerial="w.serial"
               :titleText="w.title" :offset="i" :z="w.z"
               @close="closeWindow(w.key)" @front="bringFront(w.key)" />
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { ElMessage } from 'element-plus'
import { apiGet, apiPost } from '../api'
import AppWindow from './AppWindow.vue'
import MsIcon from '../components/MsIcon.vue'

const devices = ref([])
const known = ref([])
const discovered = ref([])
const hp = ref({})
const pairForm = ref({ host: '', port: null, code: '', connect_port: null })
const pairing = ref(false)
const connecting = ref(false)
const manualAddr = ref('')
const drawer = ref(false)

// 设备信息（电量/温度；20s 慢轮询）
const info = ref({})
let infoTimer = null

const connectedSet = computed(() => new Set(devices.value.filter(d => d.state === 'device').map(d => d.serial)))
const usbDevices = computed(() => devices.value.filter(d => d.kind === 'usb'))

// 投屏窗口
const windows = ref([])
let winSeq = 0
let zTop = 200

function mirror(row) {
  const exist = windows.value.find(w => w.serial === row.serial)
  if (exist) return bringFront(exist.key)
  winSeq += 1
  windows.value.push({ key: winSeq, serial: row.serial, title: `📱 ${row.serial}`, z: ++zTop })
}
function closeWindow(key) { windows.value = windows.value.filter(w => w.key !== key) }
function bringFront(key) {
  const w = windows.value.find(w => w.key === key)
  if (w) w.z = ++zTop
}

async function load() {
  try {
    const d = await apiGet('/api/devices')
    devices.value = (d.devices || []).filter(x => x.kind !== 'emulator')
    known.value = d.known || []
    loadInfo()
  } catch (e) { ElMessage.error(e.message) }
}

async function loadInfo() {
  try { info.value = await apiGet('/api/device/info') } catch {}
}
function battType(row) {
  const lv = info.value[row.serial]?.battery?.level
  if (lv == null) return 'info'
  return lv <= 20 ? 'danger' : lv <= 50 ? 'warning' : 'success'
}

async function loadDiscovered() {
  try {
    const d = await apiGet('/api/devices/discovered')
    discovered.value = (d.services || []).filter(s => s.type.startsWith('_adb._tcp') || s.type.startsWith('_adb-tls-connect'))
  } catch { /* adb 缺失时静默 */ }
}

async function beginQr() {
  try {
    hp.value = await apiPost('/api/devices/hostpair/begin')
    pollHp()
  } catch (e) { ElMessage.error(e.message) }
}

async function pollHp() {
  while (true) {
    const st = await apiGet('/api/devices/hostpair')
    hp.value = st
    if (st.state === 'connected') {
      ElMessage.success(st.message || '已连接')
      load()
      return
    }
    if (st.state !== 'waiting' && st.state !== 'pairing' && st.state !== 'connecting') return
    await new Promise(r => setTimeout(r, 1500))
  }
}

async function cancelQr() {
  try { await apiPost('/api/devices/hostpair/cancel') } catch {}
}

async function doPair() {
  const f = pairForm.value
  if (!f.host || !f.port || !f.code) return ElMessage.error('请填 IP、配对端口、配对码')
  pairing.value = true
  try {
    const r = await apiPost('/api/devices/pair', {
      host: f.host, port: Number(f.port), code: f.code,
      connect_port: f.connect_port ? Number(f.connect_port) : undefined,
    })
    ElMessage.success('已配对并连接 ' + (r.device || ''))
    load()
  } catch (e) { ElMessage.error(e.message) }
  pairing.value = false
}

async function doConnect() { connectTo(manualAddr.value.trim()) }

async function connectTo(addr) {
  if (!addr || !addr.includes(':')) return ElMessage.error('格式：IP:端口')
  connecting.value = true
  try {
    await apiPost('/api/devices/connect', { addr })
    ElMessage.success('已连接 ' + addr)
    load()
  } catch (e) { ElMessage.error(e.message) }
  connecting.value = false
}

// 历史快连：无线序列号走 connect；USB 插入即在线无需连接
async function quickConnect(serial) {
  if (!serial.includes(':')) return ElMessage.info('USB 设备插入即自动连接')
  try {
    await apiPost('/api/devices/connect', { addr: serial })
    ElMessage.success('已连接 ' + serial)
    load()
  } catch (e) { ElMessage.error(e.message) }
}

async function forget(serial) {
  try {
    await apiPost('/api/devices/known/remove', { serial })
    known.value = known.value.filter(s => s !== serial)
  } catch (e) { ElMessage.error(e.message) }
}

async function disconnect(row) {
  try {
    await apiPost('/api/devices/disconnect', { serial: row.serial })
    ElMessage.success('已断开')
    setTimeout(load, 800)
  } catch (e) { ElMessage.error(e.message) }
}

// 多设备在线时选定管理目标
async function selectDevice(row) {
  try {
    await apiPost('/api/devices/select', { serial: row.serial })
    ElMessage.success('已切换管理目标：' + row.serial)
    load()
  } catch (e) { ElMessage.error(e.message) }
}

onMounted(() => { load(); loadDiscovered(); infoTimer = setInterval(loadInfo, 20000) })
onUnmounted(() => { windows.value = []; if (infoTimer) clearInterval(infoTimer) })
</script>

<style scoped>
.hist { display: flex; flex-wrap: wrap; gap: 10px; }
.hchip {
  display: inline-flex; align-items: center; gap: 7px;
  padding: 7px 12px; border-radius: 12px; cursor: pointer;
  background: var(--el-fill-color-light); border: 1px solid var(--el-border-color);
}
.hchip:hover { border-color: var(--el-color-primary); }
.hchip.on { border-color: var(--el-color-success); }
.hserial { font-size: 12.5px; font-family: ui-monospace, Menlo, monospace; }
.hx { color: var(--el-text-color-secondary); border-radius: 50%; padding: 2px; }
.hx:hover { color: var(--el-color-danger); background: var(--el-fill-color); }

.dsec { display: flex; align-items: center; gap: 6px; font-weight: 600; font-size: 13.5px; }
.usblist { display: grid; gap: 6px; margin-top: 8px; }
.usbrow { display: flex; align-items: center; gap: 10px; padding: 7px 10px;
          background: var(--el-fill-color-light); border-radius: 10px; }

.qr-box {
  background: #fff; padding: 10px; border-radius: 10px;
  display: inline-block; line-height: 0;
}
.qr-row { display: flex; }
.qr-cell { width: 5px; height: 5px; background: #fff; }
.qr-cell.on { background: #000; }
.disc-row {
  display: flex; align-items: center; gap: 10px;
  padding: 6px 4px; border-bottom: 1px solid var(--el-border-color);
}
.disc-row:last-child { border-bottom: none; }
</style>
