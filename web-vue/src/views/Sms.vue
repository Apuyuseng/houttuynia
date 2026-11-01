<!-- 短信：会话列表 + 详情两栏（Android 短信 App 观感）。
     数据经设备 content://sms（adb 实时读取）；发送为 smsto 意图预填（设备侧确认）。 -->
<template>
  <div class="sms">
    <div class="sms-list">
      <div class="sms-toolbar">
        <el-radio-group v-model="box" size="small" @change="load">
          <el-radio-button value="all">全部</el-radio-button>
          <el-radio-button value="inbox">收件</el-radio-button>
          <el-radio-button value="sent">已发</el-radio-button>
        </el-radio-group>
        <el-button size="small" circle :icon="Refresh" :loading="loading" @click="load" title="刷新" />
      </div>
      <div class="sms-items" v-loading="loading">
        <div class="sms-empty" v-if="!msgs.length && !loading">暂无短信（设备未插卡或已清空）</div>
        <div class="sms-item" v-for="m in msgs" :key="m._id"
             :class="{ sel: sel === m }" @click="sel = m">
          <div class="sms-ava" :class="{ out: m.type === '2' }">{{ (m.address || '?').slice(-2) }}</div>
          <div class="sms-m">
            <div class="sms-r1">
              <span class="sms-addr">{{ m.address || '未知' }}</span>
              <span class="sms-time">{{ fmtTime(m.date) }}</span>
            </div>
            <div class="sms-body1">{{ m.body }}</div>
          </div>
        </div>
      </div>
    </div>
    <div class="sms-detail">
      <template v-if="sel">
        <div class="sms-dhead">
          <div>
            <div class="sms-daddr">{{ sel.address }}</div>
            <div class="sms-dsub mono">thread {{ sel.thread_id }} · {{ typeName(sel.type) }}</div>
          </div>
        </div>
        <div class="sms-dbody">{{ sel.body }}</div>
        <div class="sms-reply">
          <el-input v-model="reply" type="textarea" :rows="3" placeholder="回复内容（将在设备上打开短信编辑预填，点发送确认）" />
          <el-button type="primary" :disabled="!reply.trim()" @click="send">在设备上发送…</el-button>
        </div>
      </template>
      <div class="sms-empty" v-else>选择左侧短信查看详情</div>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted, onUnmounted, inject } from 'vue'
import { ElMessage } from 'element-plus'
import { Refresh } from '@element-plus/icons-vue'
import { apiGet, apiPost } from '../api'
import { onDeviceEvent } from '../sseDevice'

const { deviceSerial } = inject('vmState')
const box = ref('all')
const msgs = ref([])
const loading = ref(false)
const sel = ref(null)
const reply = ref('')
let offNew = null

async function load() {
  loading.value = true
  try {
    const d = await apiGet(`/api/sms/list?box=${box.value}&limit=300`)
    msgs.value = d.msgs || []
  } catch (e) {
    ElMessage.error(e.message || '读取失败')
  } finally {
    loading.value = false
  }
}

// 新短信通知（桥 SSE）→ 自动刷新列表 + toast
offNew = onDeviceEvent('notification', (n) => {
  if (!n.title && !n.text) return
  if ((n.title || '').includes('短信') || (n.pkg || '').includes('telecom|sms|mms')) load()
})

function fmtTime(d) {
  const t = Number(d)
  if (!t) return ''
  const dt = new Date(t)
  const today = new Date().toDateString() === dt.toDateString()
  return today
    ? dt.toLocaleTimeString('zh-CN', { hour: '2-digit', minute: '2-digit' })
    : dt.toLocaleDateString('zh-CN', { month: 'numeric', day: 'numeric' })
}
function typeName(t) {
  return { '1': '收件', '2': '已发', '3': '草稿', '5': '发送中', '6': '失败' }[t] || '短信'
}
async function send() {
  try {
    const r = await apiPost('/api/sms/send', { to: sel.value.address, body: reply.value })
    ElMessage.success(r.message || '已在设备打开')
    reply.value = ''
  } catch (e) {
    ElMessage.error(e.message || '发送失败')
  }
}

onMounted(load)
onUnmounted(() => { if (offNew) offNew() })
</script>

<style scoped>
.sms { display: flex; height: 100%; min-height: 0; }
.sms-list { width: 46%; min-width: 220px; border-right: 1px solid var(--el-border-color-lighter); display: flex; flex-direction: column; }
.sms-toolbar { display: flex; align-items: center; gap: 8px; padding: 8px 10px; border-bottom: 1px solid var(--el-border-color-lighter); }
.sms-toolbar .el-button { margin-left: auto; }
.sms-items { flex: 1; overflow-y: auto; }
.sms-empty { padding: 30px 16px; text-align: center; color: var(--el-text-color-secondary); font-size: 13px; }
.sms-item {
  display: flex; gap: 10px; padding: 10px 12px; cursor: pointer;
  border-bottom: 1px solid var(--el-border-color-extra-light);
}
.sms-item:hover { background: rgba(255,255,255,0.05); }
.sms-item.sel { background: rgba(66,133,244,0.16); }
.sms-ava {
  width: 34px; height: 34px; border-radius: 50%; flex-shrink: 0;
  background: #34a853; color: #fff; font-size: 12px; font-weight: 700;
  display: inline-flex; align-items: center; justify-content: center;
}
.sms-ava.out { background: #4285f4; }
.sms-m { flex: 1; min-width: 0; }
.sms-r1 { display: flex; gap: 8px; align-items: baseline; }
.sms-addr { font-weight: 600; font-size: 13px; }
.sms-time { margin-left: auto; font-size: 11px; color: var(--el-text-color-secondary); flex-shrink: 0; }
.sms-body1 {
  font-size: 12px; color: var(--el-text-color-secondary);
  overflow: hidden; text-overflow: ellipsis; white-space: nowrap; margin-top: 2px;
}
.sms-detail { flex: 1; display: flex; flex-direction: column; min-width: 0; }
.sms-dhead { padding: 12px 14px; border-bottom: 1px solid var(--el-border-color-lighter); }
.sms-daddr { font-weight: 700; font-size: 15px; }
.sms-dsub { font-size: 11px; color: var(--el-text-color-secondary); margin-top: 2px; }
.sms-dbody { flex: 1; overflow-y: auto; padding: 14px; white-space: pre-wrap; font-size: 14px; line-height: 1.6; }
.sms-reply { padding: 10px 12px; border-top: 1px solid var(--el-border-color-lighter); display: flex; flex-direction: column; gap: 8px; }
</style>
