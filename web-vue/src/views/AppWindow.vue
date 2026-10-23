<template>
  <div class="app-window" :style="{ left: pos.x + 'px', top: pos.y + 'px', zIndex: z }"
       @pointerdown="bringFront">
    <div class="win-titlebar" @pointerdown="startDrag">
      <span class="win-live" :class="{ on: liveOn }" :title="status"></span>
      <span class="win-title">{{ title }}</span>
      <span class="win-pkg muted">{{ pkg }}</span>
      <div style="flex:1"></div>
      <button class="win-close" @pointerdown.stop @click="close" title="关闭（销毁副屏）">✕</button>
    </div>
    <div class="win-body" :style="{ width: size.w + 'px', height: size.h + 'px' }" ref="bodyRef">
      <div class="win-placeholder muted" v-if="!ready">⏳ 正在启动应用…</div>
      <input ref="imeRef" class="win-ime" autocomplete="off" autocapitalize="off"
             autocorrect="off" spellcheck="false" aria-label="输入法捕获">
      <div class="win-preedit" v-if="preedit">{{ preedit }}</div>
    </div>
    <!-- 底部导航栏：虚拟副屏无系统导航栏，窗口自带（注入该会话的控制通道） -->
    <div class="win-navbar">
      <button class="nav-btn" @pointerdown.prevent @click="key(4)" title="返回">◀</button>
      <button class="nav-btn" @pointerdown.prevent @click="key(3)" title="主页">●</button>
      <button class="nav-btn" @pointerdown.prevent @click="key(187)" title="多任务">■</button>
      <div style="flex:1"></div>
      <button class="nav-btn" @pointerdown.prevent @click="key(26)" title="电源">⏻</button>
    </div>
    <span class="win-resize" @pointerdown="startResize"></span>
  </div>
</template>

<script setup>
import { ref, onMounted, onUnmounted } from 'vue'
import { ElMessage } from 'element-plus'
import { ScrcpyWebPlayer } from '../player'
import { imgUrl } from '../api'

const props = defineProps({
  app: Object,        // {package, name}（应用独立窗口）
  deviceSerial: String, // 真机主屏镜像投屏
  titleText: String,  // 自定义标题（真机投屏用）
  offset: Number,     // 级联偏移（多窗错开）
  z: Number,
})
const emit = defineEmits(['close', 'front'])

const pos = ref({ x: 120 + (props.offset || 0) * 36, y: 40 + (props.offset || 0) * 30 })
// 初始高度不超出视口（标题栏 + 导航栏约 76px + 边距）
const initH = Math.min(640, Math.max(420, window.innerHeight - 160))
// 副屏尺寸按设备主屏宽高比定：TV/平板横屏给 1280×720，手机竖屏保持 720×1280
let displayW = 720, displayH = 1280, displayDpi = 320
const size = ref({ w: Math.round(initH * displayW / displayH), h: initH })
const status = ref('')
const liveOn = ref(false)
const preedit = ref('')
const imeRef = ref(null)
const ready = ref(false)
const bodyRef = ref(null)

const title = props.titleText || props.app?.name || props.app?.package || ''
const pkg = props.app?.package || ''

async function resolveDisplaySize() {
  try {
    const r = await fetch(imgUrl('/api/screenshot.png'))
    if (r.ok) {
      const blob = await r.blob()
      const img = await createImageBitmap(blob)
      if (img.width > img.height) {
        // 横屏设备（TV/平板）：副屏给横屏
        displayW = 1280; displayH = 720; displayDpi = 240
      }
      img.close()
    }
  } catch { /* 探测失败保持手机竖屏 */ }
  size.value = { w: Math.round(initH * displayW / displayH), h: initH }
}

let player = null
let canvas = null

onMounted(async () => {
  await resolveDisplaySize()
  const openQuery = props.deviceSerial
    ? `&serial=${encodeURIComponent(props.deviceSerial)}`
    : `&app=${encodeURIComponent(pkg)}&w=${displayW}&h=${displayH}&dpi=${displayDpi}`
  player = new ScrcpyWebPlayer({
    openQuery,
    onStatus: (t) => { status.value = t },
    onFirstFrame: () => {
      ready.value = true
      liveOn.value = true
      // 内容实际比例与窗口体不一致时（旋转/服务端约束）自适应贴合
      const cw = player.canvas.width, ch = player.canvas.height
      if (cw > 0 && ch > 0) {
        const contentRatio = cw / ch
        const bodyRatio = size.value.w / size.value.h
        if (Math.abs(contentRatio - bodyRatio) / bodyRatio > 0.02) {
          size.value = { w: Math.round(size.value.h * contentRatio), h: size.value.h }
        }
      }
    },
    onEnd: () => { liveOn.value = false },
  })
  // 找到即挂载 canvas
  canvas = player.canvas
  const box = bodyRef.value
  box.appendChild(canvas)
  // 宿主输入法捕获（隐藏输入框承载 IME 组合）
  player.attachIme(imeRef.value, { onPreedit: (t) => { preedit.value = t } })
  startClipboardSync()
  // 不 await：start() 会阻塞到流结束；失败由 catch 处理
  player.start().catch((e) => {
    ElMessage.error(`${title}：投屏失败 ${e.message}`)
    emit('close')
  })
})
onUnmounted(() => {
  stopClipboardSync()
  if (player) { const p = player; player = null; p.stop() }
})

function key(k) { player && player.key(k) }

// ---- 剪贴板双向共享 ----
// 设备 → 宿主：3s 轮询设备剪贴板，变化即写入宿主剪贴板（浏览器权限内尽力而为）
// 宿主 → 设备：窗口内粘贴事件已由 player 注入文本；这里另同步 setClipboard
let clipTimer = null
let lastClip = ''
function startClipboardSync() {
  clipTimer = setInterval(async () => {
    if (!player || !player.running) return
    try {
      const t = await player.getClipboard()
      if (t && t !== lastClip) {
        lastClip = t
        try { await navigator.clipboard.writeText(t) } catch { /* 权限/焦点限制忽略 */ }
      }
    } catch {}
  }, 3000)
}
function stopClipboardSync() {
  if (clipTimer) { clearInterval(clipTimer); clipTimer = null }
}

function close() {
  emit('close')
}
function bringFront() {
  emit('front')
}

// ---- 拖拽 ----
let drag = null
function startDrag(e) {
  drag = { sx: e.clientX, sy: e.clientY, ox: pos.value.x, oy: pos.value.y }
  const move = (ev) => {
    pos.value = {
      x: Math.max(0, Math.min(window.innerWidth - 120, drag.ox + ev.clientX - drag.sx)),
      y: Math.max(0, Math.min(window.innerHeight - 80, drag.oy + ev.clientY - drag.sy)),
    }
  }
  const up = () => {
    window.removeEventListener('pointermove', move)
    window.removeEventListener('pointerup', up)
  }
  window.addEventListener('pointermove', move)
  window.addEventListener('pointerup', up)
}

// ---- 缩放（右下角手柄；锁定内容比例） ----
function startResize(e) {
  e.preventDefault()
  const startW = size.value.w, startH = size.value.h
  const ratio = startH / startW
  const sx = e.clientX
  const move = (ev) => {
    const dw = ev.clientX - sx
    const w = Math.max(240, Math.min(720, startW + dw))
    size.value = { w, h: Math.round(w * ratio) }
  }
  const up = () => {
    window.removeEventListener('pointermove', move)
    window.removeEventListener('pointerup', up)
  }
  window.addEventListener('pointermove', move)
  window.addEventListener('pointerup', up)
}
</script>

<style scoped>
.app-window {
  position: fixed;
  background: #14171e;
  border: 1px solid #3a4152;
  border-radius: 12px;
  box-shadow: 0 18px 50px #000a;
  overflow: hidden;
  display: flex;
  flex-direction: column;
  min-width: 240px;
}
.win-titlebar {
  display: flex; align-items: center; gap: 8px;
  padding: 0 12px;
  height: 34px;                    /* 固定单行高度：fps 等状态文本不再引起抖动 */
  white-space: nowrap; overflow: hidden;
  background: #1c212b;
  border-bottom: 1px solid #2a3140;
  cursor: move;
  user-select: none;
  touch-action: none;
}
/* 直播状态点（悬停看状态文本），不随内容变形 */
.win-live {
  width: 8px; height: 8px; border-radius: 50%;
  background: #4a5265; flex-shrink: 0;
}
.win-live.on { background: #4ade80; box-shadow: 0 0 6px #4ade8088; }
.win-title { font-weight: 600; font-size: 13px; overflow: hidden; text-overflow: ellipsis; }
.win-pkg { font-size: 11px; overflow: hidden; text-overflow: ellipsis; }
.win-close {
  background: none; border: none; color: #9aa3b2; cursor: pointer;
  font-size: 14px; padding: 2px 6px; border-radius: 6px;
}
.win-close:hover { background: #f8717122; color: #fca5a5; }
.win-body {
  position: relative;
  background: #000;
  display: flex; align-items: center; justify-content: center;
}
.win-placeholder { position: absolute; }
/* IME 捕获输入框：不可见但可聚焦（display:none 会杀掉 IME） */
.win-ime {
  position: absolute; left: 2px; bottom: 2px;
  width: 2px; height: 2px; opacity: 0.01;
  border: none; background: transparent; color: transparent;
  caret-color: transparent; pointer-events: none;
}
/* 宿主 IME 组词浮层 */
.win-preedit {
  position: absolute; left: 12px; bottom: 12px;
  background: #1c212bee; border: 1px solid #3a4152; border-radius: 8px;
  color: #e6e9ef; padding: 4px 10px; font-size: 13px; z-index: 3;
  pointer-events: none;
}
/* 底部导航栏 */
.win-navbar {
  display: flex; align-items: center; gap: 4px;
  background: #1c212b;
  border-top: 1px solid #2a3140;
  padding: 4px 10px;
}
.nav-btn {
  background: none; border: none; color: #cbd5e1;
  font-size: 13px; width: 40px; height: 26px; border-radius: 7px; cursor: pointer;
}
.nav-btn:hover { background: #4a526533; }
.win-resize {
  position: absolute; right: 0; bottom: 0;
  width: 18px; height: 18px;
  cursor: nwse-resize;
  background: linear-gradient(135deg, transparent 50%, #4a5265 50%);
  border-bottom-right-radius: 12px;
  touch-action: none;
}
</style>
