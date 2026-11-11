// The strip the main window shows while a program is connected to the automation bridge
// (src-tauri/src/automation_bridge.rs): who is connected and whether it may transmit. An in-flow strip
// like the client banner, so it covers nothing. It renders nothing while no program is connected.
//
// ⚠️ ENGLISH ONLY FOR NOW: a KL5NE-build feature, so its words are named constants rather than catalog
// entries (the catalogs must translate every key, and machine translation is not shipped). Move them
// into the catalog if this is offered upstream.
import { useAutomationState } from '../features/automationBridge'

const BANNER = 'Claude is connected to Nexus through the automation bridge'
const TX_ON = 'transmit allowed, with confirmation'
const TX_OFF = 'transmit off'

export function AutomationBanner() {
  const state = useAutomationState()
  if (state.connections === 0) return null
  return (
    <div className="client-banner automation-banner" role="status" data-automation-tx={state.allowTransmit ? 'on' : 'off'}>
      <span className="client-banner-line">
        {BANNER} · {state.allowTransmit ? TX_ON : TX_OFF}
      </span>
    </div>
  )
}
