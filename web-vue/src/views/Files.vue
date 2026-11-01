<template>
  <div class="fpage">
    <!-- 顶栏：Material 3 应用栏风——路径芯片居左，图标按钮居右 -->
    <div class="appbar">
      <div class="crumbs">
        <template v-for="(seg, i) in crumbs" :key="i">
          <span class="crumb" :class="{ cur: i === crumbs.length - 1 }" @click="goto(i)">{{ seg }}</span>
          <MsIcon v-if="i < crumbs.length - 1" name="chevron_right" :size="14" class="csep" />
        </template>
      </div>
      <div class="actions">
        <button class="iconbtn" title="上级" :disabled="!canUp" @click="up">
          <MsIcon name="arrow_upward" :size="18" />
        </button>
        <button class="iconbtn" title="新建文件夹" @click="mkdirDlg">
          <MsIcon name="create_new_folder" :size="18" />
        </button>
        <button class="iconbtn" title="刷新" @click="load">
          <MsIcon name="refresh" :size="18" :class="{ spin: loading }" />
        </button>
      </div>
    </div>

    <!-- 文件列表：Google Files 风——图标彩底圆角 + 两行文字 + ⋮ 菜单 -->
    <div class="fpanel">
      <div v-if="loading && !entries.length" class="fempty">
        <MsIcon name="refresh" :size="28" class="spin" style="color:#8b93a7" />
        <div class="muted">读取中…</div>
      </div>
      <div v-else-if="!entries.length" class="fempty">
        <MsIcon name="folder_open" :size="44" style="color:#5b6474" />
        <div class="muted">空目录</div>
      </div>
      <div v-else class="flist">
        <div v-for="row in entries" :key="row.name" class="fitem"
             @dblclick="row.dir ? nav(row.name) : openOrDownload(row)"
             @click="row.dir && nav(row.name)">
          <div class="fico" :style="{ background: tint(row).bg }">
            <MsIcon :name="fileIcon(row)" :size="20" :style="{ color: tint(row).fg }" />
          </div>
          <div class="ftext">
            <div class="fname">{{ row.name }}</div>
            <div class="fsub">{{ row.dir ? '文件夹' : fmtSize(row.size) + ' · ' + fmtTime(row.mtime) }}</div>
          </div>
          <el-dropdown trigger="click" @command="(cmd) => onCmd(cmd, row)">
            <button class="iconbtn more" title="更多" @click.stop>
              <MsIcon name="more_vert" :size="18" />
            </button>
            <template #dropdown>
              <el-dropdown-menu>
                <el-dropdown-item v-if="!row.dir && previewKind(row)" command="preview">
                  <MsIcon name="visibility" :size="15" style="margin-right:6px" />预览
                </el-dropdown-item>
                <el-dropdown-item v-if="!row.dir" command="download">
                  <MsIcon name="download" :size="15" style="margin-right:6px" />下载
                </el-dropdown-item>
                <el-dropdown-item command="rename">
                  <MsIcon name="edit" :size="15" style="margin-right:6px" />重命名
                </el-dropdown-item>
                <el-dropdown-item command="delete" divided>
                  <MsIcon name="delete" :size="15" style="margin-right:6px" />删除
                </el-dropdown-item>
              </el-dropdown-menu>
            </template>
          </el-dropdown>
        </div>
      </div>
    </div>

    <!-- 预览对话框：图片/视频/音频内嵌播放，小文本直接显示 -->
    <el-dialog v-model="preview.open" :title="preview.name" width="720px"
               destroy-on-close align-center class="preview-dlg">
      <div class="preview-body">
        <img v-if="preview.kind === 'image'" :src="preview.url" class="pv-img" />
        <video v-else-if="preview.kind === 'video'" :src="preview.url" controls autoplay class="pv-media" />
        <audio v-else-if="preview.kind === 'audio'" :src="preview.url" controls autoplay class="pv-audio" />
        <pre v-else-if="preview.kind === 'text'" class="pv-text">{{ preview.text }}</pre>
        <div v-else class="muted" style="padding:30px;text-align:center">
          该类型不支持内嵌预览
          <div style="margin-top:10px"><el-button type="primary" @click="downloadPreview">下载文件</el-button></div>
        </div>
      </div>
      <template #footer>
        <el-button @click="downloadPreview">下载</el-button>
        <el-button type="primary" @click="preview.open = false">关闭</el-button>
      </template>
    </el-dialog>

    <!-- FAB：上传（Google Files 的右下角悬浮按钮） -->
    <button class="fab" title="上传到当前目录" :disabled="uploading" @click="fileInput.click()">
      <MsIcon name="add" :size="22" />
    </button>
    <input ref="fileInput" type="file" multiple style="display:none" @change="onFiles" />

    <!-- 上传进度：底部 snackbar 式条 -->
    <transition name="fadeup">
      <div v-if="uploading" class="upbar">
        <MsIcon name="refresh" :size="16" class="spin" />
        <span class="uname">{{ uploadName }}</span>
        <div class="ubar"><div class="ufill" :style="{ width: uploadPct + '%' }"></div></div>
        <span class="upct">{{ uploadPct }}%</span>
      </div>
    </transition>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, inject } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { apiGet, apiPost, imgUrl, authState } from '../api'
import MsIcon from '../components/MsIcon.vue'

const { deviceSerial } = inject('vmState')
const serial = computed(() => deviceSerial.value)

const path = ref('/sdcard')
const entries = ref([])
const loading = ref(false)
const fileInput = ref(null)

const crumbs = computed(() => path.value.split('/').filter(Boolean))
const canUp = computed(() => path.value !== '/sdcard')

function goto(i) {
  path.value = '/' + crumbs.value.slice(0, i + 1).join('/')
  load()
}
function nav(name) {
  path.value = (path.value === '/' ? '' : path.value) + '/' + name
  load()
}
function up() {
  path.value = path.value.split('/').slice(0, -1).join('/') || '/sdcard'
  load()
}

// Google Files 的类型配色（柔和底 + 饱和前景）
function fileIcon(row) {
  if (row.dir) return 'folder'
  const n = row.name.toLowerCase()
  if (/\.(png|jpe?g|gif|webp|bmp)$/.test(n)) return 'image'
  if (/\.(mp4|mkv|avi|mov|ts)$/.test(n)) return 'movie'
  if (/\.(mp3|flac|aac|ogg|wav)$/.test(n)) return 'music_note'
  if (/\.(zip|gz|tar|rar|7z|apk)$/.test(n)) return 'folder_zip'
  return 'description'
}
function tint(row) {
  const map = {
    folder:      { bg: 'rgba(251,191,36,.16)',  fg: '#fbbf24' },
    image:       { bg: 'rgba(167,139,250,.16)', fg: '#a78bfa' },
    movie:       { bg: 'rgba(248,113,113,.16)', fg: '#f87171' },
    music_note:  { bg: 'rgba(52,211,153,.16)',  fg: '#34d399' },
    folder_zip:  { bg: 'rgba(96,165,250,.16)',  fg: '#60a5fa' },
    description: { bg: 'rgba(148,163,184,.14)', fg: '#94a3b8' },
  }
  return map[fileIcon(row)]
}
function fmtSize(b) {
  if (b >= 1 << 30) return (b / (1 << 30)).toFixed(1) + 'G'
  if (b >= 1 << 20) return (b / (1 << 20)).toFixed(1) + 'M'
  if (b >= 1 << 10) return (b / (1 << 10)).toFixed(0) + 'K'
  return b + 'B'
}
function fmtTime(ts) {
  if (!ts) return ''
  const d = new Date(ts * 1000)
  const p = (n) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`
}

async function load() {
  loading.value = true
  try {
    const d = await apiGet('/api/files?path=' + encodeURIComponent(path.value))
    entries.value = d.entries || []
    path.value = d.path || path.value
  } catch (e) {
    ElMessage.error(e.message)
  }
  loading.value = false
}

function full(row) { return (path.value === '/' ? '' : path.value) + '/' + row.name }

function onCmd(cmd, row) {
  if (cmd === 'download') return download(row)
  if (cmd === 'preview') return openPreview(row)
  if (cmd === 'rename') return renameDlg(row)
  if (cmd === 'delete') return del(row)
}

// ---- 预览：图片/视频/音频内嵌，文本（≤1MB）直读，其它双击即下载 ----
const preview = ref({ open: false, name: '', kind: '', url: '', text: '' })

function previewKind(row) {
  const n = row.name.toLowerCase()
  if (/\.(png|jpe?g|gif|webp|bmp)$/.test(n)) return 'image'
  if (/\.(mp4|mkv|mov|m4v|webm)$/.test(n)) return 'video'
  if (/\.(mp3|flac|wav|ogg|m4a|aac)$/.test(n)) return 'audio'
  if (/\.(txt|log|json|xml|html?|js|css|csv|ini|conf|prop|sh)$/.test(n)) return 'text'
  return ''
}

function openOrDownload(row) {
  const kind = previewKind(row)
  if (kind) openPreview(row)
  else download(row)
}

function previewUrl(row) {
  return imgUrl('/api/files/download?path=' + encodeURIComponent(full(row)))
}

async function openPreview(row) {
  const kind = previewKind(row)
  const url = previewUrl(row)
  if (!kind) { download(row); return }
  let text = ''
  if (kind === 'text') {
    if (row.size > 1 << 20) {
      ElMessage.info('文本超过 1MB，请下载查看')
      download(row)
      return
    }
    try { text = await (await fetch(url)).text() } catch { text = '（读取失败）' }
  }
  preview.value = { open: true, name: row.name, kind, url, text }
}

function downloadPreview() {
  const a = document.createElement('a')
  a.href = preview.value.url
  a.download = preview.value.name
  a.click()
}

function download(row) {
  const url = imgUrl(`/api/files/download?path=${encodeURIComponent(full(row))}`)
  const a = document.createElement('a')
  a.href = url
  a.download = row.name
  a.click()
}

// 上传：XHR 串行 + 进度
const uploading = ref(false)
const uploadPct = ref(0)
const uploadName = ref('')
async function onFiles(e) {
  const files = [...e.target.files]
  e.target.value = ''
  if (!files.length) return
  uploading.value = true
  try {
    for (const f of files) {
      uploadName.value = `${f.name}（${fmtSize(f.size)}）`
      uploadPct.value = 0
      await new Promise((resolve, reject) => {
        const xhr = new XMLHttpRequest()
        xhr.open('POST', `/api/files/upload?path=${encodeURIComponent(path.value)}&name=${encodeURIComponent(f.name)}`)
        xhr.setRequestHeader('Content-Type', 'application/octet-stream')
        if (authState.token) xhr.setRequestHeader('Authorization', 'Bearer ' + authState.token)
        xhr.upload.onprogress = (ev) => {
          if (ev.lengthComputable) uploadPct.value = Math.round(ev.loaded * 100 / ev.total)
        }
        xhr.onload = () => {
          if (xhr.status >= 200 && xhr.status < 300) resolve()
          else {
            let d = {}
            try { d = JSON.parse(xhr.responseText) } catch {}
            reject(new Error(d.error || `上传失败 (${xhr.status})`))
          }
        }
        xhr.onerror = () => reject(new Error('上传失败：网络错误'))
        xhr.send(f)
      })
    }
    ElMessage.success(`已上传 ${files.length} 个文件`)
  } catch (e) {
    ElMessage.error(e.message)
  }
  uploading.value = false
  load()
}

async function mkdirDlg() {
  let name
  try {
    ({ value: name } = await ElMessageBox.prompt('文件夹名', '新建文件夹', { inputValue: '新建文件夹' }))
  } catch { return }
  name = (name || '').trim()
  if (!name) return
  try {
    await apiPost('/api/files/op', { op: 'mkdir', path: full({ name }) })
    load()
  } catch (e) { ElMessage.error(e.message) }
}

async function renameDlg(row) {
  let name
  try {
    ({ value: name } = await ElMessageBox.prompt('新名称', '重命名', { inputValue: row.name }))
  } catch { return }
  name = (name || '').trim()
  if (!name || name === row.name) return
  try {
    await apiPost('/api/files/op', { op: 'rename', path: full(row), to: path.value + '/' + name })
    load()
  } catch (e) { ElMessage.error(e.message) }
}

async function del(row) {
  try {
    await apiPost('/api/files/op', { op: 'delete', path: full(row) })
    ElMessage.success('已删除')
    load()
  } catch (e) { ElMessage.error(e.message) }
}

let timer = null
onMounted(() => { load(); timer = setInterval(load, 30000) })
onUnmounted(() => clearInterval(timer))
</script>

<style scoped>
.fpage {
  position: relative; display: flex; flex-direction: column; gap: 12px;
  /* 全出血（负边距抵消 el-main 自带 padding——底部 60px 大空白的根因），
     高度只减顶栏 52px，自控内边距 */
  margin: -18px -24px -60px;
  padding: 18px 24px 20px;
  height: calc(100vh - 54px);
  min-height: 360px;
  box-sizing: border-box;
}
.fpage > .el-alert { flex: none; }

/* 应用栏 */
.appbar {
  display: flex; align-items: center; gap: 10px;
  /* 毛玻璃：半透明深底 + 背景模糊饱和（壁纸透出），悬浮条质感 */
  background: linear-gradient(180deg, rgba(255, 255, 255, 0.12), rgba(255, 255, 255, 0.05));
  backdrop-filter: blur(24px) saturate(1.8);
  -webkit-backdrop-filter: blur(24px) saturate(1.8);
  border: 1px solid rgba(255, 255, 255, 0.14);
  box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.18), 0 8px 26px rgba(0, 0, 0, 0.3);
  border-radius: 14px;
  padding: 0 12px;
  height: 52px;
  box-sizing: border-box;
}
.crumbs { display: flex; align-items: center; flex-wrap: wrap; flex: 1; min-width: 0; }
.crumb { cursor: pointer; padding: 2px 7px; border-radius: 9px; font-size: 13px;
         color: var(--el-text-color-secondary); }
.crumb:hover { background: var(--el-fill-color-light); color: var(--el-text-color-primary); }
.crumb.cur { color: var(--el-text-color-primary); font-weight: 600; cursor: default; }
.crumb.cur:hover { background: none; }
.csep { color: var(--el-text-color-secondary); }
.actions { display: flex; gap: 4px; }
.iconbtn {
  border: none; background: transparent; cursor: pointer;
  width: 30px; height: 30px; border-radius: 10px;
  display: inline-flex; align-items: center; justify-content: center;
  color: var(--el-text-color-regular);
}
.iconbtn:hover { background: var(--el-fill-color); }
.iconbtn:disabled { opacity: .35; cursor: default; }
.iconbtn:disabled:hover { background: transparent; }

/* 文件面板 */
.fpanel {
  flex: 1; min-height: 0; overflow-y: auto;
  background: var(--el-bg-color); border: 1px solid var(--el-border-color);
  border-radius: 16px; padding: 6px;
}
/* 细滚动条（Material 质感） */
.fpanel::-webkit-scrollbar { width: 8px; }
.fpanel::-webkit-scrollbar-thumb { background: var(--el-fill-color-dark); border-radius: 4px; }
.fpanel::-webkit-scrollbar-thumb:hover { background: var(--el-border-color); }
.flist { display: flex; flex-direction: column; }
.fitem {
  display: flex; align-items: center; gap: 14px;
  padding: 8px 10px; border-radius: 14px; cursor: default;
}
.fitem:hover { background: var(--el-fill-color-light); }
.fitem .more { opacity: 0; }
.fitem:hover .more { opacity: 1; }
.fico {
  width: 42px; height: 42px; border-radius: 14px; flex: none;
  display: flex; align-items: center; justify-content: center;
}
.ftext { flex: 1; min-width: 0; }
.fname {
  font-size: 14px; color: var(--el-text-color-primary); font-weight: 500;
  overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
}
.fsub { font-size: 12px; color: var(--el-text-color-secondary); margin-top: 1px; }
.fempty {
  display: flex; flex-direction: column; align-items: center; justify-content: center;
  gap: 10px; height: 100%; min-height: 260px;
}

/* FAB（Material 浮动按钮） */
.fab {
  position: fixed; right: 46px; bottom: 78px; z-index: 20;
  width: 54px; height: 54px; border-radius: 18px; border: none;
  background: var(--el-color-primary); color: #fff; cursor: pointer;
  display: flex; align-items: center; justify-content: center;
  box-shadow: 0 6px 18px rgba(0,0,0,.35);
  transition: transform .12s, box-shadow .12s;
}
.fab:hover { transform: translateY(-2px); box-shadow: 0 10px 24px rgba(0,0,0,.4); }
.fab:disabled { opacity: .6; }

/* 上传进度条（底部悬浮） */
.upbar {
  position: fixed; left: 50%; transform: translateX(-50%); bottom: 78px; z-index: 20;
  display: flex; align-items: center; gap: 10px;
  background: var(--el-bg-color); border: 1px solid var(--el-border-color);
  border-radius: 14px; padding: 10px 14px; min-width: 380px;
  box-shadow: 0 8px 24px rgba(0,0,0,.3);
}
.uname { font-size: 12.5px; color: var(--el-text-color-regular); max-width: 260px;
         overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.ubar { flex: 1; height: 5px; border-radius: 3px; background: var(--el-fill-color); overflow: hidden; }
.ufill { height: 100%; background: var(--el-color-primary); border-radius: 3px;
         transition: width .2s; }
.upct { font-size: 12px; font-variant-numeric: tabular-nums; color: var(--el-text-color-secondary); }

.preview-body { display: flex; justify-content: center; align-items: center;
                min-height: 200px; max-height: 64vh; overflow: auto; }
.pv-img { max-width: 100%; max-height: 62vh; border-radius: 8px; }
.pv-media { max-width: 100%; width: 100%; max-height: 60vh; border-radius: 8px; background: #000; }
.pv-audio { width: 100%; }
.pv-text {
  width: 100%; max-height: 60vh; overflow: auto; margin: 0;
  font-size: 12px; line-height: 1.5; white-space: pre-wrap; word-break: break-all;
  background: var(--el-fill-color-light); border-radius: 8px; padding: 12px;
  font-family: ui-monospace, Menlo, monospace;
}

.spin { animation: fspin 1s linear infinite; }
@keyframes fspin { to { transform: rotate(360deg); } }
.fadeup-enter-active, .fadeup-leave-active { transition: opacity .18s, transform .18s; }
.fadeup-enter-from, .fadeup-leave-to { opacity: 0; transform: translate(-50%, 8px); }
</style>
