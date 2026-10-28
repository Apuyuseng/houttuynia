// Android 平板桌面：统一窗口管理。
// 业务功能与设备 Android 应用都是桌面上的「app」，点击开独立自由窗口。
import { reactive, computed, defineAsyncComponent } from 'vue'

// 业务应用注册表（桌面图标 + 窗口内容组件，懒加载保持代码分割）
export const BIZ_APPS = [
  // 屏幕镜像：特殊窗口（AppWindow 主屏形态，打开即全幅画面），不走 BizWindow
  { id: 'mirror',   title: '屏幕镜像', icon: 'biz_space_dashboard', tint: '#4285f4', w: 430, h: 700,
    special: 'mirror' },
  { id: 'devices',  title: '真机连接', icon: 'biz_smartphone', tint: '#34a853', w: 660, h: 540,
    comp: defineAsyncComponent(() => import('./views/Devices.vue')) },
  { id: 'files',    title: '文件',     icon: 'biz_folder', tint: '#00acc1', w: 660, h: 540,
    comp: defineAsyncComponent(() => import('./views/Files.vue')) },
  { id: 'terminal', title: '终端',     icon: 'biz_terminal', tint: '#300a24', w: 860, h: 620,
    comp: defineAsyncComponent(() => import('./views/Terminal.vue')) },
  { id: 'settings', title: '设置',     icon: 'biz_settings', tint: '#5f6368', w: 620, h: 540,
    comp: defineAsyncComponent(() => import('./views/Settings.vue')) },
]
export function bizMeta(id) { return BIZ_APPS.find(b => b.id === id) }

// 窗口：{ key, kind:'biz'|'app', id, title, app?, z, minimized }
export const winState = reactive({ list: [], seq: 0, zTop: 100 })

// 当前置顶窗口（状态栏显示其名称）
export const topWin = computed(() => {
  const vis = winState.list.filter(w => !w.minimized)
  if (!vis.length) return null
  return vis.reduce((a, b) => (a.z > b.z ? a : b))
})

function push(w) {
  winState.seq += 1
  winState.zTop += 1
  const win = { key: winState.seq, minimized: false, z: winState.zTop, ...w }
  winState.list.push(win)
  return win
}

export function openBiz(id) {
  const meta = bizMeta(id)
  if (!meta) return
  const exist = winState.list.find(w => w.kind === 'biz' && w.id === id)
  if (exist) return frontWin(exist.key)
  push({ kind: 'biz', id, title: meta.title })
}

// 屏幕镜像：直接开主屏形态窗口（与设备 app 同款：可拖拽/关闭/全幅画面）
export function openMirror(deviceSerial) {
  const exist = winState.list.find(w => w.kind === 'mirror')
  if (exist) return frontWin(exist.key)
  push({ kind: 'mirror', id: 'mirror', title: '屏幕镜像', deviceSerial: deviceSerial || '' })
}

export function openAndroid(a) {
  const exist = winState.list.find(w => w.kind === 'app' && w.app.package === a.package)
  if (exist) return frontWin(exist.key)
  push({ kind: 'app', id: a.package, title: a.name || a.package, app: a })
}

export function closeWin(key) {
  winState.list = winState.list.filter(w => w.key !== key)
}

export function frontWin(key) {
  const w = winState.list.find(w => w.key === key)
  if (!w) return
  w.minimized = false
  winState.zTop += 1
  w.z = winState.zTop
}

export function minWin(key) {
  const w = winState.list.find(w => w.key === key)
  if (w) w.minimized = true
}

// 点击桌面空白 / 状态栏品牌区：最小化全部，回到桌面
export function showDesktop() { winState.list.forEach(w => { w.minimized = true }) }
