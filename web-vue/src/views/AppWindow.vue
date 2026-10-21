<template>
  <div class="app-window" :style="{ left: pos.x + 'px', top: pos.y + 'px', zIndex: z }"
       @pointerdown="bringFront">
    <div class="win-titlebar" @pointerdown="startDrag">
      <span class="win-title">{{ title }}</span>
      <span class="win-pkg muted">{{ pkg }}</span>
      <div style="flex:1"></div>
      <span class="win-status muted">{{ status }}</span>
      <button class="win-close" @pointerdown.stop @click="close" title="关闭（销毁副屏）">✕</button>
    </div>
    <div class="win-body" :style="{ width: size.w + 'px', height: size.h + 'px' }" ref="bodyRef">
      <div class="win-placeholder muted" v-if="!ready">⏳ 正在启动应用…</div>
    </div>
    <span class="win-resize" @pointerdown="startResize"></span>
  </div>
</template>

<script setup>
import { ref, onMounted, onUnmounted } from 'vue'
import { ElMessage } from 'element-plus'
import { ScrcpyWebPlayer } from '../player'

const props = defineProps({
  app: Object,        // {package, name}
  offset: Number,     // 级联偏移（多窗错开）
  z: Number,
})
const emit = defineEmits(['close', 'front'])

const pos = ref({ x: 120 + (props.offset || 0) * 36, y: 40 + (props.offset || 0) * 30 })
const size = ref({ w: 380, h: 640 })   // 竖屏应用默认宽高（内容按比例 contain）
const status = ref('')
const ready = ref(false)
const bodyRef = ref(null)

const title = props.app?.name || props.app?.package || ''
const pkg = props.app?.package || ''

let player = null
let canvas = null

onMounted(async () => {
  player = new ScrcpyWebPlayer({
    openQuery: `&app=${encodeURIComponent(pkg)}&w=720&h=1280&dpi=320`,
    onStatus: (t) => { status.value = t },
    onEnd: () => {},
  })
  // 找到即挂载 canvas
  canvas = player.canvas
  const box = bodyRef.value
  box.appendChild(canvas)
  try {
    await player.start()
    ready.value = true
  } catch (e) {
    ElMessage.error(`${title}：投屏失败 ${e.message}`)
    emit('close')
  }
})
onUnmounted(() => { if (player) { const p = player; player = null; p.stop() } })

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
  padding: 8px 12px;
  background: #1c212b;
  border-bottom: 1px solid #2a3140;
  cursor: move;
  user-select: none;
  touch-action: none;
}
.win-title { font-weight: 600; font-size: 13px; }
.win-pkg { font-size: 11px; }
.win-status { font-size: 11px; }
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
.win-body canvas.stream-canvas {
  width: 100%; height: 100%; object-fit: contain;
}
.win-placeholder { position: absolute; }
.win-resize {
  position: absolute; right: 0; bottom: 0;
  width: 18px; height: 18px;
  cursor: nwse-resize;
  background: linear-gradient(135deg, transparent 50%, #4a5265 50%);
  border-bottom-right-radius: 12px;
  touch-action: none;
}
</style>
