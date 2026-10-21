<template>
  <div style="display:grid;gap:18px;">
    <el-card shadow="never">
      <template #header>虚拟机配置</template>
      <el-form :model="cfg" label-width="200px" label-position="left">
        <el-form-item label="设备形态">
          <el-radio-group v-model="cfg.mode">
            <el-radio-button value="phone">手机 1080×2400</el-radio-button>
            <el-radio-button value="tablet">平板 2560×1600</el-radio-button>
            <el-radio-button value="custom">自定义</el-radio-button>
          </el-radio-group>
        </el-form-item>
        <el-form-item v-if="cfg.mode === 'custom'" label="自定义尺寸">
          <el-space>
            <el-input-number v-model="cfg.width" :min="240" :max="4096" controls-position="right" style="width:110px" />
            ×
            <el-input-number v-model="cfg.height" :min="320" :max="4096" controls-position="right" style="width:110px" />
            @
            <el-input-number v-model="cfg.dpi" :min="120" :max="960" controls-position="right" style="width:100px" />
            DPI
          </el-space>
        </el-form-item>
        <el-form-item label="内存 MB（0=默认 2048）">
          <el-input-number v-model="cfg.memory_mb" :min="0" :max="16384" :step="512" controls-position="right" style="width:140px" />
        </el-form-item>
        <el-form-item label="CPU 核数（0=默认 4）">
          <el-input-number v-model="cfg.cpu_cores" :min="0" :max="16" controls-position="right" style="width:120px" />
        </el-form-item>
        <el-form-item label="GPU 后端">
          <el-select v-model="cfg.gpu_mode" style="width:240px">
            <el-option value="host" label="host（硬件加速）" />
            <el-option value="swiftshader_indirect" label="swiftshader（软渲兼容）" />
            <el-option value="swangle" label="swangle" />
          </el-select>
        </el-form-item>
        <el-form-item label="刷新率 Hz（0=默认 60）">
          <el-input-number v-model="cfg.vsync_rate" :min="0" :max="120" :step="30" controls-position="right" style="width:120px" />
        </el-form-item>
        <el-form-item label="HTTP 代理">
          <el-input v-model="cfg.proxy" placeholder="如 http://192.168.1.2:7890，空=不代理" style="width:320px" clearable />
        </el-form-item>
        <el-form-item label="自定义 DNS">
          <el-input v-model="cfg.dns_servers" placeholder="逗号分隔，空=默认" style="width:320px" clearable />
        </el-form-item>
        <el-form-item label="宿主共享目录（9p → /mnt/host）">
          <el-input v-model="cfg.share_dir" placeholder="空=不挂载" style="width:320px" clearable />
        </el-form-item>
        <el-form-item label="空闲自动关停（分钟，0=从不）">
          <el-input-number v-model="cfg.idle_stop_min" :min="0" :max="1440" controls-position="right" style="width:140px" />
        </el-form-item>
        <el-form-item label="语言覆盖">
          <el-input v-model="cfg.locale" placeholder="如 zh-CN；空=跟随宿主" style="width:200px" clearable />
        </el-form-item>
        <el-form-item label="启动行为">
          <el-space direction="vertical" alignment="flex-start">
            <el-switch v-model="cfg.no_snapshot" active-text="禁用快照（每次冷启动）" />
            <el-switch v-model="cfg.no_boot_anim" active-text="禁用启动动画" />
            <el-switch v-model="cfg.hw_keyboard" active-text="宿主键盘直通" />
            <el-switch v-model="cfg.allow_host_audio" active-text="允许使用宿主麦克风" />
          </el-space>
        </el-form-item>
        <el-form-item>
          <el-space>
            <el-button type="primary" :loading="saving" @click="save">保存配置</el-button>
            <span class="small muted">屏幕 / 内存 / GPU 修改后需重启虚拟机生效</span>
          </el-space>
        </el-form-item>
      </el-form>
    </el-card>

    <el-card shadow="never">
      <template #header>存档点（快照）</template>
      <el-space style="margin-bottom:12px;">
        <el-input v-model="snapName" placeholder="存档名（字母/数字/_/-）" style="width:240px" />
        <el-button type="primary" size="default" @click="saveSnap">保存当前状态</el-button>
      </el-space>
      <el-empty v-if="!snaps.length" description="无存档点" />
      <el-table v-else :data="snaps" style="width:100%">
        <el-table-column prop="name" label="名称" min-width="180" />
        <el-table-column label="时间" min-width="180">
          <template #default="{ row }">{{ row.mtime_ms ? new Date(row.mtime_ms).toLocaleString() : '' }}</template>
        </el-table-column>
        <el-table-column width="180" fixed="right">
          <template #default="{ row }">
            <el-button size="small" @click="loadSnap(row)">恢复</el-button>
            <el-popconfirm title="删除该存档？" @confirm="delSnap(row)">
              <template #reference><el-button size="small" type="danger" text>删除</el-button></template>
            </el-popconfirm>
          </template>
        </el-table-column>
      </el-table>
    </el-card>

    <el-card shadow="never">
      <template #header><span style="color:var(--el-color-danger)">危险操作</span></template>
      <el-space wrap>
        <el-button @click="resetIdentity">重置设备身份</el-button>
        <el-button type="danger" @click="resetVm">重置虚拟机（清空全部数据）</el-button>
      </el-space>
      <div class="small muted" style="margin-top:8px">重置设备身份：更换 android_id / 广告 ID，应用视作全新设备。</div>
    </el-card>
  </div>
</template>

<script setup>
import { ref, onMounted } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { apiGet, apiPost } from '../api'

const cfg = ref({})
const saving = ref(false)
const snaps = ref([])
const snapName = ref('')

async function loadCfg() {
  try { cfg.value = await apiGet('/api/config') } catch (e) { ElMessage.error(e.message) }
}

async function save() {
  saving.value = true
  try {
    await apiPost('/api/config', cfg.value)
    ElMessage.success('配置已保存')
  } catch (e) { ElMessage.error(e.message) }
  saving.value = false
}

async function loadSnaps() {
  try {
    const d = await apiGet('/api/snapshots')
    snaps.value = d.snapshots || []
  } catch { snaps.value = [] }
}

async function saveSnap() {
  const n = snapName.value.trim()
  if (!n) return ElMessage.error('请输入存档名')
  try {
    await apiPost('/api/snapshot/save', { name: n })
    ElMessage.success('存档已保存')
    snapName.value = ''
    loadSnaps()
  } catch (e) { ElMessage.error(e.message) }
}

async function loadSnap(row) {
  try {
    await ElMessageBox.confirm(`恢复到存档「${row.name}」？当前运行状态将被替换。`, '恢复存档', { type: 'warning' })
  } catch { return }
  try {
    await apiPost('/api/snapshot/load', { name: row.name })
    ElMessage.success('已恢复存档')
  } catch (e) { ElMessage.error(e.message) }
}

async function delSnap(row) {
  try {
    await apiPost('/api/snapshot/delete', { name: row.name })
    ElMessage.success('已删除')
    loadSnaps()
  } catch (e) { ElMessage.error(e.message) }
}

async function resetIdentity() {
  try {
    await ElMessageBox.confirm('重置设备身份（android_id / 广告 ID 换新）？', '重置身份', { type: 'warning' })
  } catch { return }
  try {
    await apiPost('/api/identity/reset')
    ElMessage.success('设备身份已重置')
  } catch (e) { ElMessage.error(e.message) }
}

async function resetVm() {
  try {
    await ElMessageBox.confirm('⚠ 确定重置虚拟机？全部应用、数据、快照都会被清空，不可恢复！', '重置虚拟机', { type: 'error', confirmButtonText: '我确定' })
  } catch { return }
  try {
    await apiPost('/api/vm/reset')
    ElMessage.success('虚拟机已重置')
  } catch (e) { ElMessage.error(e.message) }
}

onMounted(() => { loadCfg(); loadSnaps() })
</script>
