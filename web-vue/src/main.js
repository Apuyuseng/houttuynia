// NAS 版 Web 桌面：Android 平板桌面形态，无路由——
// 业务功能与设备应用都是桌面上的 app，点击开独立窗口（见 windows.js）。
import { createApp } from 'vue'
import ElementPlus from 'element-plus'
import zhCn from 'element-plus/es/locale/lang/zh-cn'
import 'element-plus/dist/index.css'
import 'element-plus/theme-chalk/dark/css-vars.css'
import './style.css'
import App from './App.vue'

const app = createApp(App)
app.use(ElementPlus, { locale: zhCn })
app.mount('#app')
