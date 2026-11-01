<!-- Ubuntu 终端形态：打开即自动连接，无按钮堆。标题栏仅 Ctrl-C/清屏/断开。
     窗口体即终端全幅（无卡片嵌套），底色一体化，字距行高贴近 GNOME Terminal。 -->
<template>
  <div class="ubt">
    <div class="ubt-toolbar">
      <button class="ubt-btn" :disabled="!open" @click="sendCtrlC" title="发送 Ctrl-C">^C</button>
      <button class="ubt-btn" :disabled="!open" @click="clearTerm" title="清屏">清屏</button>
      <div class="ubt-spring"></div>
      <span class="ubt-state" :class="{ on: open }">{{ stateText }}</span>
      <button class="ubt-btn danger" :disabled="!open" @click="closeTerm">断开</button>
    </div>
    <div ref="termHost" class="ubt-host"></div>
  </div>
</template>

<script setup>
import { ref, onMounted, onUnmounted } from 'vue'
import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import '@xterm/xterm/css/xterm.css'
import { authState } from '../api'

const termHost = ref(null)
const open = ref(false)
const stateText = ref('连接中…')

let term = null
let fit = null
let ws = null
let resizeObserver = null
let sessionGen = 0   // 会话代际：陈旧 socket 的 onclose 不得覆盖新会话

function wsUrl() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws'
  const { cols, rows } = term
  return `${proto}://${location.host}/api/term/ws?token=${encodeURIComponent(authState.token)}&cols=${cols}&rows=${rows}`
}

// Ubuntu 形态的核心：窗口挂载即自动连接，失败 4s 后自动重试
function connect() {
  if (ws) { try { ws.close() } catch {} }
  const gen = ++sessionGen
  const sock = new WebSocket(wsUrl())
  ws = sock
  sock.binaryType = 'arraybuffer'
  sock.onopen = () => {
    if (gen !== sessionGen) return
    open.value = true
    stateText.value = '已连接'
    term.focus()
  }
  sock.onmessage = (e) => {
    if (gen !== sessionGen) return
    if (typeof e.data === 'string') term.write(e.data)
    else term.write(new Uint8Array(e.data))
  }
  sock.onerror = () => { if (gen === sessionGen) stateText.value = '连接错误' }
  sock.onclose = () => {
    if (gen !== sessionGen) return     // 旧会话的迟到 onclose 不覆盖新会话
    open.value = false
    stateText.value = '已断开 · 4s 后重连'
    setTimeout(() => { if (gen === sessionGen && !open.value) connect() }, 4000)
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
    fontFamily: 'Ubuntu Mono, ui-monospace, SFMono-Regular, Menlo, Consolas, monospace',
    fontSize: 14,
    lineHeight: 1.2,
    cursorBlink: true,
    cursorStyle: 'bar',
    // GNOME Terminal (Ambiance) 配色
    theme: {
      background: '#300a24', foreground: '#ffffff',
      cursor: '#ffffff', cursorAccent: '#300a24',
      selectionBackground: '#5e4a66',
      black: '#2e3436', red: '#cc0000', green: '#4e9a06', yellow: '#c4a000',
      blue: '#3465a4', magenta: '#75507b', cyan: '#06989a', white: '#d3d7cf',
      brightBlack: '#555753', brightRed: '#ef2929', brightGreen: '#8ae234',
      brightYellow: '#fce94f', brightBlue: '#729fcf', brightMagenta: '#ad7fa8',
      brightCyan: '#34e2e2', brightWhite: '#eeeeec',
    },
    scrollback: 8000,
    allowProposedApi: true,
  })
  fit = new FitAddon()
  term.loadAddon(fit)
  term.open(termHost.value)
  fit.fit()
  connect()
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
.ubt {
  display: flex; flex-direction: column;
  height: 100%; min-height: 0;
  background: #300a24;
}
/* 工具条：极简、融入终端底色（Ubuntu 终端菜单栏观感） */
.ubt-toolbar {
  display: flex; align-items: center; gap: 6px;
  padding: 4px 10px;
  background: rgba(0, 0, 0, 0.28);
  border-bottom: 1px solid rgba(255, 255, 255, 0.06);
  flex-shrink: 0;
}
.ubt-btn {
  background: rgba(255, 255, 255, 0.07);
  border: 1px solid rgba(255, 255, 255, 0.1);
  color: #e8e2ee;
  font-size: 12px; line-height: 1;
  padding: 4px 10px; border-radius: 6px;
  cursor: pointer;
}
.ubt-btn:hover:not(:disabled) { background: rgba(255, 255, 255, 0.14); }
.ubt-btn:disabled { opacity: 0.4; cursor: default; }
.ubt-btn.danger:hover:not(:disabled) { background: rgba(204, 0, 0, 0.35); }
.ubt-spring { flex: 1; }
.ubt-state { font-size: 11px; color: #a89bab; }
.ubt-state.on { color: #8ae234; }
/* 终端体：占满窗口剩余空间，零内边距嵌套 */
.ubt-host {
  flex: 1; min-height: 0;
  padding: 6px 0 0 10px;
  background: #300a24;
}
.ubt-host :deep(.xterm) { height: 100%; }
.ubt-host :deep(.xterm-viewport) { background-color: transparent !important; }
</style>
