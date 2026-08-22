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
let runtimeReady = false
let runtimeFailed = false
let runtimeRunning = false
let guideStepIndex = -1

const guideIcons = {
  display: `
    <svg viewBox="0 0 72 64" aria-hidden="true">
      <rect class="guide-monitor-frame" x="7" y="7" width="58" height="42" rx="5"></rect>
      <path class="guide-monitor-area" d="M17 17h12M17 17v10M55 17H43M55 17v10M17 39h12M17 39V29M55 39H43M55 39V29"></path>
      <path class="guide-monitor-stand" d="M36 49v8M24 58h24"></path>
    </svg>
  `,
  run: `
    <svg viewBox="0 0 48 48" aria-hidden="true">
      <path class="guide-run-shape" d="M18 13.5 36 24 18 34.5z"></path>
    </svg>
  `,
  stop: `
    <svg viewBox="0 0 48 48" aria-hidden="true">
      <rect class="guide-stop-shape" x="14" y="14" width="20" height="20" rx="2"></rect>
    </svg>
  `,
}

const guideSteps = [
  {
    target: '#displayControls',
    icon: 'display' as const,
    title: 'Adjust the display',
    description: 'Set Width and Height, then click Apply to resize the simulator.',
  },
  {
    target: '#run',
    icon: 'run' as const,
    title: 'Run the skill',
    description: 'Click Run to load the program and start the simulation.',
  },
  {
    target: '#stop',
    icon: 'stop' as const,
    title: 'Stop the program',
    description: 'Once the skill is running, click Stop to end it safely.',
  },
]

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
      execution: {
        entry,
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
    capabilityMocks: {},
    simulatorMocks: {},
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
          <button id="guideButton" class="btn-secondary" type="button">Guide</button>
          <button id="localUploadButton" class="btn-secondary" type="button">Simulate Local Script</button>
          <a id="sourceLink" class="btn-secondary" href="#" target="_blank" rel="noreferrer">SKILL.md</a>
          <input id="localUpload" class="file-input" type="file" accept=".lua,text/x-lua,text/plain" />
        </div>
      </header>

      <main class="workspace">
        <section class="preview" aria-label="Simulator display">
          <div class="device-shell">
            <iframe id="runtime" title="ESP-Claw Lua LVGL runtime" allow="camera"></iframe>
            <div id="previewLoading" class="preview-loading" role="status" aria-label="Loading runtime">
              <span>Loading</span>
              <span class="loading-dots" aria-hidden="true">
                <span>.</span><span>.</span><span>.</span>
              </span>
            </div>
            <button id="previewRun" class="preview-run" type="button" hidden>
              <span class="preview-run-icon" aria-hidden="true">
                <svg viewBox="0 0 48 48">
                  <path d="M18 13.5 36 24 18 34.5z"></path>
                </svg>
              </span>
              <span>Run Skill</span>
            </button>
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

          <section id="displayControls" class="panel-section">
            <div class="section-title">
              <span>Display</span>
            </div>
            <div class="resolution-row">
              <label>
                <span class="label">Width</span>
                <input id="displayWidth" type="number" min="64" max="2048" step="1" value="800" />
              </label>
              <label>
                <span class="label">Height</span>
                <input id="displayHeight" type="number" min="64" max="2048" step="1" value="480" />
              </label>
              <button id="applyResolution" class="btn-secondary" type="button" disabled>Apply</button>
            </div>
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
    <div id="guideOverlay" class="guide-overlay" role="dialog" aria-modal="true" aria-labelledby="guideTitle" hidden>
      <div class="guide-shade" data-shade="top"></div>
      <div class="guide-shade" data-shade="right"></div>
      <div class="guide-shade" data-shade="bottom"></div>
      <div class="guide-shade" data-shade="left"></div>
      <div id="guideSpotlight" class="guide-spotlight"></div>
      <svg class="guide-arrow" aria-hidden="true">
        <defs>
          <marker id="guideArrowHead" markerWidth="10" markerHeight="10" refX="8" refY="5" orient="auto">
            <path d="M 0 0 L 10 5 L 0 10 z"></path>
          </marker>
        </defs>
        <path id="guideArrowPath" marker-end="url(#guideArrowHead)"></path>
      </svg>
      <section id="guideCard" class="guide-card">
        <div id="guideIcon" class="guide-icon" aria-hidden="true"></div>
        <div>
          <span id="guideStepLabel" class="guide-step-label"></span>
          <h2 id="guideTitle"></h2>
          <p id="guideDescription"></p>
        </div>
      </section>
      <div class="guide-footer">
        <div id="guideDots" class="guide-dots" aria-hidden="true">
          ${guideSteps.map(() => '<span></span>').join('')}
        </div>
        <span id="guideHint"></span>
      </div>
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
  runtimeRunning = isRunning
  const runButton = document.querySelector<HTMLButtonElement>('#run')
  const previewRunButton = document.querySelector<HTMLButtonElement>('#previewRun')
  const previewLoading = document.querySelector<HTMLElement>('#previewLoading')
  const stopButton = document.querySelector<HTMLButtonElement>('#stop')
  const applyResolutionButton = document.querySelector<HTMLButtonElement>('#applyResolution')
  const widthInput = document.querySelector<HTMLInputElement>('#displayWidth')
  const heightInput = document.querySelector<HTMLInputElement>('#displayHeight')
  const canRun = Boolean(!isRunning && loadedSkill && runtimeReady)
  const isPreparing = !isRunning && !runtimeFailed && !canRun
  runButton?.toggleAttribute('disabled', !canRun)
  previewRunButton?.toggleAttribute('hidden', !canRun)
  previewLoading?.toggleAttribute('hidden', !isPreparing)
  stopButton?.toggleAttribute('disabled', !isRunning)
  applyResolutionButton?.toggleAttribute('disabled', isRunning || !runtimeReady)
  widthInput?.toggleAttribute('disabled', isRunning || !runtimeReady)
  heightInput?.toggleAttribute('disabled', isRunning || !runtimeReady)
}

function setDisplayResolution(width: number, height: number): void {
  const widthInput = document.querySelector<HTMLInputElement>('#displayWidth')
  const heightInput = document.querySelector<HTMLInputElement>('#displayHeight')
  const deviceShell = document.querySelector<HTMLElement>('.device-shell')
  if (widthInput) widthInput.value = String(width)
  if (heightInput) heightInput.value = String(height)
  deviceShell?.style.setProperty('--device-aspect', `${width} / ${height}`)
}

function applyRequestedResolution(): void {
  const widthInput = document.querySelector<HTMLInputElement>('#displayWidth')
  const heightInput = document.querySelector<HTMLInputElement>('#displayHeight')
  const width = Math.max(64, Math.min(2048, Number.parseInt(widthInput?.value || '800', 10) || 800))
  const height = Math.max(64, Math.min(2048, Number.parseInt(heightInput?.value || '480', 10) || 480))
  setDisplayResolution(width, height)
  runtime?.setResolution(width, height)
}

function setGuideRect(element: HTMLElement, left: number, top: number, width: number, height: number): void {
  element.style.left = `${left}px`
  element.style.top = `${top}px`
  element.style.width = `${Math.max(0, width)}px`
  element.style.height = `${Math.max(0, height)}px`
}

function updateGuide(): void {
  if (guideStepIndex < 0) return
  const step = guideSteps[guideStepIndex]
  const target = document.querySelector<HTMLElement>(step.target)
  const overlay = document.querySelector<HTMLElement>('#guideOverlay')
  const spotlight = document.querySelector<HTMLElement>('#guideSpotlight')
  const card = document.querySelector<HTMLElement>('#guideCard')
  if (!target || !overlay || !spotlight || !card) return

  const padding = 8
  const targetRect = target.getBoundingClientRect()
  const left = Math.max(8, targetRect.left - padding)
  const top = Math.max(8, targetRect.top - padding)
  const right = Math.min(window.innerWidth - 8, targetRect.right + padding)
  const bottom = Math.min(window.innerHeight - 8, targetRect.bottom + padding)

  const shades = {
    top: overlay.querySelector<HTMLElement>('[data-shade="top"]')!,
    right: overlay.querySelector<HTMLElement>('[data-shade="right"]')!,
    bottom: overlay.querySelector<HTMLElement>('[data-shade="bottom"]')!,
    left: overlay.querySelector<HTMLElement>('[data-shade="left"]')!,
  }
  setGuideRect(shades.top, 0, 0, window.innerWidth, top)
  setGuideRect(shades.bottom, 0, bottom, window.innerWidth, window.innerHeight - bottom)
  setGuideRect(shades.left, 0, top, left, bottom - top)
  setGuideRect(shades.right, right, top, window.innerWidth - right, bottom - top)
  setGuideRect(spotlight, left, top, right - left, bottom - top)

  const cardWidth = Math.min(380, window.innerWidth - 32)
  const compact = window.innerWidth < 700
  const cardLeft = compact ? 16 : left >= cardWidth + 56 ? left - cardWidth - 40 : Math.max(16, right + 40)
  const cardTop = compact
    ? bottom + 220 < window.innerHeight
      ? bottom + 24
      : Math.max(16, top - 210)
    : Math.max(24, Math.min(window.innerHeight - 240, (top + bottom) / 2 - 100))
  card.style.width = `${cardWidth}px`
  card.style.left = `${Math.min(cardLeft, window.innerWidth - cardWidth - 16)}px`
  card.style.top = `${cardTop}px`

  const cardRect = card.getBoundingClientRect()
  if (compact) {
    const cardBelow = cardRect.top >= bottom
    const startX = cardRect.left + cardRect.width / 2
    const startY = cardBelow ? cardRect.top : cardRect.bottom
    const endX = (left + right) / 2
    const endY = cardBelow ? bottom + 6 : top - 6
    const bend = Math.abs(endY - startY) * 0.55
    document
      .querySelector<SVGPathElement>('#guideArrowPath')
      ?.setAttribute(
        'd',
        `M ${startX} ${startY} C ${startX} ${startY + (cardBelow ? -bend : bend)}, ${endX} ${endY + (cardBelow ? bend : -bend)}, ${endX} ${endY}`,
      )
    return
  }

  const cardOnLeft = cardRect.right <= left
  const startX = cardOnLeft ? cardRect.right : cardRect.left
  const startY = cardRect.top + cardRect.height / 2
  const endX = cardOnLeft ? left - 6 : right + 6
  const endY = (top + bottom) / 2
  const bend = Math.abs(endX - startX) * 0.55
  const path = document.querySelector<SVGPathElement>('#guideArrowPath')
  path?.setAttribute(
    'd',
    `M ${startX} ${startY} C ${startX + (cardOnLeft ? bend : -bend)} ${startY}, ${endX + (cardOnLeft ? -bend : bend)} ${endY}, ${endX} ${endY}`,
  )
}

function renderGuideStep(): void {
  const step = guideSteps[guideStepIndex]
  const icon = document.querySelector<HTMLElement>('#guideIcon')!
  icon.className = `guide-icon is-${step.icon}`
  icon.innerHTML = guideIcons[step.icon]
  document.querySelector<HTMLElement>('#guideStepLabel')!.textContent = `${guideStepIndex + 1} / ${guideSteps.length}`
  document.querySelector<HTMLElement>('#guideTitle')!.textContent = step.title
  document.querySelector<HTMLElement>('#guideDescription')!.textContent = step.description
  document.querySelector<HTMLElement>('#guideHint')!.textContent =
    guideStepIndex === guideSteps.length - 1 ? 'Click anywhere to finish' : 'Click anywhere to continue'
  document.querySelectorAll('#guideDots span').forEach((dot, index) => {
    dot.classList.toggle('is-active', index === guideStepIndex)
  })
  requestAnimationFrame(updateGuide)
}

function closeGuide(): void {
  guideStepIndex = -1
  document.querySelector<HTMLElement>('#guideOverlay')!.hidden = true
  document.body.classList.remove('guide-open')
}

function advanceGuide(): void {
  if (guideStepIndex === guideSteps.length - 1) {
    closeGuide()
    return
  }
  guideStepIndex += 1
  renderGuideStep()
}

function openGuide(): void {
  guideStepIndex = 0
  document.querySelector<HTMLElement>('#guideOverlay')!.hidden = false
  document.body.classList.add('guide-open')
  renderGuideStep()
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
  if (!loadedSkill || !runtimeReady || runtimeRunning) return
  applyRequestedResolution()
  runtime?.run(loadedSkill)
  setRunning(true)
  setStatus('Running in browser runtime.', 'running')
}

async function main(): Promise<void> {
  const iframe = renderShell()
  runtime = new RuntimeHost(iframe, {
    onLog: (message, kind) => appendLog(message, kind),
    onState: (state, detail) => {
      if (state === 'ready') {
        runtimeFailed = false
        runtimeReady = true
        setRunning(false)
        setStatus(loadedSkill ? 'Runtime ready. Click Run to start.' : 'Runtime ready.', 'ready')
      } else if (state === 'running') {
        setRunning(true)
        setStatus('Running in browser runtime.', 'running')
      } else if (state === 'stopping') {
        setStatus('Stopping runtime...', 'loading')
      } else if (state === 'exited') {
        setRunning(false)
        if (detail?.stopped || detail?.code === 0) {
          setStatus(detail.stopped ? 'Stopped.' : 'Run completed.', 'ready')
        } else {
          setStatus(`Runtime exited with code ${detail?.code ?? 1}.`, 'error')
        }
      } else if (state === 'error') {
        runtimeFailed = true
        setRunning(false)
        setStatus('Runtime error. See log for details.', 'error')
      }
    },
    onResolution: setDisplayResolution,
  })

  const runButton = document.querySelector<HTMLButtonElement>('#run')
  const previewRunButton = document.querySelector<HTMLButtonElement>('#previewRun')
  const stopButton = document.querySelector<HTMLButtonElement>('#stop')
  const applyResolutionButton = document.querySelector<HTMLButtonElement>('#applyResolution')
  const clearLogButton = document.querySelector<HTMLButtonElement>('#clearLog')
  const guideButton = document.querySelector<HTMLButtonElement>('#guideButton')
  const guideOverlay = document.querySelector<HTMLElement>('#guideOverlay')
  const localUploadButton = document.querySelector<HTMLButtonElement>('#localUploadButton')
  const localUpload = document.querySelector<HTMLInputElement>('#localUpload')

  runButton?.addEventListener('click', runLoadedSkill)
  previewRunButton?.addEventListener('click', runLoadedSkill)
  stopButton?.addEventListener('click', () => {
    runtime?.stop()
    setStatus('Stop requested...', 'loading')
  })
  applyResolutionButton?.addEventListener('click', applyRequestedResolution)
  guideButton?.addEventListener('click', openGuide)
  guideOverlay?.addEventListener('click', advanceGuide)
  window.addEventListener('resize', updateGuide)
  window.addEventListener('scroll', updateGuide, true)
  window.addEventListener('keydown', (event) => {
    if (guideStepIndex < 0) return
    if (event.key === 'Escape') closeGuide()
    if (event.key === 'Enter' || event.key === ' ') {
      event.preventDefault()
      advanceGuide()
    }
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
    setStatus(runtimeReady ? 'Runtime ready. Click Run to start.' : 'Skill loaded. Waiting for runtime...', 'ready')
    setRunning(false)
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error)
    runtimeFailed = true
    setRunning(false)
    appendLog(message, 'error')
    setStatus(message, 'error')
  }
}

main()
