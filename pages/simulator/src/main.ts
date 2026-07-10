import './style.css'
import { readSimulatorParams } from './params'
import { buildWebUrl } from './repo-provider'
import { loadSkill } from './skill-loader'
import { RuntimeHost } from './runtime-host'
import type { LoadedSkill } from './types'

const app = document.querySelector<HTMLDivElement>('#app')
if (!app) throw new Error('missing #app')
const appRoot = app

let loadedSkill: LoadedSkill | null = null
let runtime: RuntimeHost | null = null

function escapeHtml(value: string): string {
  return value
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&#39;')
}

function safeLocalScriptName(name: string): string {
  const cleaned = name.trim().replaceAll('\\', '/').split('/').pop() || 'local_script.lua'
  return cleaned.replace(/[^A-Za-z0-9._-]/g, '_') || 'local_script.lua'
}

async function createLocalSkill(file: File): Promise<LoadedSkill> {
  const name = safeLocalScriptName(file.name)
  const content = new Uint8Array(await file.arrayBuffer())
  const text = new TextDecoder().decode(content)
  const entry = `scripts/${name}`

  return {
    params: {
      repo: 'local',
      ref: 'local',
      skill: 'local/SKILL.md',
    },
    rootPath: 'local',
    frontmatter: {
      name: name.replace(/\.lua$/i, '') || 'local_script',
      description: 'Local Lua script uploaded from this browser.',
      metadata: {
        category: ['ui'],
        tags: ['local'],
      },
      simulator: {
        entry,
        files: [entry],
      },
    },
    markdownBody: '',
    files: [
      {
        path: entry,
        content,
        text,
      },
    ],
    entry,
    virtualRoot: `/uploads/local/${Date.now()}`,
  }
}

function renderShell(): HTMLIFrameElement {
  appRoot.innerHTML = `
    <div class="sim-page">
      <header class="site-header">
        <a class="brand" href="https://skills-lab.esp-claw.com/" target="_blank" rel="noreferrer">
          <img class="brand-logo" src="./logo.svg" alt="ESP-Claw" />
          <span class="brand-divider">|</span>
          <span>
            <strong>Skill Simulator</strong>
            <small>Lua LVGL Web Runtime</small>
          </span>
        </a>
        <div class="header-actions">
          <button id="localUploadButton" class="btn-secondary" type="button">Simulate Local Script</button>
          <a id="sourceLink" class="btn-secondary" href="#" target="_blank" rel="noreferrer">SKILL.md</a>
          <input id="localUpload" class="file-input" type="file" accept=".lua,text/x-lua,text/plain" />
        </div>
      </header>

      <main class="workspace">
        <section class="preview" aria-label="Simulator display">
          <div class="device-shell">
            <iframe id="runtime" title="ESP-Claw Lua LVGL runtime"></iframe>
          </div>
        </section>

        <aside class="control-panel">
          <section class="panel-section">
            <p class="eyebrow">Online Experience</p>
            <h1 id="skillName">Loading skill...</h1>
            <p id="skillDescription" class="description">Preparing simulator runtime.</p>
            <div id="categoryTags" class="tag-list"></div>
          </section>

          <section class="panel-section">
            <div class="run-row">
              <button id="run" class="btn-primary" type="button" disabled>Run</button>
              <button id="stop" class="btn-secondary" type="button" disabled>Stop</button>
            </div>
            <div id="status" class="status is-loading">Preparing runtime...</div>
          </section>

          <section class="panel-section info-grid">
            <div>
              <span class="label">Entry</span>
              <strong id="entryPath">-</strong>
            </div>
            <div>
              <span class="label">Files</span>
              <strong id="fileCount">-</strong>
            </div>
            <div>
              <span class="label">Source</span>
              <strong id="sourceName">Skills Lab</strong>
            </div>
          </section>

          <section class="panel-section log-section">
            <div class="section-title">
              <span>Runtime Log</span>
              <button id="clearLog" class="link-button" type="button">Clear</button>
            </div>
            <ol id="log" class="log"></ol>
          </section>
        </aside>
      </main>
    </div>
  `

  return document.querySelector<HTMLIFrameElement>('#runtime')!
}

function appendLog(message: string, kind: 'info' | 'error' = 'info'): void {
  const log = document.querySelector<HTMLOListElement>('#log')
  if (!log) return
  const item = document.createElement('li')
  item.className = kind === 'error' ? 'error' : ''
  item.textContent = `${new Date().toLocaleTimeString()} ${message}`
  log.appendChild(item)
  log.scrollTop = log.scrollHeight
}

function setStatus(message: string, state: 'loading' | 'ready' | 'running' | 'error' = 'ready'): void {
  const el = document.querySelector<HTMLDivElement>('#status')
  if (!el) return
  el.textContent = message
  el.className = `status is-${state}`
}

function setRunning(isRunning: boolean): void {
  const runButton = document.querySelector<HTMLButtonElement>('#run')
  const stopButton = document.querySelector<HTMLButtonElement>('#stop')
  runButton?.toggleAttribute('disabled', isRunning || !loadedSkill)
  stopButton?.toggleAttribute('disabled', !isRunning)
}

function renderLocalPrompt(): void {
  document.querySelector<HTMLHeadingElement>('#skillName')!.textContent = 'Local Lua Script'
  document.querySelector<HTMLParagraphElement>('#skillDescription')!.textContent =
    'Upload a Lua script from this computer and run it in the browser simulator.'
  document.querySelector<HTMLElement>('#entryPath')!.textContent = '-'
  document.querySelector<HTMLElement>('#fileCount')!.textContent = '0'
  document.querySelector<HTMLElement>('#sourceName')!.textContent = 'local'
  document.querySelector<HTMLAnchorElement>('#sourceLink')!.classList.add('is-hidden')
  const tagList = document.querySelector<HTMLDivElement>('#categoryTags')!
  tagList.innerHTML = '<span class="tag">local</span><span class="tag">ui</span>'
}

function renderSkill(skill: LoadedSkill): void {
  document.querySelector<HTMLHeadingElement>('#skillName')!.textContent = skill.frontmatter.name
  document.querySelector<HTMLParagraphElement>('#skillDescription')!.textContent =
    skill.frontmatter.description || 'ESP-Claw Lua LVGL skill'
  document.querySelector<HTMLElement>('#entryPath')!.textContent = skill.entry
  document.querySelector<HTMLElement>('#fileCount')!.textContent = String(skill.files.length)

  const sourceLink = document.querySelector<HTMLAnchorElement>('#sourceLink')!
  if (skill.params.repo === 'local') {
    sourceLink.classList.add('is-hidden')
    sourceLink.removeAttribute('href')
  } else {
    const sourceUrl = buildWebUrl(skill.params, skill.params.skill)
    sourceLink.classList.remove('is-hidden')
    sourceLink.href = sourceUrl
  }
  document.querySelector<HTMLElement>('#sourceName')!.textContent = skill.params.repo

  const categories = skill.frontmatter.metadata.category ?? []
  const tagList = document.querySelector<HTMLDivElement>('#categoryTags')!
  tagList.innerHTML = categories.map((cat) => `<span class="tag">${escapeHtml(cat)}</span>`).join('')
}

function runLoadedSkill(): void {
  if (!loadedSkill) return
  runtime?.run(loadedSkill)
  setRunning(true)
  setStatus('Running in browser runtime.', 'running')
}

async function main(): Promise<void> {
  const iframe = renderShell()
  runtime = new RuntimeHost(iframe, (message, kind) => appendLog(message, kind))

  const runButton = document.querySelector<HTMLButtonElement>('#run')
  const stopButton = document.querySelector<HTMLButtonElement>('#stop')
  const clearLogButton = document.querySelector<HTMLButtonElement>('#clearLog')
  const localUploadButton = document.querySelector<HTMLButtonElement>('#localUploadButton')
  const localUpload = document.querySelector<HTMLInputElement>('#localUpload')

  runButton?.addEventListener('click', runLoadedSkill)
  stopButton?.addEventListener('click', () => {
    runtime?.stop()
    setRunning(false)
    setStatus('Stop requested.', 'ready')
  })
  localUploadButton?.addEventListener('click', () => localUpload?.click())
  localUpload?.addEventListener('change', async () => {
    const file = localUpload.files?.[0]
    localUpload.value = ''
    if (!file) return
    try {
      runtime?.stop()
      setRunning(false)
      loadedSkill = await createLocalSkill(file)
      renderSkill(loadedSkill)
      appendLog(`uploaded local script ${file.name}`)
      setStatus('Local script loaded. Click Run to start.', 'ready')
      setRunning(false)
      runLoadedSkill()
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error)
      appendLog(message, 'error')
      setStatus(message, 'error')
    }
  })
  clearLogButton?.addEventListener('click', () => {
    const log = document.querySelector<HTMLOListElement>('#log')
    if (log) log.innerHTML = ''
  })

  try {
    appendLog('loading runtime')
    await runtime.load()
    let params
    try {
      params = readSimulatorParams()
    } catch (error) {
      renderLocalPrompt()
      appendLog('waiting for local script upload')
      setStatus('Upload a local Lua script to run.', 'ready')
      setRunning(false)
      return
    }
    appendLog('loading skill metadata')
    loadedSkill = await loadSkill(params)
    renderSkill(loadedSkill)
    appendLog(`loaded ${loadedSkill.files.length} files`)
    setStatus('Ready. Click Run to start.', 'ready')
    setRunning(false)
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error)
    appendLog(message, 'error')
    setStatus(message, 'error')
  }
}

main()
