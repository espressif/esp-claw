import type { LoadedSkill } from './types'

export type RuntimeState = 'ready' | 'running' | 'stopping' | 'exited' | 'error'

export interface RuntimeHostCallbacks {
  onLog?: (message: string, kind?: 'info' | 'error') => void
  onState?: (state: RuntimeState, detail?: { code?: number; stopped?: boolean }) => void
  onResolution?: (width: number, height: number) => void
}

export class RuntimeHost {
  private iframe: HTMLIFrameElement
  private ready = false
  private pendingSkill: LoadedSkill | null = null
  private runtimeUrl = './runtime/esp_claw_sim.html?embedded=1&v=unified-ui-1'
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
        if (this.pendingSkill) this.run(this.pendingSkill)
      } else if (data?.type === 'esp-claw-sim:mounted') {
        this.callbacks.onLog?.('skill files mounted')
      } else if (data?.type === 'esp-claw-sim:log') {
        const message = data.message || ''
        this.callbacks.onLog?.(message, message.startsWith('[err]') ? 'error' : 'info')
      } else if (data?.type === 'esp-claw-sim:running') {
        this.callbacks.onState?.('running')
      } else if (data?.type === 'esp-claw-sim:stopping') {
        this.callbacks.onState?.('stopping')
      } else if (data?.type === 'esp-claw-sim:exited') {
        this.callbacks.onState?.('exited', { code: data.code, stopped: data.stopped })
      } else if (
        data?.type === 'esp-claw-sim:resolutionChanged' &&
        typeof data.width === 'number' &&
        typeof data.height === 'number'
      ) {
        this.callbacks.onResolution?.(data.width, data.height)
      } else if (data?.type === 'esp-claw-sim:error') {
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

  run(skill: LoadedSkill): void {
    this.pendingSkill = skill
    if (!this.ready) {
      this.callbacks.onLog?.('waiting for runtime')
      return
    }
    this.callbacks.onLog?.(`running ${skill.entry}`)
    this.postMountAndRun(skill)
  }

  stop(): void {
    this.iframe.contentWindow?.postMessage({ type: 'esp-claw-sim:stop' }, '*')
    this.callbacks.onLog?.('stop requested')
  }

  setResolution(width: number, height: number): void {
    this.iframe.contentWindow?.postMessage({ type: 'esp-claw-sim:setResolution', width, height }, '*')
  }

  private postMountAndRun(skill: LoadedSkill): void {
    const files = skill.files.map((file) => ({
      path: `${skill.virtualRoot}/${file.path}`,
      text: file.text,
      bytes: file.text ? undefined : Array.from(file.content),
    }))

    this.iframe.contentWindow?.postMessage(
      {
        type: 'esp-claw-sim:mountSkill',
        skill: {
          id: skill.frontmatter.name,
          root: skill.virtualRoot,
          entry: `${skill.virtualRoot}/${skill.entry}`,
          files,
          peripherals: skill.frontmatter.metadata.peripherals ?? [],
          capabilityMocks: skill.capabilityMocks,
          simulatorMocks: skill.simulatorMocks,
        },
      },
      '*',
    )
    this.iframe.contentWindow?.postMessage(
      {
        type: 'esp-claw-sim:runSkill',
        path: `${skill.virtualRoot}/${skill.entry}`,
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
