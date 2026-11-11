// @vitest-environment jsdom
// The window's half of the automation bridge, driven through a stand-in event bridge.
import { act, render, screen } from '@testing-library/react'
import { afterEach, describe, expect, it, vi } from 'vitest'
import {
  AUTOMATION_VIEWS,
  HELLO_EVENT,
  NAVIGATE_EVENT,
  STATE_EVENT,
  VIEW_EVENT,
  CONFIG_EVENT,
  isAutomationView,
  readState,
  setAutomationConfig,
  useAutomationNavigation,
} from './automationBridge'
import { AutomationBanner } from '../components/AutomationBanner'
import type { View } from './registry'

type Handler = (e: { payload: unknown }) => void
function fakeBridge() {
  const handlers = new Map<string, Handler[]>()
  const sent: Array<[string, unknown]> = []
  window.__TAURI__ = {
    event: {
      listen: (async (name: string, h: Handler) => {
        handlers.set(name, [...(handlers.get(name) ?? []), h])
        return () => handlers.set(name, (handlers.get(name) ?? []).filter((x) => x !== h))
      }) as never,
      emit: async (name: string, payload?: unknown) => { sent.push([name, payload]) },
    },
  }
  const tell = (name: string, payload: unknown) => (handlers.get(name) ?? []).forEach((h) => h({ payload }))
  return { sent, tell }
}

afterEach(() => { delete window.__TAURI__ })

describe('automation bridge, window half', () => {
  it('reads a malformed state as off, and transmit only while on', () => {
    expect(readState(null).enabled).toBe(false)
    expect(readState({ enabled: false, allowTransmit: true }).allowTransmit).toBe(false)
    expect(readState({ enabled: true, allowTransmit: true, connections: 2 })).toMatchObject({ allowTransmit: true, connections: 2 })
    expect(readState({ connections: -3 }).connections).toBe(0)
  })

  it('knows every screen and nothing else', () => {
    expect(AUTOMATION_VIEWS).toContain('logbook')
    expect(isAutomationView('awards')).toBe(true)
    expect(isAutomationView('rm -rf')).toBe(false)
  })

  it('shows the asked screen through the given switch, and says which screen it shows', async () => {
    const { sent, tell } = fakeBridge()
    const onView = vi.fn()
    const onSettings = vi.fn()
    function Probe({ view }: { view: View }) {
      useAutomationNavigation(view, onView, onSettings)
      return null
    }
    const { rerender } = render(<Probe view="operate" />)
    await act(async () => {})
    tell(NAVIGATE_EVENT, { view: 'logbook', seq: 1 })
    tell(NAVIGATE_EVENT, { view: 'not-a-screen', seq: 2 })
    tell(NAVIGATE_EVENT, { settings: 'Radio ▸ Audio', seq: 3 })
    expect(onView).toHaveBeenCalledTimes(1)
    expect(onView).toHaveBeenCalledWith('logbook')
    expect(onSettings).toHaveBeenCalledWith('Radio ▸ Audio')
    rerender(<Probe view="logbook" />)
    expect(sent).toContainEqual([VIEW_EVENT, { view: 'operate' }])
    expect(sent).toContainEqual([VIEW_EVENT, { view: 'logbook' }])
  })

  it('the banner says hello, stays hidden with no program, and shows while one is connected', async () => {
    const { sent, tell } = fakeBridge()
    render(<AutomationBanner />)
    await act(async () => {})
    expect(sent.some(([n]) => n === HELLO_EVENT)).toBe(true)
    expect(screen.queryByRole('status')).toBeNull()
    act(() => tell(STATE_EVENT, { enabled: true, listening: true, connections: 1, allowTransmit: false }))
    expect(screen.getByRole('status').textContent).toMatch(/Claude is connected/)
    expect(screen.getByRole('status').textContent).toMatch(/transmit off/)
    act(() => tell(STATE_EVENT, { enabled: true, listening: true, connections: 0 }))
    expect(screen.queryByRole('status')).toBeNull()
  })

  it('the switches write through the config event, and off never leaves transmit on', () => {
    const { sent } = fakeBridge()
    setAutomationConfig(false, true)
    setAutomationConfig(true, true)
    expect(sent).toEqual([[CONFIG_EVENT, { enabled: false, allowTransmit: false }], [CONFIG_EVENT, { enabled: true, allowTransmit: true }]])
  })

  it('is inert without the event bridge', () => {
    expect(() => setAutomationConfig(true, false)).not.toThrow()
    expect(() => render(<AutomationBanner />)).not.toThrow()
  })
})
