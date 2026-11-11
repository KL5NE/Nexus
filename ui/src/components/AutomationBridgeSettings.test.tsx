// @vitest-environment jsdom
// The automation bridge's Settings switches: off at a station that never turned it on, set at the
// station from a driving computer, and transmit only after the question is answered yes.
import { act, fireEvent, render, screen } from '@testing-library/react'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { AutomationBridgeSettings } from './AutomationBridgeSettings'
import { CONFIG_EVENT, STATE_EVENT } from '../features/automationBridge'

vi.mock('../confirm', () => ({ confirmDialog: vi.fn() }))
import { confirmDialog } from '../confirm'

type Handler = (e: { payload: unknown }) => void
function fakeBridge() {
  const handlers = new Map<string, Handler[]>()
  const sent: Array<[string, unknown]> = []
  window.__TAURI__ = {
    event: {
      listen: (async (name: string, h: Handler) => {
        handlers.set(name, [...(handlers.get(name) ?? []), h])
        return () => {}
      }) as never,
      emit: async (name: string, payload?: unknown) => { sent.push([name, payload]) },
    },
  }
  return { sent, tell: (n: string, p: unknown) => (handlers.get(n) ?? []).forEach((h) => h({ payload: p })) }
}
afterEach(() => { delete window.__TAURI__; vi.mocked(confirmDialog).mockReset() })

describe('AutomationBridgeSettings', () => {
  it('turns on without transmit, and transmit needs the yes', async () => {
    const { sent, tell } = fakeBridge()
    render(<AutomationBridgeSettings away={false} />)
    await act(async () => {})
    const [onSwitch, txSwitch] = screen.getAllByRole('switch')
    expect(txSwitch.hasAttribute('disabled')).toBe(true)
    fireEvent.click(onSwitch)
    expect(sent).toContainEqual([CONFIG_EVENT, { enabled: true, allowTransmit: false }])
    act(() => tell(STATE_EVENT, { enabled: true, listening: true, connections: 0, port: 40090 }))
    expect(screen.getByText(/Listening on 127\.0\.0\.1:40090/)).toBeTruthy()
    vi.mocked(confirmDialog).mockResolvedValueOnce(false)
    await act(async () => { fireEvent.click(screen.getAllByRole('switch')[1]) })
    expect(sent.filter(([n, p]) => n === CONFIG_EVENT && (p as { allowTransmit: boolean }).allowTransmit)).toHaveLength(0)
    vi.mocked(confirmDialog).mockResolvedValueOnce(true)
    await act(async () => { fireEvent.click(screen.getAllByRole('switch')[1]) })
    expect(sent).toContainEqual([CONFIG_EVENT, { enabled: true, allowTransmit: true }])
  })

  it('is set at the station from a driving computer', async () => {
    fakeBridge()
    render(<AutomationBridgeSettings away />)
    await act(async () => {})
    screen.getAllByRole('switch').forEach((s) => expect(s.hasAttribute('disabled')).toBe(true))
    expect(screen.getByText('Set at the station')).toBeTruthy()
  })
})
