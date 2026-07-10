import type { LoadedSkill } from './types'

export class RuntimeHost {
  private iframe: HTMLIFrameElement
  private ready = false
  private pendingSkill: LoadedSkill | null = null
  private runtimeUrl = './runtime/esp_claw_sim.html?embedded=1'
  private onEvent?: (message: string, kind?: 'info' | 'error') => void

  constructor(iframe: HTMLIFrameElement, onEvent?: (message: string, kind?: 'info' | 'error') => void) {
    this.iframe = iframe
    this.onEvent = onEvent
    window.addEventListener('message', (event) => {
      if (event.source !== this.iframe.contentWindow) return
      const data = event.data as { type?: string }
      if (data?.type === 'esp-claw-sim:ready') {
        this.ready = true
        this.onEvent?.('runtime ready')
        if (this.pendingSkill) this.run(this.pendingSkill)
      } else if (data?.type === 'esp-claw-sim:mounted') {
        this.onEvent?.('skill files mounted')
      } else if (data?.type === 'esp-claw-sim:error') {
        const detail = data as { error?: string }
        this.onEvent?.(detail.error || 'runtime error', 'error')
      }
    })
  }

  async load(): Promise<void> {
    this.ready = false
    await this.assertRuntimeAvailable()
    this.iframe.src = this.runtimeUrl
    this.onEvent?.('runtime frame loaded')
  }

  run(skill: LoadedSkill): void {
    this.pendingSkill = skill
    if (!this.ready) {
      this.onEvent?.('waiting for runtime')
      return
    }
    this.onEvent?.(`running ${skill.entry}`)
    this.postMountAndRun(skill)
  }

  stop(): void {
    this.iframe.contentWindow?.postMessage({ type: 'esp-claw-sim:stop' }, '*')
    this.onEvent?.('stop requested')
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
