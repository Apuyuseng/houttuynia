import { createApp } from 'vue'
import { createRouter, createWebHashHistory } from 'vue-router'
import ElementPlus from 'element-plus'
import zhCn from 'element-plus/es/locale/lang/zh-cn'
import 'element-plus/dist/index.css'
import 'element-plus/theme-chalk/dark/css-vars.css'
import './style.css'
import App from './App.vue'

const routes = [
  { path: '/', redirect: '/overview' },
  { path: '/overview', name: 'overview', component: () => import('./views/Overview.vue'), meta: { title: '概览' } },
  { path: '/apps', name: 'apps', component: () => import('./views/Apps.vue'), meta: { title: '应用库' } },
  { path: '/devices', name: 'devices', component: () => import('./views/Devices.vue'), meta: { title: '真机连接' } },
  { path: '/store', name: 'store', component: () => import('./views/Store.vue'), meta: { title: '在线商店' } },
  { path: '/terminal', name: 'terminal', component: () => import('./views/Terminal.vue'), meta: { title: 'ADB 终端' } },
  { path: '/guard', name: 'guard', component: () => import('./views/Guard.vue'), meta: { title: '家长管控' } },
  { path: '/files', name: 'files', component: () => import('./views/Files.vue'), meta: { title: '文件' } },
  { path: '/settings', name: 'settings', component: () => import('./views/Settings.vue'), meta: { title: '设置' } },
]

export const router = createRouter({
  history: createWebHashHistory(),
  routes,
})

const app = createApp(App)
app.use(ElementPlus, { locale: zhCn })
app.use(router)
app.mount('#app')
