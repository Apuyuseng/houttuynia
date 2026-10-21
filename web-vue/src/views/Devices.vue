<template>
  <div style="display:grid;gap:18px;">
    <!-- 已连接设备 -->
    <el-card shadow="never">
      <template #header>
        <div style="display:flex;align-items:center;">
          已连接设备
          <div style="flex:1"></div>
          <el-button size="small" @click="load">刷新</el-button>
        </div>
      </template>
      <el-empty v-if="!devices.length" description="暂无连接 —— 用下方任一方式连接手机" />
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

    <!-- 扫码配对（Mac 出码 · 手机扫） -->
    <el-card shadow="never">
      <template #header>扫码配对（Android 11+：手机「无线调试 → 使用二维码配对设备」扫这里）</template>
      <div v-if="!hp.state || hp.state === 'idle' || hp.state === 'failed' || hp.state === 'connected' || hp.state === 'cancelled'">
        <el-button type="primary" @click="beginQr">生成二维码</el-button>
        <el-alert v-if="hp.state === 'failed'" :title="hp.message" type="error" :closable="false" style="margin-top:10px;" />
      </div>
      <div v-else style="display:flex;gap:24px;align-items:flex-start;">
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
    </el-card>

    <el-row :gutter="18">
      <el-col :span="12">
        <el-card shadow="never">
          <template #header>配对码连接（手机「无线调试 → 使用配对码配对设备」）</template>
          <el-form label-width="90px">
            <el-form-item label="IP">
              <el-input v-model="pairForm.host" placeholder="192.168.1.5" />
            </el-form-item>
            <el-form-item label="配对端口">
              <el-input-number v-model="pairForm.port" :min="1" :max="65535" controls-position="right" style="width:160px" />
            </el-form-item>
            <el-form-item label="配对码">
              <el-input v-model="pairForm.code" placeholder="6 位数字" maxlength="6" />
            </el-form-item>
            <el-form-item label="连接端口">
              <el-input-number v-model="pairForm.connect_port" :min="0" :max="65535" controls-position="right" style="width:160px" />
              <div class="small muted">无线调试主界面显示的端口（空=自动发现/5555）</div>
            </el-form-item>
            <el-form-item>
              <el-button type="primary" :loading="pairing" @click="doPair">配对并连接</el-button>
            </el-form-item>
          </el-form>
        </el-card>
      </el-col>
      <el-col :span="12">
        <el-card shadow="never">
          <template #header>手动连接（手机已开无线调试）</template>
          <el-form label-width="90px">
            <el-form-item label="IP:端口">
              <el-input v-model="manualAddr" placeholder="192.168.1.5:5555" />
            </el-form-item>
            <el-form-item>
              <el-button type="primary" :loading="connecting" @click="doConnect">连接</el-button>
            </el-form-item>
          </el-form>
          <el-divider />
          <div style="display:flex;align-items:center;margin-bottom:8px;">
            <span class="small">局域网发现的设备</span>
            <div style="flex:1"></div>
            <el-button size="small" text @click="loadDiscovered">刷新</el-button>
          </div>
          <el-empty v-if="!discovered.length" description="未发现" :image-size="60" />
          <div v-for="s in discovered" :key="s.addr" class="disc-row">
            <span class="mono small">{{ s.addr }}</span>
            <span class="small muted">{{ s.name }}</span>
            <div style="flex:1"></div>
            <el-button size="small" @click="connectTo(s.addr)">连接</el-button>
          </div>
        </el-card>
      </el-col>
    </el-row>
  </div>

  <!-- 投屏窗口层 -->
  <AppWindow v-for="(w, i) in windows" :key="w.key" :deviceSerial="w.serial"
             :titleText="w.title" :offset="i" :z="w.z"
             @close="closeWindow(w.key)" @front="bringFront(w.key)" />
</template>

<script setup>
import { ref, onMounted, onUnmounted } from 'vue'
import { ElMessage } from 'element-plus'
import { apiGet, apiPost } from '../api'
import AppWindow from './AppWindow.vue'

const devices = ref([])
const discovered = ref([])
const hp = ref({})
const pairForm = ref({ host: '', port: null, code: '', connect_port: null })
const pairing = ref(false)
const connecting = ref(false)
const manualAddr = ref('')

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
  } catch (e) { ElMessage.error(e.message) }
}

async function loadDiscovered() {
  try {
    const d = await apiGet('/api/devices/discovered')
    discovered.value = (d.services || []).filter(s => s.type.startsWith('_adb._tcp'))
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
      host: f.host, port: f.port, code: f.code,
      connect_port: f.connect_port || undefined,
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

async function disconnect(row) {
  try {
    await apiPost('/api/devices/disconnect', { serial: row.serial })
    ElMessage.success('已断开')
    setTimeout(load, 800)
  } catch (e) { ElMessage.error(e.message) }
}

onMounted(() => { load(); loadDiscovered() })
onUnmounted(() => { windows.value = [] })
</script>

<style scoped>
.qr-box {
  background: #fff; padding: 10px; border-radius: 10px;
  display: inline-block; line-height: 0;
}
.qr-row { display: flex; }
.qr-cell { width: 5px; height: 5px; background: #fff; }
.qr-cell.on { background: #000; }
.disc-row {
  display: flex; align-items: center; gap: 10px;
  padding: 8px 4px; border-bottom: 1px solid var(--el-border-color);
}
.disc-row:last-child { border-bottom: none; }
</style>
