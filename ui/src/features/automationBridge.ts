// THE AUTOMATION BRIDGE, THE WINDOW'S HALF (src-tauri/src/automation_bridge.rs).
//
// The bridge lets one program on this computer (KL5NE's Claude connector) run Nexus's own commands
// by name. Three things need this window, and each travels as a Tauri event, never a command:
//
//  - **A screen to show** (`automation-navigate`): the program asks, and the window makes exactly the
//    switch a click on its rail makes (`handleView`), or opens Settings at a place. Nothing else:
//    a screen change keys nothing, arms nothing.
//  - **Which screen it shows** (`automation-view`, sent on every change) so the program can say
//    where the operator is looking.
//  - **The bridge's state** (`automation-state`, answered to `automation-hello`): whether it is on,
//    whether transmit is allowed, how many programs are connected. The strip
//    (`AutomationBanner`) and the Settings switches (`AutomationBridgeSettings`) show it.
//
// The switches write the bridge's file through `automation-config`. Without the event bridge (a
// browser preview, the hosted page, the TV page) every part of this is inert.
import { useEffect, useRef, useState } from 'react'
import type { View } from './registry'

export const NAVIGATE_EVENT = 'automation-navigate'
export const STATE_EVENT = 'automation-state'
export const VIEW_EVENT = 'automation-view'
export const HELLO_EVENT = 'automation-hello'
export const CONFIG_EVENT = 'automation-config'

/** Every screen a program may ask for: `View`, exactly (the type below fails to compile when a View
 *  is added and not listed). The bridge's own list, `VIEWS` in automation_bridge.rs, is held to the
 *  same union by a Rust test. */
export const AUTOMATION_VIEWS = [
  'operate', 'cw', 'phone', 'rtty', 'psk', 'sstv', 'aprs', 'js8', 'jtty', 'connect', 'dxped',
  'sats', 'needed', 'spots', 'chat', 'fieldDay', 'logbook', 'awards', 'stats', 'pota',
  'memories', 'program', 'settings',
] as const satisfies readonly View[]
type Unlisted = Exclude<View, (typeof AUTOMATION_VIEWS)[number]>
const ALL_LISTED: [Unlisted] extends [never] ? true : Unlisted = true
void ALL_LISTED

export function isAutomationView(v: string): v is View {
  return (AUTOMATION_VIEWS as readonly string[]).includes(v)
}

export interface AutomationState {
  enabled: boolean
  allowTransmit: boolean
  port: number
  listening: boolean
  connections: number
}

export interface NavigateAsk {
  view?: string | null
  settings?: string | null
  seq?: number
}

export const OFF_STATE: AutomationState = {
  enabled: false,
  allowTransmit: false,
  port: 40090,
  listening: false,
  connections: 0,
}

/** A state as told, read defensively: anything malformed reads as off. */
export function readState(p: unknown): AutomationState {
  const o = (p ?? {}) as Partial<Record<keyof AutomationState, unknown>>
  return {
    enabled: o.enabled === true,
    allowTransmit: o.enabled === true && o.allowTransmit === true,
    port: typeof o.port === 'number' ? o.port : 40090,
    listening: o.listening === true,
    connections: typeof o.connections === 'number' && o.connections > 0 ? Math.floor(o.connections) : 0,
  }
}

function emitEvent(name: string, payload: unknown): void {
  const emit = window.__TAURI__?.event?.emit
  if (emit) void emit(name, payload).catch(() => {})
}

function subscribe<T>(name: string, on: (payload: T) => void): () => void {
  let alive = true
  let unlisten: (() => void) | undefined
  const listen = window.__TAURI__?.event?.listen
  if (listen) {
    void listen<T>(name, (e) => { if (alive) on(e.payload) }).then(
      (un) => { if (alive) unlisten = un; else un() },
      () => { /* a listener that fails to register must not take the app down */ },
    )
  }
  return () => { alive = false; unlisten?.() }
}

/** Turn the bridge on or off, and allow or forbid transmit (the Settings switches). */
export function setAutomationConfig(enabled: boolean, allowTransmit: boolean): void {
  emitEvent(CONFIG_EVENT, { enabled, allowTransmit: enabled && allowTransmit })
}

/** The bridge's state, asked once and then told each change. Every subscriber asks: the answer
 *  is cheap and the module keeps no copy. */
export function useAutomationState(): AutomationState {
  const [state, setState] = useState<AutomationState>(OFF_STATE)
  useEffect(() => {
    const off = subscribe<unknown>(STATE_EVENT, (p) => setState(readState(p)))
    emitEvent(HELLO_EVENT, {})
    return off
  }, [])
  return state
}

/** The main window's half: show the screens the program asks for, and say which one is showing. */
export function useAutomationNavigation(
  view: View,
  onView: (view: View) => void,
  onSettings: (where: string) => void,
): void {
  const latest = useRef({ onView, onSettings })
  latest.current = { onView, onSettings }
  useEffect(
    () =>
      subscribe<NavigateAsk>(NAVIGATE_EVENT, (ask) => {
        const { onView: go, onSettings: settings } = latest.current
        if (typeof ask?.settings === 'string' && ask.settings) settings(ask.settings)
        else if (typeof ask?.view === 'string' && isAutomationView(ask.view)) go(ask.view)
      }),
    [],
  )
  useEffect(() => {
    emitEvent(VIEW_EVENT, { view })
  }, [view])
}
