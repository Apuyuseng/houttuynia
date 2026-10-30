<template>
  <div class="app-window" :style="{ left: pos.x + 'px', top: pos.y + 'px', zIndex: z }"
       @pointerdown="bringFront">
    <div class="win-titlebar" @pointerdown="startDrag">
      <span class="win-live" :class="{ on: liveOn }" :title="status"></span>
      <span class="win-title">{{ title }}</span>
      <span class="win-pkg muted">{{ pkg }}</span>
      <div style="flex:1"></div>
      <button class="win-btn" @pointerdown.stop @click="$emit('minimize')" title="最小化">
        <MsIcon name="minimize" :size="13" />
      </button>
      <button class="win-btn close" @pointerdown.stop @click="close" title="关闭（销毁副屏）">
        <MsIcon name="close" :size="13" />
      </button>
    </div>
    <div class="win-body" :style="{ width: size.w + 'px', height: size.h + 'px' }" ref="bodyRef">
      <div class="win-placeholder muted" v-if="!ready">⏳ 正在启动应用…</div>
      <!-- 副屏无内容（设备/应用不支持虚拟屏独立窗口）→ 提供主屏回退 -->
      <div class="win-fallback" v-if="noDisplay">
        <div class="wf-icon"><MsIcon name="smartphone" :size="26" /></div>
        <div class="wf-title">{{ noDisplayReason }}</div>
        <div class="wf-sub muted small">并非所有设备/应用都支持独立窗口（虚拟屏）：Android 10 以下、
          部分厂商 ROM 及强制主屏的应用（如银行类）会受限</div>
        <div class="wf-actions">
          <el-button size="small" type="primary" @click="runOnMainDisplay">在手机主屏打开</el-button>
          <el-button size="small" text @click="noDisplay = false">继续等待</el-button>
        </div>
      </div>
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
import { imgUrl, apiPost } from '../api'
import { getFlags } from '../flags'
import { onDeviceEvent } from '../sseDevice'
import MsIcon from '../components/MsIcon.vue'

const props = defineProps({
  app: Object,        // {package, name}（应用独立窗口）
  deviceSerial: String, // 真机主屏镜像投屏
  titleText: String,  // 自定义标题（真机投屏用）
  offset: Number,     // 级联偏移（多窗错开）
  z: Number,
})
const emit = defineEmits(['close', 'front', 'minimize'])

const BAR = 36  // 顶部状态栏高度（窗口不压上去）
const pos = ref({
  x: Math.min(96 + (props.offset % 8) * 36, Math.max(24, window.innerWidth - 380)),
  y: BAR + 16 + (props.offset % 8) * 28,
})
// 初始高度不超出视口（标题栏 + 导航栏约 76px + 边距）
const initH = Math.min(920, Math.max(420, Math.round(window.innerHeight * 0.72)))
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
  if (props.deviceSerial) {
    // 镜像模式：直接按截图真实宽高比开窗（荣耀 9:20 / 平板 4:3 / TV 16:9
    // 各不相同，横竖二分假设 9:16 会先错后跳）
    try {
      const r = await fetch(imgUrl('/api/screenshot.png'))
      if (r.ok) {
        const img = await createImageBitmap(await r.blob())
        if (img.width > 0 && img.height > 0) {
          displayW = img.width; displayH = img.height
        }
        img.close()
      }
    } catch { /* 探测失败保持默认竖屏 */ }
    size.value = { w: Math.round(initH * displayW / displayH), h: initH }
    return
  }
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
let offClip = null

// ---- 独立窗口兼容性检测：并非所有设备都支持虚拟屏（Android<10 / 部分 ROM /
// ---- 应用强制主屏）。两种失败形态：a) 首帧迟迟不来 b) 副屏全黑（创建成功
// ---- 但应用没落上去）。检测到即给「在手机主屏打开」回退
const noDisplay = ref(false)
const noDisplayReason = ref('')
let detectTimer = null
function armNoDisplayDetect() {
  if (props.deviceSerial) return // 主屏镜像不需要
  // a) 12s 无首帧
  detectTimer = setTimeout(() => {
    if (!ready.value) {
      noDisplayReason.value = '独立窗口长时间无画面'
      noDisplay.value = true
    }
  }, 12000)
}
function checkBlackFrame() {
  if (props.deviceSerial || !canvas) return
  // b) 副屏内容均匀（全黑/纯壁纸无差异）：缩到 16×32 采样方差
  try {
    const c = document.createElement('canvas')
    c.width = 16; c.height = 32
    const g = c.getContext('2d', { willReadFrequently: true })
    g.drawImage(canvas, 0, 0, 16, 32)
    const d = g.getImageData(0, 0, 16, 32).data
    let sum = 0, sum2 = 0, n = d.length / 4
    for (let i = 0; i < d.length; i += 4) {
      const v = (d[i] + d[i + 1] + d[i + 2]) / 3
      sum += v; sum2 += v * v
    }
    const variance = sum2 / n - (sum / n) * (sum / n)
    if (variance < 4) { // 近乎纯色（虚拟屏空内容典型表现）
      noDisplayReason.value = '副屏无内容（应用未在独立窗口中打开）'
      noDisplay.value = true
    }
  } catch { /* 采样失败忽略 */ }
}
async function runOnMainDisplay() {
  try {
    await apiPost(`/api/apps/${encodeURIComponent(pkg)}/run`, {})
    ElMessage.success(`已在手机主屏打开 ${title}（可用「屏幕镜像」远程操作）`)
    emit('close')
  } catch (e) {
    ElMessage.error(`主屏启动失败：${e.message || e}`)
  }
}

onMounted(async () => {
  await resolveDisplaySize()
  // 真机主屏镜像：原始分辨率直投（max_size=0 = scrcpy 不缩放；默认档会
  // 把 720×1600 压到 576×1280，画面明显发糊）。USB 直连带宽足够 8M。
  const flags = getFlags()
  const audioPart = flags.audio_stream ? '' : '&audio=0'
  const openQuery = props.deviceSerial
    ? `&serial=${encodeURIComponent(props.deviceSerial)}&max_size=0&bit_rate=20000000&fps=30${audioPart}`
    : `&app=${encodeURIComponent(pkg)}&w=${displayW}&h=${displayH}&dpi=${displayDpi}`
  player = new ScrcpyWebPlayer({
    openQuery,
    onStatus: (t) => { status.value = t },
    onFirstFrame: () => {
      ready.value = true
      liveOn.value = true
      if (detectTimer) { clearTimeout(detectTimer); detectTimer = null }
      // 首帧后再等 4s 采样：应用正常落屏需要启动时间，太早会误判
      setTimeout(checkBlackFrame, 4000)
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
  if (props.deviceSerial && getFlags().clipboard_sync) {
    // SSE 剪贴板推送（桥/投屏会话实时事件）；组件卸载时随 SSE 单例常驻，
    // 不额外清理（handlers 列表极小）
    offClip = onDeviceEvent('clipboard', (d) => {
      if (d.text && d.text !== lastClip) {
        lastClip = d.text
        navigator.clipboard.writeText(d.text).catch(() => {})
      }
    })
  }
  // 不 await：start() 会阻塞到流结束；失败由 catch 处理
  player.start().catch((e) => {
    ElMessage.error(`${title}：投屏失败 ${e.message}`)
    emit('close')
  })
  armNoDisplayDetect()
})
onUnmounted(() => {
  if (detectTimer) { clearTimeout(detectTimer); detectTimer = null }
  stopClipboardSync()
  if (offClip) { offClip(); offClip = null }
  if (player) { const p = player; player = null; p.stop() }
})

// ---- 会话主动回收（后端空闲看门狗兜底，这里尽力即时关）----
// 1) 页面刷新/关闭：sendBeacon 异步 close（不阻塞卸载）
// 2) 长时间隐藏（切后台标签 >60s）：主动关会话省 USB 带宽与设备编码开销；
//    回到前台重新开流（该窗口的 attached 已置位，需新建会话——openStream 重入即可）
let sessionId = null
const origOpen = ScrcpyWebPlayer.prototype.start
window.addEventListener('beforeunload', () => {
  const id = player?.id
  if (!id) return
  // sendBeacon 不带自定义 header——token 走 query（网关 auth 中间件兼容 query token）
  const blob = new Blob([JSON.stringify({})], { type: 'application/json' })
  navigator.sendBeacon?.(
    `/api/stream/${id}/close?token=${encodeURIComponent(authState.token)}`, blob
  )
})
let hideTimer = null
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'hidden' && player) {
    // 隐藏 60s 后主动关会话（省设备编码开销；看门狗 90s 兜底）
    hideTimer = setTimeout(() => {
      const p = player
      player = null
      liveOn.value = false
      p?.stop()
    }, 60000)
  }
  if (document.visibilityState === 'visible' && !player) {
    if (hideTimer) { clearTimeout(hideTimer); hideTimer = null }
    // 从后台回到前台：重开投屏（原会话已被可见性守卫或看门狗回收）
    player = new ScrcpyWebPlayer({
      openQuery: props.deviceSerial
        ? `&serial=${encodeURIComponent(props.deviceSerial)}&max_size=0&bit_rate=20000000&fps=30`
        : `&app=${encodeURIComponent(pkg)}&w=${displayW}&h=${displayH}&dpi=${displayDpi}`,
      onStatus: (t) => { status.value = t },
      onFirstFrame: () => { ready.value = true; liveOn.value = true },
      onEnd: () => { liveOn.value = false },
    })
    bodyRef.value.appendChild(player.canvas)
    if (props.deviceSerial && getFlags().clipboard_sync) {
      offClip = onDeviceEvent('clipboard', (d) => {
        if (d.text && d.text !== lastClip) {
          lastClip = d.text
          navigator.clipboard.writeText(d.text).catch(() => {})
        }
      })
    }
    player.start().catch(() => {})
  }
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
      y: Math.max(36, Math.min(window.innerHeight - 80, drag.oy + ev.clientY - drag.sy)),
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
  background: #141318;
  border: 1px solid rgba(255, 255, 255, 0.1);
  border-radius: 16px;
  box-shadow: 0 12px 40px rgba(0, 0, 0, 0.5);
  overflow: hidden;
  display: flex;
  flex-direction: column;
  min-width: 240px;
}
/* Android 桌面模式 caption bar */
.win-titlebar {
  display: flex; align-items: center; gap: 8px;
  padding: 0 8px 0 12px;
  height: 38px;                    /* 固定单行高度：fps 等状态文本不再引起抖动 */
  white-space: nowrap; overflow: hidden;
  background: rgba(255, 255, 255, 0.04);
  border-bottom: 1px solid rgba(255, 255, 255, 0.07);
  cursor: move;
  user-select: none;
  touch-action: none;
}
/* 直播状态点（悬停看状态文本），不随内容变形 */
.win-live {
  width: 8px; height: 8px; border-radius: 50%;
  background: rgba(255, 255, 255, 0.25); flex-shrink: 0;
}
.win-live.on { background: #81c995; box-shadow: 0 0 6px #81c99588; }
.win-title { font-weight: 600; font-size: 13px; overflow: hidden; text-overflow: ellipsis; }
.win-pkg { font-size: 11px; overflow: hidden; text-overflow: ellipsis; }
.win-btn {
  background: none; border: none; cursor: pointer;
  color: rgba(255, 255, 255, 0.6);
  width: 30px; height: 26px; border-radius: 999px; flex-shrink: 0;
  display: inline-flex; align-items: center; justify-content: center;
}
.win-btn:hover { background: rgba(255, 255, 255, 0.1); color: #fff; }
.win-btn.close:hover { background: rgba(234, 67, 53, 0.25); color: #f6aea9; }
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
  background: #1c212bee; border: 1px solid rgba(255, 255, 255, 0.12); border-radius: 8px;
  color: #e6e9ef; padding: 4px 10px; font-size: 13px; z-index: 3;
  pointer-events: none;
}
/* 独立窗口不支持时的回退提示层 */
.win-fallback {
  position: absolute; inset: 0; z-index: 4;
  display: flex; flex-direction: column; align-items: center; justify-content: center;
  gap: 8px; padding: 20px; text-align: center;
  background: rgba(16, 18, 24, 0.82); backdrop-filter: blur(6px);
}
.wf-icon { opacity: 0.6; }
.wf-title { font-size: 14px; font-weight: 600; color: #e6e9ef; }
.wf-sub { max-width: 320px; line-height: 1.5; }
.wf-actions { display: flex; gap: 8px; margin-top: 4px; }
/* 底部导航栏 */
.win-navbar {
  display: flex; align-items: center; gap: 4px;
  background: rgba(255, 255, 255, 0.03);
  border-top: 1px solid rgba(255, 255, 255, 0.07);
  padding: 4px 10px;
}
.nav-btn {
  background: none; border: none; color: rgba(255, 255, 255, 0.75);
  font-size: 13px; width: 40px; height: 26px; border-radius: 999px; cursor: pointer;
}
.nav-btn:hover { background: rgba(255, 255, 255, 0.1); }
.win-resize {
  position: absolute; right: 0; bottom: 0;
  width: 18px; height: 18px;
  cursor: nwse-resize;
  background: linear-gradient(135deg, transparent 50%, rgba(255, 255, 255, 0.22) 50%);
  border-bottom-right-radius: 16px;
  touch-action: none;
}
</style>
