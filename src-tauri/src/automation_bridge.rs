//! The automation bridge: Nexus's own commands for ONE program on this computer (KL5NE's Claude
//! connector), by name, exactly as the main window's screen runs them, with no screen and no mouse.
//!
//! It is the paired-computer command lane's idea brought home: every call goes through the app's
//! own handler ([`crate::command_dispatch::tauri_runner`]), and every command is admitted by its
//! row in the class table (`tempo_client::classes`), so a command added to Nexus is reachable here
//! the day it is classed, and a command with no row is refused.
//!
//! ## Off until the operator turns it on
//!
//! Nothing listens until `automation-bridge.json` exists in this instance's config folder (beside
//! `settings.json`) and says `"enabled": true` with a token of at least 32 characters. One manager
//! thread reads that file every [`POLL`] and hot-applies it, in the CAT broker's and TCI server's
//! mold: turning it on starts the listener with no restart, off (or deleting the file) stops it and
//! closes every connection, a new port rebinds.
//!
//! ```json
//! { "enabled": true, "port": 40090, "token": "<64 hex chars>", "allowTransmit": false }
//! ```
//!
//! ## The locks
//!
//! - **This computer only.** The listener binds `127.0.0.1` and nothing else, and a peer that is not
//!   loopback is dropped on arrival anyway. It is raw TCP with JSON lines, which a web page cannot
//!   open, so no browser tab can reach it.
//! - **A token.** The first line on a connection must be `{"hello":"<token>"}`; a wrong one is told
//!   so and closed. The token lives in the file above, which only this Windows user can read.
//! - **By class** ([`admit`]):
//!   - `RunsAtShack` (the radio, the cockpits, decodes, spots, the logbook…) runs.
//!   - `StopsTx` always runs. Stop is never refused.
//!   - `StartsTx` runs only with `"allowTransmit": true` in the file AND `"confirmTransmit": true` on
//!     the call. Off by default. Every gate in front of Nexus's own transmissions still applies:
//!     TX Enable, the licence-class lockout, the power ceilings, the watchdog.
//!   - `RefusedHere` (credentials, remote-access admin, bulk log rewrites) never runs here.
//!   - `RunsHere` (this computer's windows, file dialogs, updater) is refused, but for [`LOCAL_OK`].
//!   - A small set of commands that change or delete data ([`NEEDS_CONFIRM`]) runs only with
//!     `"confirmChange": true` on the call.
//!   - `set_settings` may not change a transmit safeguard ([`PROTECTED_SETTINGS`]): those are set
//!     at the station's own screen.
//! - **The internal `afar` argument is stripped** from every call: only the paired-computer lane
//!   writes it, and a program here must not pose as a computer driving the station from afar.
//! - **Bounded.** [`MAX_CONNECTIONS`] connections, [`MAX_LINE`] bytes a request, [`IN_FLIGHT`] calls
//!   at once on a connection, [`PER_SECOND`] calls a second, and an answer waited on for at most
//!   [`ANSWER_WAIT`].
//!
//! ## The wire
//!
//! One JSON object per line, both ways (UTF-8, `\n`):
//!
//! - `→ {"hello":"<token>"}` / `← {"hello":"nexus-automation","protocol":1,"allowTransmit":false}`
//! - `→ {"id":7,"cmd":"cw_decode","args":{"sensitivity":0.5}}` / `← {"id":7,"ok":true,"value":{…}}`
//! - a refusal or a command's own error: `← {"id":7,"ok":false,"error":"…","refused":true|false}`
//! - `→ {"id":8,"cmd":"bridge.commands"}` answers the class table: every command, class and reason.
//! - `→ {"id":9,"cmd":"bridge.ping"}` answers `"pong"`.
//! - `→ {"id":10,"cmd":"bridge.navigate","args":{"view":"logbook"}}` asks the main window to show a
//!   screen exactly as a click on its rail does (`handleView`), or `{"settings":"Radio ▸ Audio"}` to
//!   open Settings at a place. It never transmits: it is the rail's click and nothing more.
//! - `→ {"id":11,"cmd":"bridge.view"}` answers the screen the main window says it is showing.
//! - `→ {"id":12,"cmd":"bridge.alerts","args":{"since":0}}` answers the alerts Nexus raised (Pounce,
//!   needed DXCC, new parks: the `alert` events its own window hears), newest last, kept in memory.
//! - `→ {"id":13,"cmd":"bridge.state"}` answers whether it listens, how many programs are connected,
//!   and whether transmit is allowed.
//!
//! ## Its own screen
//!
//! The main window shows a small strip while a program is connected ("Claude is connected"), and
//! Settings ▸ Services ▸ Integrations & Feeds has the on/off switch and the transmit switch, which
//! write the same file. The window and this module talk by Tauri events only ([`UI_EVENTS`] in,
//! [`STATE_EVENT`] and [`NAVIGATE_EVENT`] out): no new command, so the class table is untouched.
//! A computer driving a station (client mode) runs no bridge, and its Settings shows the switches as
//! set at the station.

use std::collections::{HashMap, VecDeque};
use std::io::{ErrorKind, Read, Write};
use std::net::{Ipv4Addr, Shutdown, SocketAddr, TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant};

use serde_json::{json, Map, Value};

use crate::command_classes::{class_of, Class};
use crate::command_dispatch::{Runner, AFAR};

/// The file that turns the bridge on, beside `settings.json`.
pub(crate) const CONFIG_FILE: &str = "automation-bridge.json";
/// The port when the file names none.
pub(crate) const DEFAULT_PORT: u16 = 40090;
/// How often the manager reads the file.
const POLL: Duration = Duration::from_secs(2);
/// The shortest token accepted.
pub(crate) const MIN_TOKEN: usize = 32;
/// Connections at once.
pub(crate) const MAX_CONNECTIONS: usize = 4;
/// The longest request line.
pub(crate) const MAX_LINE: usize = 1024 * 1024;
/// Calls running at once on one connection.
pub(crate) const IN_FLIGHT: usize = 8;
/// Calls a second on one connection.
pub(crate) const PER_SECOND: u32 = 60;
/// How long a call's answer is waited for.
const ANSWER_WAIT: Duration = Duration::from_secs(60);
/// A connection that has not said hello by then is closed.
const HELLO_WAIT: Duration = Duration::from_secs(10);
/// The wire's version.
pub(crate) const PROTOCOL: u32 = 1;
/// The connection log's lane.
const LANE: &str = "Automation bridge";

/// `RunsHere` commands that are harmless here: they read this build and change nothing.
pub(crate) const LOCAL_OK: &[&str] = &["app_version"];

/// Station commands that change or delete the operator's data, or post to the outside world: each
/// needs `"confirmChange": true` on the call, which the connector sends only when the operator
/// asked for that change.
pub(crate) const NEEDS_CONFIRM: &[&str] = &[
    "cluster_self_spot",
    "contest_entry_reset",
    "contest_remove_last",
    "contest_restore",
    "contest_self_spot",
    "delete_qso",
    "delete_qso_by_id",
    "edit_qso",
    "edit_qso_by_id",
    "import_adif",
    "import_call_history",
    "import_hunted_parks_csv",
    "import_log",
    "import_parks_csv",
    "import_pota_log",
    "import_tles",
    "import_voice_message",
    "js8_inbox_delete",
    "log_qso",
    "mark_lotw_uploaded",
    "post_spot",
    "radioprog_delete_project",
    "remove_radio",
    "reset_lotw_cursor",
    "restore_deleted_qso",
    "self_spot_activation",
    "set_settings",
    "sstv_delete_image",
    "sstv_delete_images",
    "undo_qso_change",
    "upload_lotw_report",
];

/// Settings that guard the transmitter. A `set_settings` through the bridge that would change any
/// of them is refused: the operator sets these at the station's own screen.
pub(crate) const PROTECTED_SETTINGS: &[&str] = &[
    "licenseClass",
    "tciServerTx",
    "tciServerCw",
    "catBrokerPtt",
    "wsjtxUdpAcceptOtherComputers",
    "js8ApiTcpAccept",
    "js8ApiUdpAccept",
    "maxPowerPhone",
    "maxPowerCw",
    "maxPowerDigital",
    "maxPowerAm",
    "txWatchdogMin",
];

/// What `automation-bridge.json` says.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Config {
    pub enabled: bool,
    pub port: u16,
    pub token: String,
    pub allow_transmit: bool,
}

impl Config {
    /// Read the file's text. `None` when it is not JSON, is off, or its token is too short: the
    /// bridge is off in every one of those cases.
    pub(crate) fn parse(text: &str) -> Option<Config> {
        let v: Value = serde_json::from_str(text).ok()?;
        let enabled = v.get("enabled").and_then(Value::as_bool).unwrap_or(false);
        let token = v
            .get("token")
            .and_then(Value::as_str)
            .unwrap_or("")
            .trim()
            .to_string();
        let port = v
            .get("port")
            .and_then(Value::as_u64)
            .filter(|p| (1024..=65535).contains(p))
            .map(|p| p as u16)
            .unwrap_or(DEFAULT_PORT);
        let allow_transmit = v
            .get("allowTransmit")
            .and_then(Value::as_bool)
            .unwrap_or(false);
        (enabled && token.len() >= MIN_TOKEN && token.chars().all(|c| c.is_ascii_graphic()))
            .then_some(Config {
                enabled,
                port,
                token,
                allow_transmit,
            })
    }

    fn read(dir: &Path) -> Option<Config> {
        Config::parse(&std::fs::read_to_string(dir.join(CONFIG_FILE)).ok()?)
    }
}

/// Events the main window sends this module.
pub(crate) const UI_EVENTS: &[&str] = &["automation-view", "automation-hello", "automation-config"];
/// The bridge's state, to the main window (the strip and the Settings switches).
pub(crate) const STATE_EVENT: &str = "automation-state";
/// A screen to show, to the main window.
pub(crate) const NAVIGATE_EVENT: &str = "automation-navigate";
/// Alerts kept for programs to read.
pub(crate) const ALERTS_KEPT: usize = 200;

/// The screens a program may ask for: `View` in ui/src/features/registry.ts (a test holds the two
/// lists together).
pub(crate) const VIEWS: &[&str] = &[
    "operate", "cw", "phone", "rtty", "psk", "sstv", "aprs", "js8", "jtty", "connect", "dxped",
    "sats", "needed", "spots", "chat", "fieldDay", "logbook", "awards", "stats", "pota",
    "memories", "program", "settings",
];

/// How the module tells the main window something: the app's `emit_to("main", …)`.
pub(crate) type Emit = Arc<dyn Fn(&'static str, Value) + Send + Sync>;

#[derive(Default)]
struct Shared {
    emit: Option<Emit>,
    dir: Option<PathBuf>,
    view: Option<String>,
    alerts: VecDeque<Value>,
    next_alert: u64,
    connections: usize,
    listening: Option<Config>,
    navigate_seq: u64,
}

fn shared() -> std::sync::MutexGuard<'static, Shared> {
    static S: OnceLock<Mutex<Shared>> = OnceLock::new();
    S.get_or_init(|| Mutex::new(Shared::default()))
        .lock()
        .unwrap_or_else(|p| p.into_inner())
}

fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

/// What the strip and the Settings switches show.
pub(crate) fn state_value() -> Value {
    let (dir, listening, connections) = {
        let s = shared();
        (s.dir.clone(), s.listening.clone(), s.connections)
    };
    let file = dir
        .as_deref()
        .and_then(|d| std::fs::read_to_string(d.join(CONFIG_FILE)).ok())
        .and_then(|t| serde_json::from_str::<Value>(&t).ok())
        .unwrap_or(Value::Null);
    json!({
        "enabled": file.get("enabled").and_then(Value::as_bool).unwrap_or(false),
        "allowTransmit": file.get("allowTransmit").and_then(Value::as_bool).unwrap_or(false),
        "port": file.get("port").and_then(Value::as_u64).unwrap_or(DEFAULT_PORT as u64),
        "listening": listening.is_some(),
        "connections": connections,
    })
}

fn emit_state() {
    let emit = shared().emit.clone();
    if let Some(emit) = emit {
        emit(STATE_EVENT, state_value());
    }
}

/// A new random token, 64 hex characters, from the system's secure generator.
fn new_token() -> Option<String> {
    use ring::rand::SecureRandom;
    let mut b = [0u8; 32];
    ring::rand::SystemRandom::new().fill(&mut b).ok()?;
    Some(b.iter().map(|x| format!("{x:02x}")).collect())
}

/// Write the switch's answer into the file, keeping its port and token (a token is made the first
/// time). Written beside and renamed, so the manager never reads half a file.
pub(crate) fn write_config(dir: &Path, enabled: bool, allow_transmit: bool) -> std::io::Result<()> {
    let path = dir.join(CONFIG_FILE);
    let old: Value = std::fs::read_to_string(&path)
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or(Value::Null);
    let token = old
        .get("token")
        .and_then(Value::as_str)
        .filter(|t| t.len() >= MIN_TOKEN)
        .map(str::to_string)
        .or_else(new_token)
        .ok_or_else(|| std::io::Error::other("no secure random source"))?;
    let port = old
        .get("port")
        .and_then(Value::as_u64)
        .unwrap_or(DEFAULT_PORT as u64);
    let text = serde_json::to_string_pretty(&json!({
        "enabled": enabled,
        "port": port,
        "token": token,
        "allowTransmit": enabled && allow_transmit,
    }))
    .unwrap_or_default();
    let tmp = dir.join(format!("{CONFIG_FILE}.tmp"));
    std::fs::write(&tmp, text)?;
    std::fs::rename(&tmp, &path)
}

/// An event from the main window ([`UI_EVENTS`]).
pub(crate) fn ui_event(name: &str, payload: &str) {
    let v: Value = serde_json::from_str(payload).unwrap_or(Value::Null);
    match name {
        "automation-view" => {
            if let Some(view) = v
                .get("view")
                .and_then(Value::as_str)
                .filter(|x| VIEWS.contains(x))
            {
                shared().view = Some(view.to_string());
            }
        }
        "automation-hello" => emit_state(),
        "automation-config" => {
            let dir = shared().dir.clone();
            if let Some(dir) = dir {
                let enabled = v.get("enabled").and_then(Value::as_bool).unwrap_or(false);
                let allow = v
                    .get("allowTransmit")
                    .and_then(Value::as_bool)
                    .unwrap_or(false);
                let _ = write_config(&dir, enabled, allow);
            }
            emit_state();
        }
        _ => {}
    }
}

/// An alert Nexus raised (`alert` event): kept for programs to read with `bridge.alerts`.
pub(crate) fn alert(name: &str, payload: &str) {
    let data: Value =
        serde_json::from_str(payload).unwrap_or_else(|_| Value::String(payload.to_string()));
    let mut s = shared();
    s.next_alert += 1;
    let entry = json!({"id": s.next_alert, "event": name, "atMs": now_ms(), "data": data});
    s.alerts.push_back(entry);
    while s.alerts.len() > ALERTS_KEPT {
        s.alerts.pop_front();
    }
}

/// The bridge's own commands (`bridge.*`): `None` for every other command.
pub(crate) fn bridge_command(cmd: &str, args: &Value) -> Option<Result<Value, String>> {
    Some(match cmd {
        "bridge.ping" => Ok(json!("pong")),
        "bridge.commands" => Ok(Value::Array(
            tempo_client::classes::CLASSES
                .iter()
                .map(|(name, class, why)| json!({"cmd": name, "class": format!("{class:?}"), "why": why}))
                .collect(),
        )),
        "bridge.state" => Ok(state_value()),
        "bridge.view" => Ok(json!({"view": shared().view.clone()})),
        "bridge.alerts" => {
            let since = args.get("since").and_then(Value::as_u64).unwrap_or(0);
            let limit = args.get("limit").and_then(Value::as_u64).unwrap_or(50).min(ALERTS_KEPT as u64) as usize;
            let s = shared();
            let newer: Vec<Value> = s
                .alerts
                .iter()
                .filter(|a| a["id"].as_u64().unwrap_or(0) > since)
                .cloned()
                .collect();
            let skip = newer.len().saturating_sub(limit);
            Ok(json!({"alerts": newer[skip..].to_vec(), "latestId": s.next_alert}))
        }
        "bridge.navigate" => {
            let view = args.get("view").and_then(Value::as_str);
            let place = args.get("settings").and_then(Value::as_str).filter(|p| !p.is_empty() && p.len() <= 120);
            if view.is_none() && place.is_none() {
                return Some(Err("bridge.navigate: give a view or a settings place".into()));
            }
            if let Some(v) = view {
                if !VIEWS.contains(&v) {
                    return Some(Err(format!("bridge.navigate: no screen named {v}; one of {}", VIEWS.join(", "))));
                }
            }
            let (emit, seq) = {
                let mut s = shared();
                s.navigate_seq += 1;
                (s.emit.clone(), s.navigate_seq)
            };
            match emit {
                Some(emit) => {
                    emit(NAVIGATE_EVENT, json!({"view": view, "settings": place, "seq": seq}));
                    Ok(json!({"asked": view.unwrap_or("settings"), "settings": place, "seq": seq}))
                }
                None => Err("bridge.navigate: the main window is not attached".into()),
            }
        }
        _ => return None,
    })
}

/// One call, as it arrives.
#[derive(Debug, PartialEq)]
pub(crate) struct Request {
    pub id: Value,
    pub cmd: String,
    pub args: Value,
    pub confirm_transmit: bool,
    pub confirm_change: bool,
}

impl Request {
    pub(crate) fn parse(line: &[u8]) -> Result<Request, String> {
        let v: Value = serde_json::from_slice(line).map_err(|_| "notJson".to_string())?;
        let id = v.get("id").cloned().unwrap_or(Value::Null);
        let cmd = v
            .get("cmd")
            .and_then(Value::as_str)
            .filter(|c| !c.is_empty() && c.len() <= 64)
            .ok_or_else(|| "noCommand".to_string())?
            .to_string();
        let mut args = v
            .get("args")
            .cloned()
            .unwrap_or_else(|| Value::Object(Map::new()));
        if !args.is_object() {
            return Err("argsMustBeAnObject".to_string());
        }
        // Only the paired-computer lane writes `afar`.
        if let Some(map) = args.as_object_mut() {
            map.remove(AFAR);
        }
        Ok(Request {
            id,
            cmd,
            args,
            confirm_transmit: v.get("confirmTransmit").and_then(Value::as_bool) == Some(true),
            confirm_change: v.get("confirmChange").and_then(Value::as_bool) == Some(true),
        })
    }
}

/// Whether a call runs, and if not, why.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Admission {
    Run,
    Refused(String),
}

/// ⛔ The bridge's whole policy, by the command's class. Pure, so every rule is a test.
pub(crate) fn admit(req: &Request, allow_transmit: bool) -> Admission {
    let refuse = |why: &str| Admission::Refused(format!("{}: {why}", req.cmd));
    let Some(class) = class_of(&req.cmd) else {
        return refuse("no such command in this build of Nexus");
    };
    match class {
        Class::StopsTx => Admission::Run,
        Class::RefusedHere => refuse("set at the station's own screen only (credentials, remote-access admin or a bulk log change)"),
        Class::RunsHere if LOCAL_OK.contains(&req.cmd.as_str()) => Admission::Run,
        Class::RunsHere => refuse("a command for this computer's own windows or files, not the station"),
        Class::StartsTx if !allow_transmit => refuse("transmitting through the automation bridge is off (\"allowTransmit\" in automation-bridge.json)"),
        Class::StartsTx if !req.confirm_transmit => refuse("can transmit: the call must carry confirmTransmit"),
        Class::StartsTx => Admission::Run,
        Class::RunsAtShack if NEEDS_CONFIRM.contains(&req.cmd.as_str()) && !req.confirm_change => {
            refuse("changes or deletes your data, or posts outside: the call must carry confirmChange")
        }
        Class::RunsAtShack => Admission::Run,
    }
}

/// The protected settings a `set_settings` would change, compared with what is in force now.
pub(crate) fn protected_changes(current: &Value, proposed: &Value) -> Vec<&'static str> {
    PROTECTED_SETTINGS
        .iter()
        .copied()
        .filter(|key| match proposed.get(*key) {
            None => false, // a partial save leaves it as it is
            Some(new) => current.get(*key) != Some(new),
        })
        .collect()
}

/// Constant-time token comparison.
pub(crate) fn token_matches(given: &str, want: &str) -> bool {
    let (a, b) = (given.as_bytes(), want.as_bytes());
    let mut diff = (a.len() ^ b.len()) as u32;
    for i in 0..a.len().max(b.len()) {
        diff |= u32::from(a.get(i).copied().unwrap_or(0) ^ b.get(i).copied().unwrap_or(0));
    }
    diff == 0
}

/// Run one command through the app's handler and wait for its answer.
fn run(runner: &Runner, cmd: &str, args: Value) -> Result<Value, String> {
    let (tx, rx) = mpsc::channel();
    runner(
        cmd.to_string(),
        args,
        Box::new(move |answer| {
            let _ = tx.send(answer);
        }),
    );
    rx.recv_timeout(ANSWER_WAIT).unwrap_or_else(|_| {
        Err(format!(
            "{cmd}: no answer within {} s",
            ANSWER_WAIT.as_secs()
        ))
    })
}

/// Answer one admitted-or-not call. Pure apart from the runner, so the tests drive it with a mock.
pub(crate) fn answer(runner: &Runner, req: Request, allow_transmit: bool) -> Value {
    let id = req.id.clone();
    let reply_err = |error: String, refused: bool| json!({"id": id, "ok": false, "error": error, "refused": refused});
    if let Some(r) = bridge_command(&req.cmd, &req.args) {
        return match r {
            Ok(value) => json!({"id": id, "ok": true, "value": value}),
            Err(error) => reply_err(error, true),
        };
    }
    if let Admission::Refused(why) = admit(&req, allow_transmit) {
        return reply_err(why, true);
    }
    if req.cmd == "set_settings" {
        let Some(proposed) = req.args.get("settings") else {
            return reply_err("set_settings: no settings in the call".into(), true);
        };
        match run(runner, "get_settings", json!({})) {
            Ok(current) => {
                let changed = protected_changes(&current, proposed);
                if !changed.is_empty() {
                    return reply_err(
                        format!("set_settings: {} guard the transmitter and are set at the station's own screen", changed.join(", ")),
                        true,
                    );
                }
            }
            Err(e) => {
                return reply_err(
                    format!("set_settings: could not read the settings in force: {e}"),
                    true,
                )
            }
        }
    }
    match run(runner, &req.cmd, req.args) {
        Ok(value) => json!({"id": id, "ok": true, "value": value}),
        Err(error) => reply_err(error, false),
    }
}

// ---------------------------------------------------------------------------------------------
// The listener and its manager.

struct Running {
    config: Config,
    stop: Arc<AtomicBool>,
}

/// Start the manager thread. `dir` is this instance's config folder (`config_dir()`); `log` writes
/// the connection log.
pub(crate) fn spawn(
    runner: Runner,
    dir: PathBuf,
    log: fn(&'static str, &'static str, String),
    emit: Option<Emit>,
) {
    {
        let mut s = shared();
        s.emit = emit;
        s.dir = Some(dir.clone());
    }
    let _ = std::thread::Builder::new()
        .name("automation-bridge".into())
        .spawn(move || manage(runner, dir, log));
}

fn manage(runner: Runner, dir: PathBuf, log: fn(&'static str, &'static str, String)) {
    let mut running: Option<Running> = None;
    let mut last_bind_error: Option<(u16, Instant)> = None;
    loop {
        let wanted = Config::read(&dir);
        let same = matches!((&running, &wanted), (Some(r), Some(w)) if &r.config == w);
        if !same {
            if let Some(r) = running.take() {
                r.stop.store(true, Ordering::SeqCst);
                shared().listening = None;
                log(LANE, "info", "stopped".into());
                emit_state();
            }
            if let Some(config) = wanted {
                let retry_ok = last_bind_error
                    .map(|(port, at)| port != config.port || at.elapsed() > Duration::from_secs(30))
                    .unwrap_or(true);
                if retry_ok {
                    match listen(runner.clone(), config.clone(), log) {
                        Ok(stop) => {
                            log(
                                LANE,
                                "info",
                                format!(
                                    "listening on 127.0.0.1:{} (transmit {})",
                                    config.port,
                                    if config.allow_transmit {
                                        "allowed with confirmation"
                                    } else {
                                        "off"
                                    }
                                ),
                            );
                            shared().listening = Some(config.clone());
                            running = Some(Running { config, stop });
                            last_bind_error = None;
                            emit_state();
                        }
                        Err(e) => {
                            log(
                                LANE,
                                "error",
                                format!("port {} is taken or refused: {e}", config.port),
                            );
                            last_bind_error = Some((config.port, Instant::now()));
                        }
                    }
                }
            }
        }
        std::thread::sleep(POLL);
    }
}

fn listen(
    runner: Runner,
    config: Config,
    log: fn(&'static str, &'static str, String),
) -> std::io::Result<Arc<AtomicBool>> {
    let listener = TcpListener::bind(SocketAddr::from((Ipv4Addr::LOCALHOST, config.port)))?;
    listener.set_nonblocking(true)?;
    let stop = Arc::new(AtomicBool::new(false));
    let stopped = stop.clone();
    let open = Arc::new(AtomicUsize::new(0));
    std::thread::Builder::new()
        .name("automation-bridge-listen".into())
        .spawn(move || {
            while !stopped.load(Ordering::SeqCst) {
                match listener.accept() {
                    Ok((stream, peer)) => {
                        if !peer.ip().is_loopback()
                            || open.load(Ordering::SeqCst) >= MAX_CONNECTIONS
                        {
                            let _ = stream.shutdown(Shutdown::Both);
                            continue;
                        }
                        open.fetch_add(1, Ordering::SeqCst);
                        let (runner, config, stopped, open) = (
                            runner.clone(),
                            config.clone(),
                            stopped.clone(),
                            open.clone(),
                        );
                        let _ = std::thread::Builder::new()
                            .name("automation-bridge-conn".into())
                            .spawn(move || {
                                serve(stream, runner, config, stopped, log);
                                open.fetch_sub(1, Ordering::SeqCst);
                            });
                    }
                    Err(e) if e.kind() == ErrorKind::WouldBlock => {
                        std::thread::sleep(Duration::from_millis(100))
                    }
                    Err(_) => std::thread::sleep(Duration::from_millis(250)),
                }
            }
        })?;
    Ok(stop)
}

fn write_line(out: &Mutex<TcpStream>, v: &Value) -> bool {
    let mut line = serde_json::to_vec(v).unwrap_or_default();
    line.push(b'\n');
    let mut s = out.lock().unwrap_or_else(|p| p.into_inner());
    s.write_all(&line).is_ok()
}

fn serve(
    stream: TcpStream,
    runner: Runner,
    config: Config,
    stop: Arc<AtomicBool>,
    log: fn(&'static str, &'static str, String),
) {
    let _ = stream.set_nonblocking(false);
    let _ = stream.set_read_timeout(Some(Duration::from_millis(500)));
    let _ = stream.set_nodelay(true);
    let Ok(writer) = stream.try_clone() else {
        return;
    };
    let out = Arc::new(Mutex::new(writer));
    let mut reader = stream;
    let mut buf: Vec<u8> = Vec::new();
    let mut chunk = [0u8; 16 * 1024];
    let mut hello = false;
    let opened = Instant::now();
    let in_flight = Arc::new(AtomicUsize::new(0));
    let mut window = (Instant::now(), 0u32);
    let mut counts: HashMap<&'static str, u64> = HashMap::new();
    'conn: while !stop.load(Ordering::SeqCst) {
        if !hello && opened.elapsed() > HELLO_WAIT {
            break;
        }
        match reader.read(&mut chunk) {
            Ok(0) => break,
            Ok(n) => buf.extend_from_slice(&chunk[..n]),
            Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => continue,
            Err(_) => break,
        }
        if buf.len() > MAX_LINE && !buf.contains(&b'\n') {
            write_line(
                &out,
                &json!({"ok": false, "error": "requestTooLong", "refused": true}),
            );
            break;
        }
        while let Some(pos) = buf.iter().position(|&b| b == b'\n') {
            let line: Vec<u8> = buf.drain(..=pos).collect();
            let line = &line[..line.len() - 1];
            if line.iter().all(u8::is_ascii_whitespace) {
                continue;
            }
            if !hello {
                let given = serde_json::from_slice::<Value>(line)
                    .ok()
                    .and_then(|v| v.get("hello").and_then(Value::as_str).map(str::to_string))
                    .unwrap_or_default();
                if token_matches(&given, &config.token) {
                    hello = true;
                    shared().connections += 1;
                    log(LANE, "info", "a program connected".into());
                    emit_state();
                    write_line(
                        &out,
                        &json!({"hello": "nexus-automation", "protocol": PROTOCOL, "allowTransmit": config.allow_transmit}),
                    );
                    continue;
                }
                write_line(
                    &out,
                    &json!({"ok": false, "error": "badToken", "refused": true}),
                );
                log(
                    LANE,
                    "warn",
                    "refused a program with the wrong token".into(),
                );
                break 'conn;
            }
            // Rate and concurrency.
            if window.0.elapsed() >= Duration::from_secs(1) {
                window = (Instant::now(), 0);
            }
            window.1 += 1;
            let req = match Request::parse(line) {
                Ok(r) => r,
                Err(e) => {
                    write_line(&out, &json!({"ok": false, "error": e, "refused": true}));
                    continue;
                }
            };
            if window.1 > PER_SECOND || in_flight.load(Ordering::SeqCst) >= IN_FLIGHT {
                write_line(
                    &out,
                    &json!({"id": req.id, "ok": false, "error": "busy", "refused": true}),
                );
                continue;
            }
            *counts
                .entry(match class_of(&req.cmd) {
                    Some(Class::StartsTx) => "tx",
                    Some(Class::StopsTx) => "stop",
                    _ => "other",
                })
                .or_default() += 1;
            in_flight.fetch_add(1, Ordering::SeqCst);
            let (runner, out, in_flight, allow) = (
                runner.clone(),
                out.clone(),
                in_flight.clone(),
                config.allow_transmit,
            );
            let _ = std::thread::Builder::new()
                .name("automation-bridge-call".into())
                .spawn(move || {
                    let cmd = req.cmd.clone();
                    let reply = answer(&runner, req, allow);
                    if matches!(class_of(&cmd), Some(Class::StartsTx)) {
                        log(
                            LANE,
                            "warn",
                            format!(
                                "{cmd}: {}",
                                if reply["ok"] == true {
                                    "ran (transmit start)"
                                } else {
                                    "refused or failed"
                                }
                            ),
                        );
                    }
                    write_line(&out, &reply);
                    in_flight.fetch_sub(1, Ordering::SeqCst);
                });
        }
    }
    let _ = out.lock().map(|s| s.shutdown(Shutdown::Both));
    if hello {
        {
            let mut s = shared();
            s.connections = s.connections.saturating_sub(1);
        }
        emit_state();
        log(
            LANE,
            "info",
            format!(
                "a program disconnected ({} calls, {} transmit starts, {} stops)",
                counts.values().sum::<u64>(),
                counts.get("tx").unwrap_or(&0),
                counts.get("stop").unwrap_or(&0)
            ),
        );
    }
}

#[cfg(test)]
#[path = "automation_bridge_tests.rs"]
mod tests;
