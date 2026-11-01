// 功能开关（localStorage 持久化，设置页管理）
const KEY = 'mwine_flags'
const DEFAULTS = {
  files_app: true,      // 文件管理器桌面图标（默认开）
  audio_stream: true,   // 投屏音频
  clipboard_sync: true, // 双向共享剪贴板
  notify_toast: true,   // 设备通知桌面 toast
  auto_reconnect: true, // 历史设备端口探测自动回连
}
export function getFlags() {
  try { return { ...DEFAULTS, ...JSON.parse(localStorage.getItem(KEY) || '{}') } }
  catch { return { ...DEFAULTS } }
}
export function setFlag(k, v) {
  const f = getFlags(); f[k] = v
  localStorage.setItem(KEY, JSON.stringify(f))
  return f
}
