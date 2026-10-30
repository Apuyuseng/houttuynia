// 设备事件流（SSE）：通知（桥 APK 实时）+ 剪贴板（活跃投屏会话）。
// EventSource 断线自动重连；消费者按 type 订阅。
import { authState } from './api'

let es = null
const handlers = { notification: [], clipboard: [] }

function ensure() {
  if (es) return
  es = new EventSource('/api/events/device?token=' + encodeURIComponent(authState.token))
  es.onmessage = (e) => {
    try {
      const d = JSON.parse(e.data)
      const list = handlers[d.type]
      if (list) list.forEach(fn => { try { fn(d) } catch {} })
    } catch {}
  }
  // EventSource 自带重连；无需手动处理
}

export function onDeviceEvent(type, fn) {
  ensure()
  handlers[type].push(fn)
  return () => { // 取消订阅
    const i = handlers[type].indexOf(fn)
    if (i >= 0) handlers[type].splice(i, 1)
  }
}
