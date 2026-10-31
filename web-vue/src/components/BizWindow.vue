<!-- 业务应用独立窗口：Android 自由窗口（freeform）形态。
     标题栏 = 应用图标 chip + 名称 + 最小化/最大化/关闭；可拖拽、右下角自由缩放。 -->
<template>
  <div class="win biz-win" :class="{ max: maximized }" :style="winStyle" @pointerdown="front">
    <div class="win-caption" @pointerdown="startDrag" @dblclick="toggleMax">
      <span class="cap-ico" :style="{ background: meta.tint }">
        <MsIcon :name="meta.icon" :size="14" />
      </span>
      <span class="cap-title">{{ win.title }}</span>
      <div style="flex:1"></div>
      <button class="cap-btn" title="最小化" @pointerdown.stop @click="$emit('minimize', win.key)">
        <MsIcon name="minimize" :size="13" />
      </button>
      <button class="cap-btn" :title="maximized ? '还原' : '最大化'" @pointerdown.stop @click="toggleMax">
        <MsIcon :name="maximized ? 'close_fullscreen' : 'open_in_full'" :size="13" />
      </button>
      <button class="cap-btn close" title="关闭" @pointerdown.stop @click="$emit('close', win.key)">
        <MsIcon name="close" :size="13" />
      </button>
    </div>
    <div class="biz-body" :class="{ flush: win.id === 'terminal' }">
      <component :is="meta.comp" />
    </div>
    <span v-if="!maximized" class="win-resize" @pointerdown="startResize"></span>
  </div>
</template>

<script setup>
import { ref, computed } from 'vue'
import { bizMeta } from '../windows'
import MsIcon from './MsIcon.vue'

const props = defineProps({
  win: Object,      // { key, id, title, z }
  offset: Number,   // 级联偏移（多窗错开）
})
const emit = defineEmits(['close', 'front', 'minimize'])

const meta = bizMeta(props.win.id)

const BAR = 36  // 顶部状态栏高度（窗口不压上去）
const pos = ref({
  x: Math.min(96 + (props.offset % 8) * 36, Math.max(24, window.innerWidth - meta.w - 24)),
  y: BAR + 16 + (props.offset % 8) * 28,
})
const size = ref({
  w: Math.min(meta.w, window.innerWidth - 32),
  h: Math.min(meta.h, window.innerHeight - BAR - 60),
})
const maximized = ref(false)
let savedRect = null

const winStyle = computed(() => maximized.value
  ? { left: '8px', top: BAR + 8 + 'px', width: 'calc(100vw - 16px)', height: 'calc(100vh - ' + (BAR + 16) + 'px)', zIndex: props.win.z }
  : { left: pos.value.x + 'px', top: pos.value.y + 'px', width: size.value.w + 'px', height: size.value.h + 'px', zIndex: props.win.z })

function front() { emit('front', props.win.key) }

function toggleMax() {
  maximized.value = !maximized.value
}

// ---- 拖拽 ----
function startDrag(e) {
  if (maximized.value || e.target.closest('.cap-btn')) return
  const drag = { sx: e.clientX, sy: e.clientY, ox: pos.value.x, oy: pos.value.y }
  const move = (ev) => {
    pos.value = {
      x: Math.max(-size.value.w + 160, Math.min(window.innerWidth - 120, drag.ox + ev.clientX - drag.sx)),
      y: Math.max(BAR, Math.min(window.innerHeight - 64, drag.oy + ev.clientY - drag.sy)),
    }
  }
  const up = () => {
    window.removeEventListener('pointermove', move)
    window.removeEventListener('pointerup', up)
  }
  window.addEventListener('pointermove', move)
  window.addEventListener('pointerup', up)
}

// ---- 缩放（右下角手柄，自由比例） ----
function startResize(e) {
  e.preventDefault()
  const startW = size.value.w, startH = size.value.h
  const sx = e.clientX, sy = e.clientY
  const move = (ev) => {
    size.value = {
      w: Math.max(320, Math.min(window.innerWidth - pos.value.x - 8, startW + ev.clientX - sx)),
      h: Math.max(240, Math.min(window.innerHeight - pos.value.y - 8, startH + ev.clientY - sy)),
    }
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
.biz-win {
  position: fixed;
  background: var(--m-surface-c, #1d1b20);
  border: 1px solid rgba(255, 255, 255, 0.1);
  border-radius: 16px;
  box-shadow: 0 12px 40px rgba(0, 0, 0, 0.5);
  overflow: hidden;
  display: flex; flex-direction: column;
}
.biz-win.max { border-radius: 12px; }
/* Android 桌面模式 caption bar */
.win-caption {
  display: flex; align-items: center; gap: 8px;
  padding: 0 8px 0 10px;
  height: 38px; flex-shrink: 0;
  background: rgba(255, 255, 255, 0.04);
  border-bottom: 1px solid rgba(255, 255, 255, 0.07);
  cursor: move; user-select: none; touch-action: none;
  white-space: nowrap; overflow: hidden;
}
.cap-ico {
  width: 22px; height: 22px; border-radius: 6px; flex-shrink: 0;
  display: flex; align-items: center; justify-content: center;
  color: #fff;
}
.cap-title { font-weight: 600; font-size: 13px; overflow: hidden; text-overflow: ellipsis; }
.cap-btn {
  background: none; border: none; cursor: pointer;
  color: rgba(255, 255, 255, 0.6);
  width: 30px; height: 26px; border-radius: 999px; flex-shrink: 0;
  display: inline-flex; align-items: center; justify-content: center;
}
.cap-btn:hover { background: rgba(255, 255, 255, 0.1); color: #fff; }
.cap-btn.close:hover { background: rgba(234, 67, 53, 0.25); color: #f6aea9; }
.biz-body {
  flex: 1; overflow-y: auto; padding: 16px;
}
/* 终端窗口：全幅贴合（Ubuntu 终端没有内边距卡片） */
.biz-body.flush { padding: 0; overflow: hidden; display: flex; flex-direction: column; }
.biz-body.flush > * { flex: 1; min-height: 0; }
.win-resize {
  position: absolute; right: 0; bottom: 0;
  width: 18px; height: 18px;
  cursor: nwse-resize;
  background: linear-gradient(135deg, transparent 50%, rgba(255, 255, 255, 0.22) 50%);
  border-bottom-right-radius: 16px;
  touch-action: none;
}
</style>
