// scrcpy Web 投屏播放器（与 vanilla 版 stream.js 同逻辑，框架无关类）
// 帧协议（大端，头 15 字节）：u16 magic(0x4D57) | u8 type | u64 pts_us | u32 len | payload
import { apiPost, imgUrl } from './api'

export const SCRCPY_KEY = {
  BACK: 4, HOME: 3, APP_SWITCH: 187, POWER: 26,
  VOL_UP: 24, VOL_DOWN: 25, ENTER: 66, DEL: 67,
  UP: 19, DOWN: 20, LEFT: 21, RIGHT: 22,
}

export class ScrcpyWebPlayer {
  constructor({ onStatus, onEnd }) {
    this.onStatus = onStatus || (() => {})
    this.onEnd = onEnd || (() => {})
    this.id = 0
    this.decoder = null
    this.config = null
    this.needsKey = true
    this.running = false
    this.abort = null
    this.canvas = document.createElement('canvas')
    this.canvas.className = 'stream-canvas'
    this.canvas.tabIndex = 0
    this.devW = 0
    this.devH = 0
    this.fps = 0
    this._frames = 0
    this._downPos = null
    this._lastMove = 0
    this._lastWheel = 0
    this._bindInput()
  }

  static supported() {
    return typeof VideoDecoder !== 'undefined' && typeof EncodedVideoChunk !== 'undefined'
  }

  async start() {
    this.onStatus('正在建立投屏会话…')
    const r = await apiPost('/api/stream/open?max_size=1280&bit_rate=4000000&fps=30', {})
    this.id = r.id
    this.devW = r.width || 0
    this.devH = r.height || 0
    this.running = true
    this._statsTimer = setInterval(() => {
      this.fps = this._frames
      this._frames = 0
      if (this.running)
        this.onStatus(`投屏中 · 会话 #${this.id} · ${this.devW}×${this.devH} · ${this.fps} fps`)
    }, 1000)
    this.abort = new AbortController()
    try {
      const resp = await fetch(imgUrl(`/api/stream/${this.id}/video`), { signal: this.abort.signal })
      if (!resp.ok) throw new Error(`视频流请求失败 (${resp.status})`)
      this.onStatus(`投屏中 · 会话 #${this.id} · 等待首帧…`)
      await this._pump(resp.body.getReader())
    } catch (e) {
      if (e.name !== 'AbortError' && this.running) this.onStatus('投屏中断：' + e.message, true)
    }
    this._finish()
  }

  async stop() {
    if (!this.running && !this.id) return
    this.running = false
    if (this.abort) this.abort.abort()
    if (this.id) {
      try { await apiPost(`/api/stream/${this.id}/close`) } catch {}
    }
    this._finish()
  }

  _finish() {
    this.running = false
    this.id = 0
    clearInterval(this._statsTimer)
    if (this.decoder) { try { this.decoder.close() } catch {} this.decoder = null }
    this.config = null
    this.needsKey = true
    this.onEnd()
  }

  async _pump(reader) {
    let buf = new Uint8Array(0)
    for (;;) {
      const { done, value } = await reader.read()
      if (done) break
      const next = new Uint8Array(buf.length + value.length)
      next.set(buf)
      next.set(value, buf.length)
      buf = next
      for (;;) {
        if (buf.length < 15) break
        const magic = (buf[0] << 8) | buf[1]
        if (magic !== 0x4d57) throw new Error('流帧同步丢失')
        const type = buf[2]
        let pts = 0
        for (let i = 3; i < 11; i++) pts = pts * 256 + buf[i]
        const len = ((buf[11] << 24) | (buf[12] << 16) | (buf[13] << 8) | buf[14]) >>> 0
        if (buf.length < 15 + len) break
        const payload = buf.slice(15, 15 + len)
        buf = buf.slice(15 + len)
        this._onFrame(type, pts, payload)
      }
    }
  }

  _onFrame(type, pts, payload) {
    if (type === 4) { this.stop(); return }
    if (type === 3) {
      const dv = new DataView(payload.buffer, payload.byteOffset)
      this.devW = dv.getUint32(0)
      this.devH = dv.getUint32(4)
      return
    }
    if (type === 0) { this.config = payload; return }
    let data = payload
    if (type === 2) {
      if (this.config) {
        data = new Uint8Array(this.config.length + payload.length)
        data.set(this.config)
        data.set(payload, this.config.length)
      }
      this.needsKey = false
    } else if (this.needsKey) {
      return
    }
    if (!this._ensureDecoder(data)) return
    try {
      this.decoder.decode(new EncodedVideoChunk({
        type: type === 2 ? 'key' : 'delta',
        timestamp: Math.round(pts),
        data,
      }))
    } catch { /* 队列瞬时溢出由 error 回调兜底 */ }
  }

  _ensureDecoder(sampleData) {
    if (this.decoder) return true
    const codec = this._codecFromSps(this.config || sampleData)
    try {
      this.decoder = new VideoDecoder({
        output: (frame) => this._render(frame),
        error: () => this._decoderError(),
      })
      this.decoder.configure({ codec, optimizeForLatency: true })
      return true
    } catch (e) {
      this.onStatus('浏览器无法硬解 H.264：' + e.message, true)
      this.decoder = null
      return false
    }
  }

  _codecFromSps(data) {
    for (let i = 0; i + 4 < data.length; i++) {
      if ((data[i] & 0x1f) === 7) {
        const h = (b) => b.toString(16).padStart(2, '0')
        return 'avc1.' + h(data[i + 1]) + h(data[i + 2]) + h(data[i + 3])
      }
    }
    return 'avc1.640028'
  }

  _render(frame) {
    if (!this.running) { frame.close(); return }
    const cw = frame.displayWidth, ch = frame.displayHeight
    if (this.canvas.width !== cw || this.canvas.height !== ch) {
      this.canvas.width = cw
      this.canvas.height = ch
    }
    this.canvas.getContext('2d').drawImage(frame, 0, 0)
    frame.close()
    this._frames++
    if (this.decoder && this.decoder.decodeQueueSize > 8) {
      try { this.decoder.reset() } catch {}
      this.needsKey = true
    }
  }

  async _decoderError() {
    if (!this.running) return
    this.needsKey = true
    try { this.decoder && this.decoder.reset() } catch {}
    try { await apiPost(`/api/stream/${this.id}/reset`) } catch {}
  }

  _pos(e) {
    const rect = this.canvas.getBoundingClientRect()
    const x = Math.round((e.clientX - rect.left) * this.devW / rect.width)
    const y = Math.round((e.clientY - rect.top) * this.devH / rect.height)
    return {
      x: Math.max(0, Math.min(this.devW - 1, x)),
      y: Math.max(0, Math.min(this.devH - 1, y)),
    }
  }

  _touch(action, p) {
    if (!this.running) return
    apiPost(`/api/stream/${this.id}/touch`, { action, x: p.x, y: p.y }).catch(() => {})
  }

  _bindInput() {
    const c = this.canvas
    c.style.touchAction = 'none'
    c.addEventListener('contextmenu', (e) => e.preventDefault())
    c.addEventListener('pointerdown', (e) => {
      if (!this.running) return
      c.focus()
      e.preventDefault()
      if (e.button === 2) { this.key(SCRCPY_KEY.BACK); return }
      if (e.button === 1) { this.key(SCRCPY_KEY.HOME); return }
      c.setPointerCapture(e.pointerId)
      const p = this._pos(e)
      this._downPos = p
      this._touch(0, p)
    })
    c.addEventListener('pointermove', (e) => {
      if (!this.running || !this._downPos) return
      const now = performance.now()
      if (now - this._lastMove < 33) return
      this._lastMove = now
      this._touch(2, this._pos(e))
    })
    const up = (e) => {
      if (!this._downPos) return
      this._touch(1, this._pos(e))
      this._downPos = null
    }
    c.addEventListener('pointerup', up)
    c.addEventListener('pointercancel', up)
    c.addEventListener('wheel', (e) => {
      if (!this.running) return
      e.preventDefault()
      const now = performance.now()
      if (now - this._lastWheel < 80) return
      this._lastWheel = now
      const p = this._pos(e)
      apiPost(`/api/stream/${this.id}/scroll`, {
        x: p.x, y: p.y,
        hs: Math.max(-3, Math.min(3, e.deltaX / 120)),
        vs: Math.max(-3, Math.min(3, e.deltaY / 120)),
      }).catch(() => {})
    }, { passive: false })
    c.addEventListener('keydown', (e) => {
      if (!this.running) return
      const map = {
        Enter: SCRCPY_KEY.ENTER, Backspace: SCRCPY_KEY.DEL,
        ArrowUp: SCRCPY_KEY.UP, ArrowDown: SCRCPY_KEY.DOWN,
        ArrowLeft: SCRCPY_KEY.LEFT, ArrowRight: SCRCPY_KEY.RIGHT,
        Escape: SCRCPY_KEY.BACK,
      }
      if (map[e.key] !== undefined) {
        e.preventDefault()
        this.key(map[e.key])
      } else if (e.key.length === 1 && !e.metaKey && !e.ctrlKey && !e.altKey) {
        e.preventDefault()
        this.text(e.key)
      }
    })
  }

  key(keycode, action = 'tap') {
    if (!this.running) return
    apiPost(`/api/stream/${this.id}/key`, { keycode, action }).catch(() => {})
  }

  text(text) {
    if (!this.running || !text) return
    apiPost(`/api/stream/${this.id}/text`, { text }).catch(() => {})
  }
}
