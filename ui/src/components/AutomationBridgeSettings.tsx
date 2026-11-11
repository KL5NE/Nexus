// Settings ▸ Services ▸ Integrations & Feeds: the automation bridge's two switches. They write the
// bridge's own file (automation-bridge.json, beside settings.json) through the `automation-config`
// event, not through Save: the bridge picks the change up within two seconds. A computer driving a
// station shows them as set at the station, since the bridge is the station's.
//
// ⚠️ ENGLISH ONLY FOR NOW, as AutomationBanner: named constants, not catalog entries.
import { t } from '../i18n'
import { setAutomationConfig, useAutomationState } from '../features/automationBridge'
import { confirmDialog } from '../confirm'

const LABEL = 'Claude automation bridge'
const HINT =
  "Lets one program on this computer, the KL5NE Nexus Connector for Claude, run Nexus's own commands with no screen or mouse: tune, read the decoders and spots, switch screens. This computer only, with a key. Off as it comes."
const TX_LABEL = 'Programs on the bridge may transmit'
const TX_HINT =
  'Off: the bridge tunes, reads and switches screens, and never transmits. On: each transmission still needs the program to confirm it, and every safeguard in front of your own transmissions still applies: TX Enable, your licence privileges, the power limits and Stop TX.'
const TX_CONFIRM_TITLE = 'Let the program on the automation bridge transmit?'
const TX_CONFIRM_BODY =
  'Each transmission still needs the program to confirm it, every Nexus transmit safeguard still applies, and Stop TX always works.'
const TX_CONFIRM_YES = 'Allow transmit'
const STARTING = 'Starting…'
const listeningOn = (port: number, count: number) => `Listening on 127.0.0.1:${port}. Programs connected: ${count}.`

export function AutomationBridgeSettings({ away }: { away: boolean }) {
  const state = useAutomationState()
  const toggleOn = () => setAutomationConfig(!state.enabled, false)
  const toggleTx = async () => {
    if (!state.allowTransmit) {
      const ok = await confirmDialog({ title: TX_CONFIRM_TITLE, body: TX_CONFIRM_BODY, confirmLabel: TX_CONFIRM_YES, danger: true })
      if (!ok) return
    }
    setAutomationConfig(true, !state.allowTransmit)
  }
  return (
    <>
      <div className="settings-field" data-automation-setting="enabled">
        <label className="settings-toggle">
          <span className="settings-label">{LABEL}</span>
          <button
            type="button"
            role="switch"
            disabled={away}
            aria-checked={state.enabled}
            className={`toggle${state.enabled ? ' on' : ''}`}
            onClick={toggleOn}
          >
            <span className="toggle-knob" />
          </button>
        </label>
        {away && <span className="settings-hint">{t('settings.stationOnly.hint')}</span>}
        <span className="settings-hint">{HINT}</span>
        {state.enabled && !away && (
          <span className="settings-hint" data-automation-status>
            {state.listening
              ? listeningOn(state.port, state.connections)
              : STARTING}
          </span>
        )}
      </div>
      <div className="settings-field" data-automation-setting="allowTransmit">
        <label className="settings-toggle">
          <span className="settings-label">{TX_LABEL}</span>
          <button
            type="button"
            role="switch"
            disabled={away || !state.enabled}
            aria-checked={state.allowTransmit}
            className={`toggle${state.allowTransmit ? ' on' : ''}`}
            onClick={() => void toggleTx()}
          >
            <span className="toggle-knob" />
          </button>
        </label>
        <span className="settings-hint">{TX_HINT}</span>
      </div>
    </>
  )
}
