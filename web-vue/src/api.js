// API 封装：统一带令牌；401 抛特殊错误由 App 弹登录框
import { reactive } from 'vue'

export const authState = reactive({
  needLogin: false,
  token: localStorage.getItem('mw_token') || '',
})

export function saveToken(t) {
  authState.token = t || ''
  if (t) localStorage.setItem('mw_token', t)
  else localStorage.removeItem('mw_token')
}

export async function api(method, path, body, opts = {}) {
  const headers = {}
  if (authState.token) headers['Authorization'] = 'Bearer ' + authState.token
  let payload
  if (body !== undefined && body !== null) {
    headers['Content-Type'] = 'application/json'
    payload = JSON.stringify(body)
  }
  const resp = await fetch(path, { method, headers, body: payload, signal: opts.signal })
  if (resp.status === 401) {
    authState.needLogin = true
    throw new Error('未授权')
  }
  let data = {}
  const text = await resp.text()
  try { data = text ? JSON.parse(text) : {} } catch { data = { raw: text } }
  if (!resp.ok) {
    const e = new Error(data.error || `请求失败 (${resp.status})`)
    e.status = resp.status
    e.data = data
    throw e
  }
  return data
}

export const apiGet = (p) => api('GET', p)
export const apiPost = (p, b) => api('POST', p, b ?? {})

// <img> 等无法带头的场景用 query 带令牌
export function imgUrl(path) {
  return path + (path.includes('?') ? '&' : '?') + 'token=' + encodeURIComponent(authState.token)
}

export function fmtBytes(n) {
  if (n >= 1 << 30) return (n / (1 << 30)).toFixed(2) + ' GB'
  if (n >= 1 << 20) return (n / (1 << 20)).toFixed(1) + ' MB'
  if (n >= 1024) return (n / 1024).toFixed(0) + ' KB'
  return n + ' B'
}

export function fmtMb(mb) {
  if (mb >= 1024) return (mb / 1024).toFixed(1) + ' GB'
  return Math.round(mb) + ' MB'
}

export function fmtUptime(sec) {
  sec = Math.floor(sec)
  const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60)
  if (h > 0) return `${h} 小时 ${m} 分`
  if (m > 0) return `${m} 分 ${sec % 60} 秒`
  return `${sec} 秒`
}
