import type { LoadedApp } from './types'
import { buildAppBootstrap } from './app-bootstrap'

export type RuntimeState = 'ready' | 'running' | 'stopping' | 'exited' | 'error'

export interface RuntimeHostCallbacks {
  onLog?: (message: string, kind?: 'info' | 'error') => void
  onState?: (state: RuntimeState, detail?: { code?: number; stopped?: boolean }) => void
  onResolution?: (width: number, height: number) => void
}

export class RuntimeHost {
  private iframe: HTMLIFrameElement
  private ready = false
  private running = false
  private pendingApp: LoadedApp | null = null
  private runtimeUrl = './runtime/esp_claw_sim.html?embedded=1&v=display-1'
  private callbacks: RuntimeHostCallbacks

  constructor(iframe: HTMLIFrameElement, callbacks: RuntimeHostCallbacks = {}) {
    this.iframe = iframe
    this.callbacks = callbacks
    window.addEventListener('message', (event) => {
      if (event.source !== this.iframe.contentWindow) return
      const data = event.data as {
        type?: string
        message?: string
        error?: string
        code?: number
        stopped?: boolean
        width?: number
        height?: number
      }
      if (data?.type === 'esp-claw-sim:ready') {
        this.ready = true
        this.callbacks.onLog?.('runtime ready')
        this.callbacks.onState?.('ready')
        if (typeof data.width === 'number' && typeof data.height === 'number') {
          this.callbacks.onResolution?.(data.width, data.height)
        }
        if (this.pendingApp) this.run(this.pendingApp)
      } else if (data?.type === 'esp-claw-sim:mounted') {
        this.callbacks.onLog?.('App files mounted')
      } else if (data?.type === 'esp-claw-sim:log') {
        const message = data.message || ''
        this.callbacks.onLog?.(message, message.startsWith('[err]') ? 'error' : 'info')
      } else if (data?.type === 'esp-claw-sim:running') {
        this.callbacks.onState?.('running')
      } else if (data?.type === 'esp-claw-sim:stopping') {
        this.callbacks.onState?.('stopping')
      } else if (data?.type === 'esp-claw-sim:exited') {
        this.running = false
        this.callbacks.onState?.('exited', { code: data.code, stopped: data.stopped })
        if (this.pendingApp) this.run(this.pendingApp)
      } else if (
        data?.type === 'esp-claw-sim:resolutionChanged' &&
        typeof data.width === 'number' &&
        typeof data.height === 'number'
      ) {
        this.callbacks.onResolution?.(data.width, data.height)
      } else if (data?.type === 'esp-claw-sim:error') {
        this.running = false
        this.pendingApp = null
        this.callbacks.onLog?.(data.error || 'runtime error', 'error')
        this.callbacks.onState?.('error')
      }
    })
  }

  async load(): Promise<void> {
    this.ready = false
    await this.assertRuntimeAvailable()
    this.iframe.src = this.runtimeUrl
    this.callbacks.onLog?.('runtime frame loaded')
  }

  run(app: LoadedApp): void {
    this.pendingApp = app
    if (!this.ready) {
      this.callbacks.onLog?.('waiting for runtime')
      return
    }
    if (this.running) {
      this.iframe.contentWindow?.postMessage({ type: 'esp-claw-sim:stop' }, '*')
      return
    }
    this.pendingApp = null
    this.running = true
    this.callbacks.onLog?.(`running ${app.entry}`)
    try {
      this.postMountAndRun(app)
    } catch (error) {
      this.running = false
      this.callbacks.onLog?.(error instanceof Error ? error.message : String(error), 'error')
      this.callbacks.onState?.('error')
    }
  }

  stop(): void {
    this.pendingApp = null
    this.iframe.contentWindow?.postMessage({ type: 'esp-claw-sim:stop' }, '*')
    this.callbacks.onLog?.('stop requested')
  }

  setResolution(width: number, height: number): void {
    this.iframe.contentWindow?.postMessage({ type: 'esp-claw-sim:setResolution', width, height }, '*')
  }

  private postMountAndRun(app: LoadedApp): void {
    const files = app.files.map((file) => ({
      path: `${app.virtualRoot}/${file.path}`,
      text: file.text,
      bytes: file.text !== undefined ? undefined : Array.from(file.content),
    }))

    const bootstrap = '/__simulator__/launch.lua'
    files.push({ path: bootstrap, text: buildAppBootstrap(app), bytes: undefined })
    this.iframe.contentWindow?.postMessage(
      {
        type: 'esp-claw-sim:mountApp',
        app: {
          id: app.manifest.id,
          root: app.virtualRoot,
          entry: bootstrap,
          files,
          peripherals: app.peripherals,
          capabilityMocks: app.capabilityMocks,
          simulatorMocks: app.simulatorMocks,
        },
      },
      '*',
    )
    this.iframe.contentWindow?.postMessage(
      {
        type: 'esp-claw-sim:runApp',
        path: bootstrap,
      },
      '*',
    )
  }

  private async assertRuntimeAvailable(): Promise<void> {
    const response = await fetch(this.runtimeUrl, { cache: 'no-store' })
    if (!response.ok) {
      throw new Error(`simulator runtime is missing: HTTP ${response.status}`)
    }

    const html = await response.text()
    if (!html.includes('esp-claw-sim:ready') || !html.includes('window.espClawSim')) {
      throw new Error(
        'simulator runtime is missing or invalid. Build/copy esp_claw_sim.html/js/wasm/data into pages/simulator/public/runtime first.',
      )
    }
  }
}
