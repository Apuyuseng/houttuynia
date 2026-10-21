<template>
  <el-card shadow="never">
    <el-space wrap>
      <el-button type="primary" :disabled="open" @click="openTerm">打开会话</el-button>
      <el-button :disabled="!open" @click="ctrlC">Ctrl-C</el-button>
      <el-button @click="clearOut">清屏</el-button>
      <el-button type="danger" :disabled="!open" @click="closeTerm">关闭会话</el-button>
      <div style="flex:1"></div>
      <span class="small muted">{{ stateText }}</span>
    </el-space>
    <div class="term-box" ref="box" style="margin-top:12px;">{{ output }}</div>
    <el-input v-model="cmd" :disabled="!open" placeholder="输入命令回车执行（logcat、top 等长驻命令实时输出）"
              style="margin-top:10px" @keyup.enter="send">
      <template #append><el-button :disabled="!open" @click="send">发送</el-button></template>
    </el-input>
  </el-card>
</template>

<script setup>
import { ref, onUnmounted } from 'vue'
import { ElMessage } from 'element-plus'
import { apiPost, apiGet } from '../api'

const open = ref(false)
const stateText = ref('未连接')
const output = ref('点击「打开会话」连接设备 adb shell。')
const cmd = ref('')
const box = ref(null)

let id = 0
let cursor = 0
let timer = null

async function openTerm() {
  try {
    const r = await apiPost('/api/term/open')
    id = r.id
    cursor = 0
    open.value = true
    output.value = ''
    stateText.value = `会话 #${id} · ${r.serial}${r.pty ? ' · pty' : ''}`
    timer = setInterval(readOut, 900)
  } catch (e) { ElMessage.error(e.message) }
}

async function readOut() {
  if (!open.value) return
  try {
    const r = await apiGet(`/api/term/${id}/read?cursor=${cursor}`)
    cursor = r.cursor
    if (r.data) {
      output.value += r.data
      scrollDown()
    }
    if (!r.alive) {
      ElMessage.warning('会话已断开')
      closeTerm(true)
    }
  } catch { /* 会话失效 */ }
}

function scrollDown() {
  setTimeout(() => {
    const b = box.value
    if (b) b.scrollTop = b.scrollHeight
  }, 0)
}

async function send() {
  if (!cmd.value) return
  const c = cmd.value
  cmd.value = ''
  try { await apiPost(`/api/term/${id}/write`, { data: c + '\n' }) } catch (e) { ElMessage.error(e.message) }
}

async function ctrlC() {
  try { await apiPost(`/api/term/${id}/write`, { data: '\x03' }) } catch {}
}

function clearOut() { output.value = '' }

async function closeTerm(silent) {
  open.value = false
  clearInterval(timer)
  timer = null
  stateText.value = '未连接'
  const tid = id
  id = 0
  if (!silent) {
    try { await apiPost(`/api/term/${tid}/close`) } catch {}
  }
}

onUnmounted(() => { if (id) closeTerm() })
</script>
