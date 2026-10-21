<template>
  <el-card shadow="never" class="term-card">
    <template #header>
      <el-space wrap>
        <el-button type="primary" :disabled="open" @click="openTerm">打开会话</el-button>
        <el-button :disabled="!open" @click="sendCtrlC">Ctrl-C</el-button>
        <el-button @click="clearTerm">清屏</el-button>
        <el-button type="danger" :disabled="!open" @click="closeTerm">关闭会话</el-button>
        <div style="flex:1"></div>
        <span class="small muted">{{ stateText }}</span>
      </el-space>
    </template>
    <div ref="termHost" class="xterm-host"></div>
  </el-card>
</template>

<script setup>
import { ref, onMounted, onUnmounted } from 'vue'
import { ElMessage } from 'element-plus'
import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import '@xterm/xterm/css/xterm.css'
import { authState } from '../api'

const termHost = ref(null)
const open = ref(false)
const stateText = ref('未连接')

let term = null
let fit = null
let ws = null
let resizeObserver = null
let sessionGen = 0   // 会话代际：陈旧 socket 的 onclose 不得覆盖新会话状态

function wsUrl() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws'
  const { cols, rows } = term
  return `${proto}://${location.host}/api/term/ws?token=${encodeURIComponent(authState.token)}&cols=${cols}&rows=${rows}`
}

function openTerm() {
  if (ws) { try { ws.close() } catch {} }
  const gen = ++sessionGen
  const sock = new WebSocket(wsUrl())
  ws = sock
  sock.binaryType = 'arraybuffer'
  sock.onopen = () => {
    if (gen !== sessionGen) return
    open.value = true
    stateText.value = '已连接（pty）'
    term.focus()
  }
  sock.onmessage = (e) => {
    if (gen !== sessionGen) return
    if (typeof e.data === 'string') term.write(e.data)
    else term.write(new Uint8Array(e.data))
  }
  sock.onerror = () => { if (gen === sessionGen) stateText.value = '连接错误' }
  sock.onclose = () => {
    if (gen !== sessionGen) return     // 旧会话的迟到的 onclose 不覆盖新会话
    open.value = false
    stateText.value = '已断开'
  }
}

function closeTerm() {
  sessionGen++                          // 使旧 socket 的回调全部失效
  if (ws) { try { ws.close() } catch {} ws = null }
  open.value = false
  stateText.value = '未连接'
}

function sendCtrlC() { if (ws && open.value) ws.send('\x03') }
function clearTerm() { term && term.clear() }

onMounted(() => {
  term = new Terminal({
    fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace',
    fontSize: 13,
    cursorBlink: true,
    theme: { background: '#0a0c10', foreground: '#d1d5db' },
    scrollback: 5000,
  })
  fit = new FitAddon()
  term.loadAddon(fit)
  term.open(termHost.value)
  fit.fit()
  term.writeln('点击「打开会话」连接设备 adb shell。\x1b[90m（WebSocket + 真 PTY：颜色/光标/长驻命令全支持）\x1b[0m')
  term.onData((d) => { if (ws && open.value) ws.send(d) })
  // 尺寸同步：浏览器 resize → 设备 pty winsz
  resizeObserver = new ResizeObserver(() => {
    if (!fit) return
    fit.fit()
    if (ws && open.value) {
      ws.send(JSON.stringify({ resize: [term.cols, term.rows] }))
    }
  })
  resizeObserver.observe(termHost.value)
})

onUnmounted(() => {
  if (resizeObserver) resizeObserver.disconnect()
  closeTerm()
  if (term) { term.dispose(); term = null }
})
</script>

<style scoped>
.term-card :deep(.el-card__body) { padding: 10px; }
.xterm-host {
  height: 58vh;
  background: #0a0c10;
  border-radius: 10px;
  overflow: hidden;
  padding: 6px 0 0 8px;
}
</style>
