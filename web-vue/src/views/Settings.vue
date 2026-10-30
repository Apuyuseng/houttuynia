<!-- Android 设置形态：主分类列表（彩色圆形 icon）→ 点入子页 → 顶栏返回。
     分类页 = 纯导航行；子页 = 该分类的具体开关/操作。 -->
<template>
  <div class="aset">
    <!-- ===== 子页：顶栏返回 + 内容 ===== -->
    <template v-if="page">
      <div class="aset-subhead">
        <button class="aset-back" @click="page = null" title="返回">
          <MsIcon name="arrow_back" :size="20" />
        </button>
        <span class="aset-subtitle">{{ page.title }}</span>
      </div>
      <div class="aset-body">
        <!-- 连接：真机连接全部能力（原「真机连接」app + 连接抽屉合并至此） -->
        <template v-if="page.id === 'conn'">
          <!-- 已连接设备 -->
          <div class="aset-sec">已连接设备</div>
          <div class="adw-boxed" style="padding:6px 10px">
            <el-empty v-if="!devList.length" description="暂无连接 —— 通过下方方式接入" :image-size="50" />
            <el-table v-else :data="devList" size="small" style="width:100%">
              <el-table-column prop="serial" label="设备" min-width="170" />
              <el-table-column label="通道" width="80">
                <template #default="{ row }">
                  <el-tag size="small" :type="row.kind === 'usb' ? 'primary' : 'success'">
                    {{ row.kind === 'usb' ? 'USB' : '无线' }}
                  </el-tag>
                </template>
              </el-table-column>
              <el-table-column label="电量" width="90">
                <template #default="{ row }">
                  <span v-if="devInfo[row.serial]?.battery?.present">{{ devInfo[row.serial].battery.level }}%<span v-if="devInfo[row.serial].battery.status === 'charging'">⚡</span></span>
                  <span v-else class="muted small">—</span>
                </template>
              </el-table-column>
              <el-table-column label="管理" width="80">
                <template #default="{ row }">
                  <el-tag v-if="row.current" size="small" type="success" effect="dark">当前</el-tag>
                  <el-button v-else-if="row.state === 'device'" size="small" text type="primary" @click="selectDev(row)">设为当前</el-button>
                </template>
              </el-table-column>
              <el-table-column width="70" fixed="right">
                <template #default="{ row }">
                  <el-popconfirm title="断开该设备？" @confirm="disconnect(row.serial)">
                    <template #reference><el-button size="small" text type="danger">断开</el-button></template>
                  </el-popconfirm>
                </template>
              </el-table-column>
            </el-table>
          </div>

          <!-- USB -->
          <div class="aset-sec">USB 设备（插入即出现）</div>
          <div class="adw-boxed">
            <div class="adw-row" v-for="u in usbList" :key="u.serial">
              <div class="adw-row-icon color c-blue"><MsIcon name="usb" :size="18" /></div>
              <div class="adw-row-main">
                <div class="adw-row-title mono">{{ u.serial }}</div>
                <div class="adw-row-sub">{{ u.state === 'device' ? '已连接' : u.state === 'unauthorized' ? '待授权——请在手机屏幕允许 USB 调试' : u.state }}</div>
              </div>
            </div>
            <div class="adw-row" v-if="!usbList.length">
              <div class="adw-row-sub">未检测到 USB 设备；首次使用需在手机上允许 USB 调试授权</div>
            </div>
          </div>

          <!-- 扫码配对 -->
          <div class="aset-sec">扫码配对（Android 11+ 新设备首次接入）</div>
          <div class="adw-boxed">
            <div class="adw-row">
              <div class="adw-row-icon color c-blue"><MsIcon name="qr_code_scanner" :size="18" /></div>
              <div class="adw-row-main">
                <div class="adw-row-title">二维码配对</div>
                <div class="adw-row-sub">手机「无线调试 → 使用二维码配对设备」扫此码；同网段且路由器放行组播</div>
              </div>
              <el-button size="small" :type="hp.state === 'waiting' ? 'warning' : 'primary'"
                         @click="hp.state === 'waiting' ? cancelQr() : beginQr()">
                {{ hp.state === 'waiting' ? '取消' : '出二维码' }}
              </el-button>
            </div>
            <div class="aset-qr" v-if="hp.state === 'waiting' || hp.state === 'pairing'">
              <div class="aset-qr-matrix">
                <div v-for="(row, y) in hp.rows" :key="y" class="aset-qr-row">
                  <span v-for="(c, x) in row" :key="x" class="aset-qr-cell" :class="{ on: c === '1' }"></span>
                </div>
              </div>
              <div class="aset-qr-tip">{{ hp.message }}</div>
            </div>
            <el-alert v-if="hp.state === 'failed'" :title="hp.message" type="error" :closable="false" style="margin:8px 12px" />
          </div>

          <!-- 配对码 + 手动 IP -->
          <div class="aset-sec">配对码 / 手动连接（已配对设备）</div>
          <div class="adw-boxed" style="padding:10px 12px">
            <el-row :gutter="16">
              <el-col :span="12">
                <div class="muted small" style="margin-bottom:6px">配对码（手机「使用配对码配对设备」弹出的 6 位数字）</div>
                <el-input v-model="pairForm.host" size="small" placeholder="IP 如 192.168.1.15" style="margin-bottom:6px" />
                <div style="display:flex;gap:6px">
                  <el-input-number v-model="pairForm.port" size="small" :min="1" :max="65535" controls-position="right" style="width:120px" />
                  <el-input v-model="pairForm.code" size="small" placeholder="6 位配对码" maxlength="6" style="flex:1" />
                  <el-button size="small" type="primary" :loading="pairing" @click="doPair">配对</el-button>
                </div>
              </el-col>
              <el-col :span="12">
                <div class="muted small" style="margin-bottom:6px">手动 IP:端口（已授权设备直连）</div>
                <div style="display:flex;gap:6px">
                  <el-input v-model="manualAddr" size="small" placeholder="192.168.1.15:5555" style="flex:1" @keyup.enter="manualConnect" />
                  <el-button size="small" type="primary" :loading="manualBusy" :disabled="!manualAddr.includes(':')" @click="manualConnect">连接</el-button>
                </div>
                <div style="display:flex;align-items:center;margin-top:10px">
                  <span class="small">局域网发现</span>
                  <div style="flex:1"></div>
                  <el-button size="small" text @click="loadDiscovered">刷新</el-button>
                </div>
                <div v-for="d in discovered" :key="d.addr" class="disc-row">
                  <span class="mono small">{{ d.addr }}</span>
                  <div style="flex:1"></div>
                  <el-button size="small" @click="connectTo(d.addr)">连接</el-button>
                </div>
              </el-col>
            </el-row>
          </div>

          <!-- 历史设备 -->
          <div class="aset-sec">历史设备</div>
          <div class="adw-boxed">
            <div class="adw-row" v-for="k in known" :key="k">
              <div class="adw-row-icon color c-gray"><MsIcon name="nav_phone" :size="18" /></div>
              <div class="adw-row-main">
                <div class="adw-row-title mono">{{ k }}</div>
                <div class="adw-row-sub" v-if="deviceSerial === k">当前已连接</div>
              </div>
              <el-button size="small" @click="connectKnown(k)">回连</el-button>
              <el-button size="small" type="danger" plain @click="forget(k)">移除</el-button>
            </div>
            <div class="adw-row" v-if="!known.length">
              <div class="adw-row-sub">暂无历史设备（连接过一次后自动记录）</div>
            </div>
          </div>

          <!-- 自动化 -->
          <div class="aset-sec">自动化</div>
          <div class="adw-boxed">
            <div class="adw-row">
              <div class="adw-row-icon color c-orange"><MsIcon name="refresh" :size="18" /></div>
              <div class="adw-row-main">
                <div class="adw-row-title">自动回连</div>
                <div class="adw-row-sub">周期探测历史设备端口，可达即自动连接</div>
              </div>
              <el-switch v-model="flags.auto_reconnect" @change="(v) => onFlag('auto_reconnect', v)" />
            </div>
            <div class="adw-row">
              <div class="adw-row-icon color c-green"><MsIcon name="key" :size="18" /></div>
              <div class="adw-row-main">
                <div class="adw-row-title">访问令牌</div>
                <div class="adw-row-sub">由服务端 MWINE_TOKEN 环境变量控制</div>
              </div>
              <el-button size="small" @click="needLogin = true">更换…</el-button>
            </div>
          </div>
        </template>

        <!-- 投屏与音频 -->
        <template v-else-if="page.id === 'stream'">
          <div class="adw-boxed">
            <div class="adw-row" v-for="f in flagsOf(['audio_stream'])" :key="f.key">
              <div class="adw-row-icon color" :class="f.tint"><MsIcon :name="f.icon" :size="18" /></div>
              <div class="adw-row-main">
                <div class="adw-row-title">{{ f.title }}</div>
                <div class="adw-row-sub">{{ f.desc }}</div>
              </div>
              <el-switch v-model="flags[f.key]" @change="(v) => onFlag(f.key, v)" />
            </div>
            <div class="adw-row">
              <div class="adw-row-icon color c-purple"><MsIcon name="movie" :size="18" /></div>
              <div class="adw-row-main">
                <div class="adw-row-title">画质</div>
                <div class="adw-row-sub">镜像默认原始分辨率 + 20Mbps；自动降档保护弱链路</div>
              </div>
            </div>
          </div>
        </template>

        <!-- 通知与剪贴板 -->
        <template v-else-if="page.id === 'notify'">
          <div class="adw-boxed">
            <div class="adw-row" v-for="f in flagsOf(['notify_toast', 'clipboard_sync'])" :key="f.key">
              <div class="adw-row-icon color" :class="f.tint"><MsIcon :name="f.icon" :size="18" /></div>
              <div class="adw-row-main">
                <div class="adw-row-title">{{ f.title }}</div>
                <div class="adw-row-sub">{{ f.desc }}</div>
              </div>
              <el-switch v-model="flags[f.key]" @change="(v) => onFlag(f.key, v)" />
            </div>
            <div class="adw-row">
              <div class="adw-row-icon color c-teal"><MsIcon name="nav_terminal" :size="18" /></div>
              <div class="adw-row-main">
                <div class="adw-row-title">短信管理 <el-tag size="small" type="info">规划中</el-tag></div>
                <div class="adw-row-sub">短信列表/发送（数据通道方案，不走投屏画面）</div>
              </div>
            </div>
          </div>
        </template>

        <!-- 桌面与应用 -->
        <template v-else-if="page.id === 'desktop'">
          <div class="adw-boxed">
            <div class="adw-row" v-for="f in flagsOf(['files_app'])" :key="f.key">
              <div class="adw-row-icon color" :class="f.tint"><MsIcon :name="f.icon" :size="18" /></div>
              <div class="adw-row-main">
                <div class="adw-row-title">{{ f.title }}</div>
                <div class="adw-row-sub">{{ f.desc }}</div>
              </div>
              <el-switch v-model="flags[f.key]" @change="(v) => onFlag(f.key, v)" />
            </div>
          </div>
        </template>

        <!-- 设备运维 -->
        <template v-else-if="page.id === 'ops'">
          <div class="adw-boxed">
            <div class="adw-row" v-for="op in OPS" :key="op.action">
              <div class="adw-row-icon color" :class="op.tint"><MsIcon :name="op.icon" :size="18" /></div>
              <div class="adw-row-main">
                <div class="adw-row-title">{{ op.title }}</div>
                <div class="adw-row-sub">{{ op.desc }}</div>
              </div>
              <el-button size="small" :type="op.danger ? 'danger' : 'default'" plain
                         :loading="busy === op.action" :disabled="!deviceSerial || busy !== ''"
                         @click="runOp(op)">{{ op.btn }}</el-button>
            </div>
          </div>
          <p class="aset-note">
            投屏异常时的自救开关：「重启 adb server」清空全部残留会话（投屏/终端）并重扫 USB，是最常用的一键修复。
          </p>
        </template>

        <!-- 关于 -->
        <template v-else-if="page.id === 'about'">
          <div class="aset-hero">
            <div class="aset-logo"><img src="/logo.svg" alt=""></div>
            <div>
              <div class="aset-appname">鱼腥草 · NAS 网关</div>
              <div class="aset-ver mono">v{{ about.version || version }}（纯真机版）</div>
            </div>
          </div>
          <div class="adw-boxed">
            <div class="adw-row">
              <div class="adw-row-icon color c-blue"><MsIcon name="add_link" :size="18" /></div>
              <div class="adw-row-main">
                <div class="adw-row-title">项目地址</div>
                <div class="adw-row-sub">github.com/Apuyuseng/houttuynia</div>
              </div>
              <el-button size="small" @click="openUrl(about.project)">打开</el-button>
            </div>
            <div class="adw-row">
              <div class="adw-row-icon color c-green"><MsIcon name="description" :size="18" /></div>
              <div class="adw-row-main">
                <div class="adw-row-title">使用者协议</div>
                <div class="adw-row-sub">MIT 许可 · 仅供学习与内部局域网使用</div>
              </div>
              <el-button size="small" @click="showLicense = true">查看</el-button>
            </div>
            <div class="adw-row">
              <div class="adw-row-icon color c-orange"><MsIcon name="nav_apps" :size="18" /></div>
              <div class="adw-row-main">
                <div class="adw-row-title">开源鸣谢</div>
                <div class="adw-row-sub">{{ (about.components || []).length }} 个开源组件支撑了本项目</div>
              </div>
              <el-button size="small" @click="showCredits = true">查看</el-button>
            </div>
            <div class="adw-row">
              <div class="adw-row-icon color c-purple"><MsIcon name="nav_terminal" :size="18" /></div>
              <div class="adw-row-main">
                <div class="adw-row-title">能力</div>
                <div class="adw-row-sub">投屏、应用库、ADB 终端与文件互传；无内置虚拟机</div>
              </div>
            </div>
          </div>

          <!-- 协议弹层 -->
          <el-dialog v-model="showLicense" title="使用者协议（MIT License）" width="86%">
            <div class="aset-license">
              <p>Copyright (c) 2026 Apuyuseng</p>
              <p>特此免费授予任何获得本软件副本的人无限使用权、复制、修改、合并、发布、分发、再许可和/或销售本软件的副本，并允许被授权者这样做，但须满足以下条件：</p>
              <p>上述版权声明和本许可声明应包含在本软件的所有副本或主要部分中。</p>
              <p><b>本软件按“原样”提供，不附带任何明示或默示的保证</b>，包括但不限于适销性、特定用途适用性和非侵权的保证。在任何情况下，作者或版权持有人均不对任何索赔、损害或其他责任负责，无论是在合同诉讼、侵权诉讼或其他诉讼中，还是因软件或软件的使用或其他交易而产生的行为。</p>
              <p class="aset-license-extra">使用须知：</p>
              <p class="aset-license-extra">1. 本工具仅用于自有设备的本地管理，请遵守设备所在国家/地区的法律法规；</p>
              <p class="aset-license-extra">2. 家长管控等功能请在合法监护场景下使用；</p>
              <p class="aset-license-extra">3. 请勿用于未授权访问他人设备。</p>
            </div>
            <template #footer>
              <el-button @click="showLicense = false">我已阅读</el-button>
            </template>
          </el-dialog>

          <!-- 鸣谢弹层 -->
          <el-dialog v-model="showCredits" title="开源鸣谢" width="86%">
            <p class="aset-credits-lead">本项目站在以下开源项目的肩膀上（按字母序）：</p>
            <div class="aset-credits">
              <div class="aset-credit" v-for="c in (about.components || [])" :key="c[0]">
                <span class="mono">{{ c[0] }}</span>
                <span class="muted">v{{ c[1] }}</span>
                <span class="aset-credit-lic">{{ c[2] }}</span>
              </div>
            </div>
            <p class="aset-credits-lead" style="margin-top:12px">
              以及所有未列出的依赖传递项（完整清单见源码 Cargo.lock）。
              感谢开源社区。
            </p>
          </el-dialog>
        </template>
      </div>
    </template>

    <!-- ===== 主分类列表 ===== -->
    <template v-else>
      <div class="aset-head">设置</div>
      <div class="aset-search">
        <MsIcon name="qr_code_scanner" :size="0" style="display:none" />
        <svg width="16" height="16" viewBox="0 -960 960 960" fill="currentColor" style="opacity:.6"><path d="M380-320q-109 0-184.5-75.5T120-580q0-109 75.5-184.5T380-840q109 0 184.5 75.5T640-580q0 44-14 83t-38 69l224 224q11 11 11 28t-11 28q-11 11-28 11t-28-11L532-372q-30 24-69 38t-83 14Zm0-80q75 0 127.5-52.5T560-580q0-75-52.5-127.5T380-760q-75 0-127.5 52.5T200-580q0 75 52.5 127.5T380-400Z"/></svg>
        <input class="aset-search-input" v-model="query" placeholder="搜索设置…" @keyup.esc="query = ''">
        <button v-if="query" class="aset-search-clear" @click="query = ''"><MsIcon name="close" :size="14" /></button>
      </div>
      <div class="aset-body">
        <!-- 搜索结果（扁平匹配：分类/开关/操作全量） -->
        <div class="adw-boxed" v-if="query">
          <div class="aset-nores" v-if="!searchResults.length">无匹配项</div>
          <div class="adw-cat" v-for="r in searchResults" :key="r.key" @click="goResult(r)">
            <div class="adw-row-icon color" :class="r.tint"><MsIcon :name="r.icon" :size="18" /></div>
            <div class="adw-row-main">
              <div class="adw-row-title">{{ r.title }}</div>
              <div class="adw-row-sub">{{ r.crumbs }} · {{ r.sub }}</div>
            </div>
            <MsIcon name="chevron_right" :size="18" style="color: var(--el-text-color-secondary)" />
          </div>
        </div>
        <div class="adw-boxed" v-else>
          <div class="adw-cat" v-for="c in CATS" :key="c.id" @click="page = c">
            <div class="adw-row-icon color" :class="c.tint"><MsIcon :name="c.icon" :size="20" /></div>
            <div class="adw-row-main">
              <div class="adw-row-title">{{ c.title }}</div>
              <div class="adw-row-sub">{{ c.sub }}</div>
            </div>
            <MsIcon name="chevron_right" :size="20" style="color: var(--el-text-color-secondary)" />
          </div>
        </div>
      </div>
    </template>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, inject } from 'vue'
import { ElMessage } from 'element-plus'
import { apiGet, apiPost, authState } from '../api'
import MsIcon from '../components/MsIcon.vue'
import { openBiz } from '../windows'
import { getFlags, setFlag } from '../flags'

const { deviceSerial } = inject('vmState')
const version = ref('')
const busy = ref('')
const page = ref(null)   // null = 主分类列表；否则为分类对象

const flags = ref(getFlags())
const FLAG_DEFS = {
  files_app:      { title: '文件管理器', desc: '桌面显示「文件」图标', icon: 'biz_folder', tint: 'c-cyan' },
  audio_stream:   { title: '投屏音频',   desc: '镜像画面同时传输设备声音（下次开投屏生效）', icon: 'nav_settings', tint: 'c-purple' },
  clipboard_sync: { title: '共享剪贴板', desc: '镜像窗口与设备剪贴板双向自动同步（SSE 实时推送）', icon: 'nav_terminal', tint: 'c-teal' },
  notify_toast:   { title: '通知推送',   desc: '设备通知实时弹到桌面（桥 APK 通知监听）', icon: 'nav_terminal', tint: 'c-orange' },
  auto_reconnect: { title: '自动回连',   desc: '探测历史设备端口并自动连接', icon: 'refresh', tint: 'c-orange' },
}
const flagsOf = (keys) => keys.map(k => ({ key: k, ...FLAG_DEFS[k] }))
function onFlag(k, v) {
  setFlag(k, v)
  ElMessage.success('已保存' + (k === 'audio_stream' ? '（下次开投屏生效）' : ''))
}

const OPS = [
  { action: 'restart_adb', title: '重启 adb server', desc: '清空全部残留会话并重扫 USB——一键修复', btn: '重启', icon: 'refresh', tint: 'c-red', danger: true },
  { action: 'kill_foreground', title: '停止前台应用', desc: 'force-stop 设备当前前台应用', btn: '停止', icon: 'nav_apps', tint: 'c-blue', danger: false },
  { action: 'force_stop_all', title: '清理后台', desc: '强制停止全部第三方应用（不动系统应用）', btn: '清理', icon: 'delete', tint: 'c-orange', danger: true },
  { action: 'wake', title: '唤醒屏幕', desc: 'KEYCODE_WAKEUP', btn: '唤醒', icon: 'nav_phone', tint: 'c-green', danger: false },
  { action: 'sleep', title: '熄屏', desc: 'KEYCODE_SLEEP', btn: '熄屏', icon: 'nav_phone', tint: 'c-gray', danger: false },
  { action: 'home', title: '回到主页', desc: 'KEYCODE_HOME', btn: '主页', icon: 'nav_home', tint: 'c-purple', danger: false },
]

// 主分类（Android 设置式：彩色圆形 icon + 标题 + 摘要 + 右箭头）
const CATS = [
  { id: 'conn',   title: '连接',           sub: deviceSerial.value ? `已连接 ${deviceSerial.value}` : '未连接 —— 管理真机接入', icon: 'nav_phone', tint: 'c-blue' },
  { id: 'stream', title: '投屏与音频',     sub: '画质、声音传输',                     icon: 'movie',     tint: 'c-purple' },
  { id: 'notify', title: '通知与剪贴板',   sub: '通知推送、共享剪贴板、短信（规划中）', icon: 'nav_terminal', tint: 'c-teal' },
  { id: 'desktop', title: '桌面',          sub: '桌面图标显示',                       icon: 'biz_folder', tint: 'c-cyan' },
  { id: 'ops',    title: '设备运维',       sub: '重启 adb、清理后台、唤醒熄屏',        icon: 'refresh',   tint: 'c-orange' },
  { id: 'about',  title: '关于',           sub: '版本与能力',                         icon: 'nav_settings', tint: 'c-gray' },
]

// 搜索：全量索引（分类 + 功能开关 + 运维操作 + 静态项），关键字命中 title/desc/分类名
const query = ref('')
const SEARCH_INDEX = (() => {
  const idx = []
  for (const c of CATS) {
    idx.push({ key: 'cat-' + c.id, cat: c.id, crumbs: '设置', title: c.title, sub: c.sub, icon: c.icon, tint: c.tint })
  }
  // 分类归属映射（子项挂到分类下）
  const catOf = { audio_stream: 'stream', notify_toast: 'notify', clipboard_sync: 'notify', files_app: 'desktop' }
  const catName = Object.fromEntries(CATS.map(c => [c.id, c.title]))
  for (const [k, f] of Object.entries(FLAG_DEFS)) {
    const cid = catOf[k] || 'conn'
    idx.push({ key: 'flag-' + k, cat: cid, crumbs: catName[cid], title: f.title, sub: f.desc, icon: f.icon, tint: f.tint, type: 'flag' })
  }
  for (const o of OPS) {
    idx.push({ key: 'op-' + o.action, cat: 'ops', crumbs: '设备运维', title: o.title, sub: o.desc, icon: o.icon, tint: o.tint, type: 'op' })
  }
  idx.push({ key: 'sms', cat: 'notify', crumbs: '通知与剪贴板', title: '短信管理', sub: '短信列表/发送（规划中）', icon: 'nav_terminal', tint: 'c-teal' })
  idx.push({ key: 'token', cat: 'conn', crumbs: '连接', title: '访问令牌', sub: '由服务端 MWINE_TOKEN 环境变量控制', icon: 'key', tint: 'c-green' })
  idx.push({ key: 'ver', cat: 'about', crumbs: '关于', title: '后端版本', sub: 'Rust 网关（mwine-nas）', icon: 'nav_settings', tint: 'c-gray' })
  return idx
})()
const searchResults = computed(() => {
  const q = query.value.trim().toLowerCase()
  if (!q) return []
  return SEARCH_INDEX.filter(r =>
    (r.title + r.sub + r.crumbs).toLowerCase().includes(q))
})
// 点搜索结果：开关项直接进入对应子页（并高亮）；操作项同
function goResult(r) {
  query.value = ''
  page.value = CATS.find(c => c.id === r.cat) || null
}


async function runOp(op) {
  busy.value = op.action
  try {
    const r = await apiPost('/api/device/op', { action: op.action })
    ElMessage.success(r.message || '已完成')
  } catch (e) {
    ElMessage.error(e.message || '操作失败')
  } finally {
    busy.value = ''
  }
}
const needLogin = computed({
  get: () => authState.needLogin,
  set: (v) => { authState.needLogin = v },
})

const showLicense = ref(false)
const showCredits = ref(false)
const about = ref({})
function openUrl(u) { window.open(u, '_blank') }

// ---- 连接子页状态（原 Devices app 全量逻辑） ----
const devList = ref([])
const usbList = ref([])
const devInfo = ref({})
const hp = ref({ state: 'idle', rows: [], message: '' })
let hpTimer = null
const pairForm = ref({ host: '', port: 5555, code: '' })
const pairing = ref(false)
const discovered = ref([])

async function loadDevs() {
  try {
    const d = await apiGet('/api/devices')
    devList.value = d.devices || []
    known.value = (d.known || []).filter(k => k !== deviceSerial.value)
    // 拆 USB 通道设备到 usbList（ Devices 页原逻辑是独立接口；桥模式下 devices 已含 USB）
    usbList.value = (d.devices || []).filter(x => x.kind === 'usb')
  } catch {}
  try {
    const m = await apiGet('/api/device/info')
    devInfo.value = m || {}
  } catch {}
}
async function selectDev(row) {
  try {
    await apiPost('/api/devices/select', { serial: row.serial })
    ElMessage.success('已设为当前设备')
    loadDevs()
  } catch (e) { ElMessage.error(e.message || '失败') }
}
async function disconnect(serial) {
  try {
    await apiPost('/api/devices/disconnect', { serial })
    ElMessage.success('已断开')
    loadDevs()
  } catch (e) { ElMessage.error(e.message || '断开失败') }
}
async function beginQr() {
  try {
    const r = await apiPost('/api/devices/hostpair/begin', {})
    hp.value = { state: r.state || 'waiting', rows: r.rows || [], message: r.message || '' }
    hpTimer = setInterval(async () => {
      try {
        const st = await apiGet('/api/devices/hostpair')
        hp.value = { ...hp.value, message: st.message || hp.value.message, state: st.state || hp.value.state }
        if (st.state === 'connected') {
          clearInterval(hpTimer)
          ElMessage.success('配对成功：' + (st.device || ''))
          setTimeout(() => { hp.value = { state: 'idle', rows: [], message: '' }; loadDevs() }, 1500)
        } else if (st.state === 'failed' || st.state === 'cancelled') {
          clearInterval(hpTimer)
        }
      } catch {}
    }, 2000)
  } catch (e) { ElMessage.error(e.message || '出码失败') }
}
function cancelQr() {
  clearInterval(hpTimer)
  apiPost('/api/devices/hostpair/cancel', {}).catch(() => {})
  hp.value = { state: 'idle', rows: [], message: '' }
}
async function doPair() {
  pairing.value = true
  try {
    const r = await apiPost('/api/devices/pair', {
      host: pairForm.value.host.trim(),
      port: Number(pairForm.value.port),
      code: pairForm.value.code.trim(),
    })
    ElMessage.success('配对成功，已连接 ' + (r.device || ''))
    loadDevs()
  } catch (e) {
    ElMessage.error(e.message || '配对失败')
  } finally { pairing.value = false }
}
async function loadDiscovered() {
  try {
    const d = await apiGet('/api/devices/discovered')
    discovered.value = d.services || []
  } catch { discovered.value = [] }
}
async function connectTo(addr) {
  manualAddr.value = addr
  manualConnect()
}

// ---- 连接子页状态 ----
const pairState = ref('idle')   // idle | waiting | pairing | connected | failed
const pairRows = ref([])
const pairMessage = ref('')
const pairQr = ref('')
const pairPwd = ref('')
let pairTimer = null
const manualAddr = ref('')
const manualBusy = ref(false)
const known = ref([])

async function loadKnown() {
  try {
    const d = await apiGet('/api/devices')
    known.value = (d.known || []).filter(k => k !== deviceSerial.value)
  } catch { known.value = [] }
}

async function startPair() {
  try {
    const r = await apiPost('/api/devices/hostpair/begin', {})
    pairQr.value = r.qr || ''
    pairRows.value = r.rows || []
    pairMessage.value = r.message || '等待扫码'
    pairPwd.value = (r.qr || '').match(/P:([^;]+)/)?.[1] || ''
    pairState.value = 'waiting'
    pairTimer = setInterval(async () => {
      try {
        const s = await apiGet('/api/devices/hostpair')
        pairMessage.value = s.message || pairMessage.value
        const st = s.state || 'waiting'
        if (st === 'connected') {
          pairState.value = 'connected'
          clearInterval(pairTimer)
          ElMessage.success('配对成功：' + (s.device || ''))
          setTimeout(() => { pairState.value = 'idle'; loadKnown() }, 1500)
        } else if (st === 'failed' || st === 'cancelled') {
          pairState.value = 'failed'
          clearInterval(pairTimer)
          ElMessage.error(s.message || '配对失败')
        }
      } catch {}
    }, 2000)
  } catch (e) {
    ElMessage.error(e.message || '出码失败')
  }
}
function cancelPair() {
  clearInterval(pairTimer)
  apiPost('/api/devices/hostpair/cancel', {}).catch(() => {})
  pairState.value = 'idle'
}

async function manualConnect() {
  manualBusy.value = true
  try {
    await apiPost('/api/devices/connect', { addr: manualAddr.value.trim() })
    ElMessage.success('已连接 ' + manualAddr.value)
    await loadKnown()
  } catch (e) {
    ElMessage.error(e.message || '连接失败')
  } finally {
    manualBusy.value = false
  }
}
async function connectKnown(k) {
  manualBusy.value = true
  try {
    await apiPost('/api/devices/connect', { addr: k })
    ElMessage.success('已连接 ' + k)
  } catch (e) {
    ElMessage.error(e.message || '回连失败（设备不在线？）')
  } finally {
    manualBusy.value = false
  }
}
async function forget(k) {
  try {
    await apiPost('/api/devices/known/remove', { serial: k })
    await loadKnown()
    ElMessage.success('已移除')
  } catch {}
}

onMounted(async () => {
  try {
    const st = await apiGet('/api/status')
    version.value = st.version || ''
  } catch {}
  try {
    about.value = await apiGet('/api/about')
  } catch {}
  loadDevs()
  loadDiscovered()
})
</script>

<style scoped>
.aset { display: flex; flex-direction: column; height: 100%; min-height: 0; }
.aset-head {
  padding: 18px 20px 10px; font-size: 22px; font-weight: 600;
  color: var(--el-text-color-primary);
}
.aset-subhead {
  display: flex; align-items: center; gap: 10px;
  padding: 10px 14px; flex-shrink: 0;
  border-bottom: 1px solid var(--el-border-color-lighter);
}
.aset-back {
  background: none; border: none; cursor: pointer;
  color: var(--el-text-color-primary);
  width: 34px; height: 34px; border-radius: 50%;
  display: inline-flex; align-items: center; justify-content: center;
}
.aset-back:hover { background: rgba(255,255,255,0.08); }
.aset-subtitle { font-size: 17px; font-weight: 600; }
.aset-body { flex: 1; overflow-y: auto; padding: 6px 14px 16px; }
.aset-search {
  display: flex; align-items: center; gap: 8px;
  margin: 0 14px 10px;
  padding: 8px 12px;
  background: rgba(255,255,255,0.06);
  border: 1px solid rgba(255,255,255,0.1);
  border-radius: 24px;
}
.aset-search-input {
  flex: 1; background: none; border: none; outline: none;
  color: var(--el-text-color-primary); font-size: 14px;
}
.aset-search-clear {
  background: rgba(255,255,255,0.1); border: none; cursor: pointer;
  color: var(--el-text-color-secondary);
  width: 20px; height: 20px; border-radius: 50%;
  display: inline-flex; align-items: center; justify-content: center;
}
.aset-nores { padding: 14px; text-align: center; color: var(--el-text-color-secondary); font-size: 13px; }

/* 彩色圆形 icon（Android 设置签名样式） */
.color {
  width: 38px; height: 38px; border-radius: 50%;
  display: inline-flex; align-items: center; justify-content: center;
  color: #fff; flex-shrink: 0;
}
.c-blue   { background: #4285f4; }
.c-green  { background: #34a853; }
.c-purple { background: #9334e6; }
.c-teal   { background: #00acc1; }
.c-orange { background: #fa903e; }
.c-cyan   { background: #00bcd4; }
.c-red    { background: #ea4335; }
.c-gray   { background: #5f6368; }

/* 主分类行：更大点击区 */
.adw-cat {
  display: flex; align-items: center; gap: 14px;
  padding: 12px 12px; cursor: pointer; border-radius: 12px;
}
.adw-cat:hover { background: rgba(255,255,255,0.05); }
.adw-cat .adw-row-title { font-size: 15px; }

.aset-hero {
  display: flex; align-items: center; gap: 14px;
  padding: 8px 14px 16px;
}
.aset-logo { width: 52px; height: 52px; }
.aset-logo img { width: 100%; height: 100%; }
.aset-appname { font-size: 18px; font-weight: 600; }
.aset-ver { font-size: 12px; color: var(--el-text-color-secondary); margin-top: 2px; }
.aset-license p { margin: 8px 0; font-size: 13px; line-height: 1.7; }
.aset-license-extra { color: var(--el-text-color-secondary); }
.aset-credits-lead { font-size: 13px; margin: 6px 0; }
.aset-credits { max-height: 42vh; overflow-y: auto; }
.aset-credit {
  display: flex; align-items: center; gap: 10px;
  padding: 7px 4px; font-size: 13px;
  border-bottom: 1px solid var(--el-border-color-lighter);
}
.aset-credit-lic { margin-left: auto; font-size: 11px; color: var(--el-text-color-secondary); }
.aset-sec {
  padding: 4px 14px 6px; font-size: 12px; font-weight: 600;
  color: var(--el-text-color-secondary);
}
.aset-qr {
  display: flex; gap: 14px; align-items: center;
  padding: 10px 14px 14px;
}
.aset-qr-matrix {
  display: grid; padding: 6px; background: #fff; border-radius: 8px;
}
.aset-qr-row { display: flex; }
.aset-qr-cell { width: 4px; height: 4px; }
.aset-qr-cell.on { background: #000; }
.aset-qr-tip { font-size: 12px; color: var(--el-text-color-secondary); line-height: 1.7; }
.disc-row {
  display: flex; align-items: center; gap: 8px;
  padding: 5px 2px; border-bottom: 1px solid var(--el-border-color-extra-light);
}
.aset-note {
  margin: 10px 14px 0; font-size: 12px;
  color: var(--el-text-color-secondary); line-height: 1.6;
}
</style>
