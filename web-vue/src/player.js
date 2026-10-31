// scrcpy Web 投屏播放器（与 vanilla 版 stream.js 同逻辑，框架无关类）
// 帧协议（大端，头 15 字节）：u16 magic(0x4D57) | u8 type | u64 pts_us | u32 len | payload
import { apiPost, imgUrl, authState } from './api'

export const SCRCPY_KEY = {
  BACK: 4, HOME: 3, APP_SWITCH: 187, POWER: 26,
  VOL_UP: 24, VOL_DOWN: 25, ENTER: 66, DEL: 67,
  UP: 19, DOWN: 20, LEFT: 21, RIGHT: 22,
}

// 画质档位（max_size=长边上限 / bit_rate / fps）
export const QUALITY_TIERS = [
  { key: 'low',    label: '流畅', max_size: 720,  bit_rate: 800_000,  fps: 15 },
  { key: 'medium', label: '均衡', max_size: 1080, bit_rate: 2_000_000, fps: 24 },
  { key: 'high',   label: '高清', max_size: 1440, bit_rate: 4_000_000, fps: 30 },
]

export class ScrcpyWebPlayer {
  constructor({ onStatus, onEnd, onFirstFrame, openQuery = '', quality }) {
    this.openQuery = openQuery
    // 画质：'auto'（自动升降档）或档位 key；缺省读用户偏好
    this.qualityPref = quality || localStorage.getItem('mw_quality') || 'auto'
    this.tier = QUALITY_TIERS.find(t => t.key === localStorage.getItem('mw_auto_tier')) || QUALITY_TIERS[1]
    this._switching = false
    this.onFirstFrame = onFirstFrame || (() => {})
    this._firstFrameFired = false
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
    // 内联尺寸样式：Vue scoped CSS 作用不到动态创建的元素（曾导致窗口内
    // 画布按原始分辨率展示被裁掉）。contain 保持比例，不足处留黑。
    this.canvas.style.width = '100%'
    this.canvas.style.height = '100%'
    this.canvas.style.objectFit = 'contain'
    this.canvas.style.display = 'block'
    this.devW = 0
    this.devH = 0
    this.fps = 0
    this._frames = 0
    this._downPos = null
    this._lastMove = 0
    this._lastWheel = 0
    // 音频状态
    this.audioDecoder = null
    this.actx = null
    this.gain = null
    this.muted = false
    this._audioDisabled = false
    this._nextAudio = 0
    this._audioMeta = null
    this._ctlQueue = []
    this._ctlTries = 0
    this._bindInput()
  }

  static supported() {
    // WebCodecs 的 VideoDecoder 仅在安全上下文（HTTPS/localhost）暴露；
    // HTTP 部署（NAS 局域网常见）下 undefined。以 EncodedVideoChunk 为准
    // （它不受安全上下文限制），解码失败由 _ensureDecoder 的 error 回调兜底。
    return typeof EncodedVideoChunk !== 'undefined'
  }

  async start() {
    this.onStatus('正在建立投屏会话…')
    try {
      await this._openAndPump()
    } catch (e) {
      if (e.name !== 'AbortError' && this.running) this.onStatus('投屏中断：' + e.message, true)
      this._finish()
    }
  }

  // 建会话并拉流（start 与换挡重开共用）。拉流循环后台跑——_pump 直到流
  // 断开才返回，若 await 它，换挡（setQuality）会永远挂起（UI 选择器卡死）。
  // 代际守卫：旧 pump 结束时若已有新会话（gen 变化）不得误杀。
  async _openAndPump() {
    const gen = (this._pumpGen = (this._pumpGen || 0) + 1)
    const t = this.tier
    const r = await apiPost('/api/stream/open?max_size=' + t.max_size + '&bit_rate=' + t.bit_rate + '&fps=' + t.fps + this.openQuery, {})
    this.id = r.id
    this.devW = r.width || 0
    this.devH = r.height || 0
    this._frames = 0
    this.fps = 0
    this._sessionAt = Date.now()
    this._lastFrameAt = Date.now()
    this.running = true
    window.__mwinePlayer = this  // 诊断钩子（自动化验证用）
    if (!this._statsTimer) {
      this._statsTimer = setInterval(() => this._onTick(), 1000)
    }
    this.abort = new AbortController()
    if (!this._wdTimer) this._watchdog()
    const resp = await fetch(imgUrl(`/api/stream/${this.id}/video`), { signal: this.abort.signal })
    if (!resp.ok) throw new Error(`视频流请求失败 (${resp.status})`)
    this.onStatus(`投屏中 · ${this._tierLabel()} · 等待首帧…`)
    this._pump(resp.body.getReader())
      .catch(e => {
        // 换挡主动中断不当作错误
        if (e.name !== 'AbortError' && this.running) this.onStatus('投屏中断：' + e.message, true)
      })
      .then(() => {
        if (gen === this._pumpGen && !this._switching && this.running) this._finish()
      })
  }

  _tierLabel() {
    return this.qualityPref === 'auto' ? `自动·${this.tier.label}` : this.tier.label
  }

  // 每秒：状态栏 + 自动档升降判断
  _onTick() {
    this.fps = this._frames
    this._frames = 0
    if (!this.running) return
    this.onStatus(`投屏中 · ${this._tierLabel()} · ${this.devW}×${this.devH} · ${this.fps} fps`)
    if (this.qualityPref !== 'auto' || this._switching || !this.id) return
    const t = this.tier
    const now = Date.now()
    // 卡顿判定：帧率持续低于目标 55%；会话稳定 10s 后，连续 3s 完全无帧
    // 也计入（深卡顿不能被「等待首帧」豁免吞掉）
    const deep = this.fps === 0
      && Date.now() - this._sessionAt > 10000
      && Date.now() - this._lastFrameAt > 3000
    if ((this.fps > 0 && this.fps < t.fps * 0.55) || deep) {
      this._strikes = (this._strikes || 0) + 1
    } else {
      this._strikes = 0
    }
    if (now < (this._cooldownUntil || 0)) return
    const idx = QUALITY_TIERS.indexOf(t)
    if (this._strikes >= 5 && idx > 0) {
      this._autoSwitch(idx - 1, `链路吃紧，自动降档 → ${QUALITY_TIERS[idx - 1].label}`)
    } else if (this._strikes === 0 && this.fps >= t.fps * 0.92 && idx < QUALITY_TIERS.length - 1) {
      this._stableSecs = (this._stableSecs || 0) + 1
      if (this._stableSecs >= 60) this._autoSwitch(idx + 1, `链路富余，自动升档 → ${QUALITY_TIERS[idx + 1].label}`)
    } else {
      this._stableSecs = 0
    }
  }

  async _autoSwitch(newIdx, reason) {
    this._switching = true
    this._strikes = 0
    this._stableSecs = 0
    this._cooldownUntil = Date.now() + 45000 // 冷却 45s 防来回抖
    this.tier = QUALITY_TIERS[newIdx]
    localStorage.setItem('mw_auto_tier', this.tier.key)
    this.onStatus(reason + '，重建会话…')
    await this._reopen()
    this._switching = false
  }

  // 换挡重开：关旧会话 → 建新会话（canvas 与输入绑定保留，UI 不闪）
  async _reopen() {
    if (this.abort) { this.abort.abort(); this.abort = null }
    if (this.id) {
      const old = this.id
      this.id = 0
      try { await apiPost(`/api/stream/${old}/close`) } catch {}
    }
    if (this.decoder) { try { this.decoder.close() } catch {} this.decoder = null }
    this.config = null
    this.needsKey = true
    try {
      await this._openAndPump()
    } catch (e) {
      this.onStatus('换挡失败：' + e.message, true)
      this._finish()
    }
  }

  // 手动切档（外部 UI 调用；quality='auto' 或档位 key）
  async setQuality(pref) {
    this.qualityPref = pref
    localStorage.setItem('mw_quality', pref)
    if (pref !== 'auto') {
      const t = QUALITY_TIERS.find(x => x.key === pref)
      if (t && t !== this.tier) {
        this.tier = t
        localStorage.setItem('mw_auto_tier', t.key)
      }
    }
    if (this.running) {
      this._switching = true
      this.onStatus(`已切 ${pref === 'auto' ? '自动·' + this.tier.label : this.tier.label}，重建会话…`)
      await this._reopen()
      this._switching = false
    }
  }

  async stop() {
    if (!this.running && !this.id) return
    this.running = false
    if (this.abort) this.abort.abort()
    if (this.id) {
      // close 请求 3s 超时兜底：设备侧会话销毁可能挂起（慢链路/桥重建），
      // 不能让 UI 的关闭/切档流程被它卡死（后端会话由服务端空闲回收兜底）
      const id = this.id
      try {
        await Promise.race([
          apiPost(`/api/stream/${id}/close`),
          new Promise(r => setTimeout(r, 3000)),
        ])
      } catch {}
    }
    this._finish()
  }

  _finish() {
    this.running = false
    this.id = 0
    if (this._ctlSocket) { try { this._ctlSocket.close() } catch {} this._ctlSocket = null }
    clearInterval(this._statsTimer)
    clearInterval(this._wdTimer)
    if (this.decoder) { try { this.decoder.close() } catch {} this.decoder = null }
    if (this.audioDecoder) { try { this.audioDecoder.close() } catch {} this.audioDecoder = null }
    if (this.actx) { try { this.actx.close() } catch {} this.actx = null; this.gain = null }
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
    if (type === 5) { this._onAudioConfig(payload); return }
    if (type === 6) { this._onAudioFrame(pts, payload); return }
    if (type === 0) {
      this.config = payload
      // SPS/PPS（config）到达即建解码器——黑屏兜底：delta 帧丢了才等关键帧
      this._ensureDecoder(payload)
      return
    }
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

  // ---- 音频（AAC；scrcpy config 包 = AudioSpecificConfig）----
  _onAudioConfig(asc) {
    if (!window.AudioDecoder) { this._audioDisabled = true; return }
    // ASC 解析：AOT(5bit) sf_index(4bit) channel(4bit)
    let sampleRate = 48000, channels = 2
    try {
      const b0 = asc[0], b1 = asc[1]
      const sfIdx = ((b0 & 0x07) << 1) | (b1 >> 7)
      channels = (b1 >> 3) & 0x07
      sampleRate = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                    16000, 12000, 11025, 8000, 7350][sfIdx] || 48000
    } catch {}
    try {
      if (this.audioDecoder) { try { this.audioDecoder.close() } catch {} }
      this.audioDecoder = new AudioDecoder({
        output: (ad) => this._playAudio(ad),
        error: () => { this._audioDisabled = true },
      })
      this.audioDecoder.configure({
        codec: 'mp4a.40.2',
        sampleRate, numberOfChannels: channels,
        description: asc,
      })
      this._audioMeta = { sampleRate, channels }
      // AudioContext 需用户手势解锁——投屏由点击发起，这里补 resume 兜底
      if (!this.actx) {
        this.actx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate })
        this.gain = this.actx.createGain()
        this.gain.gain.value = this.muted ? 0 : 1
        this.gain.connect(this.actx.destination)
      }
      this.actx.resume().catch(() => {})
    } catch {
      this._audioDisabled = true
    }
  }

  _onAudioFrame(pts, data) {
    if (this._audioDisabled || !this.audioDecoder) return
    try {
      this.audioDecoder.decode(new EncodedAudioChunk({
        type: 'key',
        timestamp: Math.round(pts),
        data,
      }))
    } catch { /* 忽略瞬时解码失败 */ }
  }

  // 解码输出 → AudioBufferSourceNode 顺序调度（追赶：积压超 0.5s 丢齐）
  _playAudio(ad) {
    if (!this.running || !this.actx) { ad.close(); return }
    const { sampleRate, channels } = this._audioMeta || { sampleRate: 48000, channels: 2 }
    const frames = ad.numberOfFrames
    if (!frames) { ad.close(); return }
    const buf = this.actx.createBuffer(channels, frames, sampleRate)
    for (let ch = 0; ch < channels; ch++) {
      // WebCodecs AAC 输出常见 f32-planar；按格式取交错/平面
      try {
        if (ad.format === 'f32-planar') {
          const plane = new Float32Array(frames)
          ad.copyTo(plane, { planeIndex: ch, frameCount: frames })
          buf.copyToChannel(plane, ch)
        } else {
          const tmp = new Float32Array(frames * channels)
          ad.copyTo(tmp, { format: 'f32', frameCount: frames })
          const chData = new Float32Array(frames)
          for (let i = 0; i < frames; i++) chData[i] = tmp[i * channels + ch]
          buf.copyToChannel(chData, ch)
        }
      } catch {}
    }
    ad.close()
    const t = this.actx.currentTime
    if (this._nextAudio === undefined || this._nextAudio < t) {
      this._nextAudio = Math.max(this._nextAudio || 0, t + 0.03)
    }
    if (this._nextAudio - t > 0.5) this._nextAudio = t + 0.03 // 追赶丢齐
    const src = this.actx.createBufferSource()
    src.buffer = buf
    src.connect(this.gain)
    src.start(this._nextAudio)
    this._nextAudio += frames / sampleRate
    this._audioPlayed = (this._audioPlayed || 0) + 1
  }

  setMuted(m) {
    this.muted = !!m
    if (this.gain) this.gain.gain.value = this.muted ? 0 : 1
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
    this._lastFrameAt = Date.now()
    const cw = frame.displayWidth, ch = frame.displayHeight
    if (this.canvas.width !== cw || this.canvas.height !== ch) {
      this.canvas.width = cw
      this.canvas.height = ch
    }
    this.canvas.getContext('2d').drawImage(frame, 0, 0)
    frame.close()
    this._frames++
    if (!this._firstFrameFired) {
      this._firstFrameFired = true
      this.onFirstFrame()
    }
    if (this.decoder && this.decoder.decodeQueueSize > 8) {
      try { this.decoder.reset() } catch {}
      this.needsKey = true
    }
  }

  // 黑屏兜底：解码器静默丢帧（optimizeForLatency 下部分实现不报错）时
  // 主动 reset 强制编码器重发 config+关键帧
  _watchdog() {
    this._lastFrameAt = Date.now()
    this._wdTimer = setInterval(async () => {
      if (!this.running || !this.id) return
      const idle = Date.now() - this._lastFrameAt
      if (idle > 1500) {
        try { this.decoder && this.decoder.reset() } catch {}
        this.needsKey = true
        try { await apiPost(`/api/stream/${this.id}/reset`) } catch {}
      }
    }, 800)
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

  // 控制通道：单条 WebSocket 即发即走（原生 scrcpy 同款路径）。
  // 原 POST 每事件一请求，连续拖动时事件在浏览器同源连接池排队，输入延迟大。
  // WS 不可用时自动回落 POST。
  _ctl(msg) {
    if (!this.running) return
    const ws = this._ctlSocket
    if (ws && ws.readyState === 1) {
      try { ws.send(JSON.stringify(msg)) } catch { this._ctlPost(msg) }
      return
    }
    this._ctlQueue.push(msg)
    if (!this._ctlSocket && this._ctlTries < 3) {
      this._ctlTries = (this._ctlTries || 0) + 1
      try {
        const proto = location.protocol === 'https:' ? 'wss:' : 'ws:'
        const tok = encodeURIComponent(authState.token || '')
        const s = new WebSocket(`${proto}//${location.host}/api/stream/${this.id}/ctl?token=${tok}`)
        this._ctlSocket = s
        s.onopen = () => {
          this._ctlQueue.splice(0).forEach((m) => { try { s.send(JSON.stringify(m)) } catch {} })
        }
        s.onclose = s.onerror = () => {
          if (this._ctlSocket === s) this._ctlSocket = null
          // 回落：把队列里的消息用 POST 补发
          this._ctlQueue.splice(0).forEach((m) => this._ctlPost(m))
        }
      } catch { this._ctlSocket = null }
    }
  }
  _ctlPost(msg) {
    const map = { touch: 'touch', key: 'key', text: 'text', scroll: 'scroll' }
    const ep = map[msg.t]
    if (!ep) return
    const body = { ...msg }
    delete body.t
    apiPost(`/api/stream/${this.id}/${ep}`, body).catch(() => {})
  }

  _touch(action, p) {
    if (!this.running) return
    this._ctl({ t: 'touch', action, x: p.x, y: p.y })
  }

  _bindInput() {
    const c = this.canvas
    c.style.touchAction = 'none'
    c.addEventListener('contextmenu', (e) => e.preventDefault())
    c.addEventListener('pointerdown', (e) => {
      if (!this.running) return
      this.focusIme()
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
      this._ctl({
        t: 'scroll', x: p.x, y: p.y,
        hs: Math.max(-3, Math.min(3, e.deltaX / 120)),
        vs: Math.max(-3, Math.min(3, e.deltaY / 120)),
      })
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
    this._ctl({ t: 'key', keycode, action })
  }

  // ---- 宿主输入法接入（组合文本注入设备）----
  // hidden input 捕获宿主 IME 的 composition 事件；preedit 经 onPreedit 上浮显示
  attachIme(inputEl, { onPreedit } = {}) {
    this.imeEl = inputEl
    this._imeValue = ''
    let composing = false
    inputEl.addEventListener('compositionstart', () => {
      composing = true
      onPreedit && onPreedit(inputEl.value)
    })
    inputEl.addEventListener('compositionupdate', () => {
      onPreedit && onPreedit(inputEl.value)
    })
    inputEl.addEventListener('compositionend', () => {
      composing = false
      const text = inputEl.value
      inputEl.value = ''
      this._imeValue = ''
      onPreedit && onPreedit('')
      if (text) this.text(text)
    })
    // 非 IME 输入（英文/数字直接键入）：按差量注入
    inputEl.addEventListener('input', (e) => {
      if (composing || e.isComposing) return
      const now = inputEl.value
      const prev = this._imeValue
      if (now.length > prev.length && now.startsWith(prev)) {
        const delta = now.slice(prev.length)
        if (delta) this.text(delta)
      }
      this._imeValue = now
      // 防无限增长：失焦风险的清零留给 compositionend/change 后空闲时
      if (now.length > 64) { inputEl.value = ''; this._imeValue = '' }
    })
    // 控制键：Enter/Backspace/Escape/方向 → 设备按键（可打印字符不进这里，
    // 交由 input/composition 通道，避免双重注入）
    inputEl.addEventListener('keydown', (e) => {
      const map = {
        Enter: SCRCPY_KEY.ENTER, Backspace: SCRCPY_KEY.DEL,
        ArrowUp: SCRCPY_KEY.UP, ArrowDown: SCRCPY_KEY.DOWN,
        ArrowLeft: SCRCPY_KEY.LEFT, ArrowRight: SCRCPY_KEY.RIGHT,
        Escape: SCRCPY_KEY.BACK,
      }
      if (map[e.key] !== undefined) {
        e.preventDefault()
        this.key(map[e.key])
      }
    })
    // 粘贴进设备：SET_CLIPBOARD(paste=true)——server 置剪贴板并对焦点字段
    // 触发 KEYCODE_PASTE（与桌面版 ⌘V 语义一致；CJK/ASCII 通吃）
    inputEl.addEventListener('paste', (e) => {
      e.preventDefault()
      const text = e.clipboardData?.getData('text') || ''
      if (text) this.pasteClipboard(text)
    })
  }

  focusIme() {
    if (this.imeEl) this.imeEl.focus()
    else this.canvas.focus()
  }

  // ---- 剪贴板 ----
  async getClipboard() {
    const r = await apiGet(`/api/stream/${this.id}/clipboard`)
    return r.text || ''
  }
  async setClipboard(text) {
    try { await apiPost(`/api/stream/${this.id}/clipboard`, { text }) } catch {}
  }
  async pasteClipboard(text) {
    try { await apiPost(`/api/stream/${this.id}/clipboard`, { text, paste: true }) } catch {}
  }

  text(text) {
    if (!this.running || !text) return
    this._ctl({ t: 'text', text })
  }
}
