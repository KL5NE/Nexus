//! The automation bridge's rules, each as a test, and one end-to-end run over a real socket with a
//! stand-in for the app's handler.
use super::*;
use std::io::{BufRead, BufReader};

const TOKEN: &str = "0123456789abcdef0123456789abcdef0123456789abcdef";

fn req(cmd: &str) -> Request {
    Request::parse(format!(r#"{{"id":1,"cmd":"{cmd}"}}"#).as_bytes()).unwrap()
}

fn with(cmd: &str, extra: &str) -> Request {
    Request::parse(format!(r#"{{"id":1,"cmd":"{cmd}",{extra}}}"#).as_bytes()).unwrap()
}

#[test]
fn the_file_turns_it_on_only_with_a_long_token() {
    let on = format!(r#"{{"enabled":true,"token":"{TOKEN}"}}"#);
    let c = Config::parse(&on).expect("on");
    assert_eq!(
        (c.port, c.allow_transmit),
        (DEFAULT_PORT, false),
        "transmit is off unless named"
    );
    assert!(Config::parse(&format!(r#"{{"enabled":false,"token":"{TOKEN}"}}"#)).is_none());
    assert!(Config::parse(r#"{"enabled":true,"token":"short"}"#).is_none());
    assert!(Config::parse("not json").is_none());
    let c = Config::parse(&format!(
        r#"{{"enabled":true,"token":"{TOKEN}","port":80}}"#
    ))
    .unwrap();
    assert_eq!(c.port, DEFAULT_PORT, "a privileged port is not taken");
}

#[test]
fn station_reads_and_controls_run() {
    for cmd in [
        "get_snapshot",
        "cw_decode",
        "cw_skim",
        "get_ota_spots",
        "work_spot",
        "set_operating_mode",
        "set_cw_wpm",
    ] {
        assert_eq!(admit(&req(cmd), false), Admission::Run, "{cmd}");
    }
}

#[test]
fn stop_is_never_refused() {
    for cmd in ["halt_tx", "stop_cw", "psk_stop", "jtty_stop"] {
        assert_eq!(admit(&req(cmd), false), Admission::Run, "{cmd}");
    }
}

#[test]
fn a_transmit_start_needs_the_file_and_the_call_to_say_so() {
    assert!(
        matches!(admit(&req("send_cw"), false), Admission::Refused(_)),
        "off in the file"
    );
    assert!(
        matches!(admit(&req("send_cw"), true), Admission::Refused(_)),
        "no confirmTransmit"
    );
    assert_eq!(
        admit(&with("send_cw", r#""confirmTransmit":true"#), true),
        Admission::Run
    );
    assert!(matches!(
        admit(&with("send_cw", r#""confirmTransmit":true"#), false),
        Admission::Refused(_)
    ));
    assert!(
        matches!(
            admit(&with("call_cq", r#""confirmTransmit":"yes""#), true),
            Admission::Refused(_)
        ),
        "only true is true"
    );
}

#[test]
fn credentials_admin_and_this_computers_files_are_refused() {
    assert!(matches!(
        admit(&req("answer_saved_key"), true),
        Admission::Refused(_)
    ));
    assert!(matches!(
        admit(&req("check_for_update"), true),
        Admission::Refused(_)
    ));
    assert_eq!(admit(&req("app_version"), false), Admission::Run);
    assert!(matches!(
        admit(&req("no_such_command_at_all"), true),
        Admission::Refused(_)
    ));
}

#[test]
fn data_changes_need_confirm_change() {
    for cmd in [
        "delete_qso",
        "log_qso",
        "set_settings",
        "post_spot",
        "import_adif",
    ] {
        assert!(
            matches!(admit(&req(cmd), true), Admission::Refused(_)),
            "{cmd}"
        );
        assert_eq!(
            admit(&with(cmd, r#""confirmChange":true"#), false),
            Admission::Run,
            "{cmd}"
        );
    }
}

#[test]
fn every_named_command_is_one_nexus_has() {
    for cmd in NEEDS_CONFIRM.iter().chain(LOCAL_OK) {
        assert!(
            class_of(cmd).is_some(),
            "{cmd} is not a command of this build"
        );
    }
    for cmd in NEEDS_CONFIRM {
        assert_eq!(class_of(cmd), Some(Class::RunsAtShack), "{cmd}");
    }
}

#[test]
fn afar_is_stripped_and_args_must_be_an_object() {
    let r = with("get_snapshot", r#""args":{"afar":{"x":1},"keep":2}"#);
    assert_eq!(r.args, json!({"keep": 2}));
    assert!(Request::parse(br#"{"id":1,"cmd":"x","args":[1]}"#).is_err());
    assert!(Request::parse(br#"{"id":1}"#).is_err());
}

#[test]
fn protected_settings_are_caught_and_partial_saves_pass() {
    let current = json!({"licenseClass": "general", "tciServerTx": false, "mycall": "KL5NE"});
    assert!(
        protected_changes(&current, &json!({"licenseClass": "general", "mycall": "X"})).is_empty()
    );
    assert_eq!(
        protected_changes(&current, &json!({"licenseClass": "open"})),
        vec!["licenseClass"]
    );
    assert_eq!(
        protected_changes(&current, &json!({"tciServerTx": true})),
        vec!["tciServerTx"]
    );
    assert!(protected_changes(&current, &json!({"mycall": "Y"})).is_empty());
}

#[test]
fn token_compare() {
    assert!(token_matches(TOKEN, TOKEN));
    assert!(!token_matches("", TOKEN));
    assert!(!token_matches(&TOKEN[1..], TOKEN));
}

/// A stand-in for the app's handler: echoes the command and its args, holds settings.
fn mock_runner(calls: Arc<Mutex<Vec<String>>>) -> Runner {
    Arc::new(
        move |cmd: String, args: Value, answer: crate::command_dispatch::Answer| {
            calls.lock().unwrap().push(cmd.clone());
            match cmd.as_str() {
                "get_settings" => {
                    answer(Ok(json!({"licenseClass": "general", "tciServerTx": false})))
                }
                "explode" => answer(Err("boom".into())),
                _ => answer(Ok(json!({"ran": cmd, "args": args}))),
            }
        },
    )
}

#[test]
fn answers_through_the_handler() {
    let calls = Arc::new(Mutex::new(vec![]));
    let r = mock_runner(calls.clone());
    let v = answer(
        &r,
        with("cw_decode", r#""args":{"sensitivity":0.5}"#),
        false,
    );
    assert_eq!(v["ok"], true);
    assert_eq!(v["value"]["args"]["sensitivity"], 0.5);
    let v = answer(
        &r,
        with(
            "set_settings",
            r#""confirmChange":true,"args":{"settings":{"licenseClass":"open"}}"#,
        ),
        false,
    );
    assert_eq!(
        (v["ok"].clone(), v["refused"].clone()),
        (json!(false), json!(true))
    );
    let v = answer(
        &r,
        with(
            "set_settings",
            r#""confirmChange":true,"args":{"settings":{"licenseClass":"general","mycall":"KL5NE"}}"#,
        ),
        false,
    );
    assert_eq!(v["ok"], true);
    let v = answer(&r, req("send_cw"), true);
    assert_eq!(v["refused"], true);
    assert!(
        !calls.lock().unwrap().contains(&"send_cw".to_string()),
        "a refused call never reaches the handler"
    );
    let v = answer(&r, req("bridge.commands"), false);
    assert!(v["value"].as_array().unwrap().len() > 400);
}

fn noop_log(_: &'static str, _: &'static str, _: String) {}

#[test]
fn end_to_end_over_loopback() {
    let calls = Arc::new(Mutex::new(vec![]));
    let port = {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        l.local_addr().unwrap().port()
    };
    let config = Config {
        enabled: true,
        port,
        token: TOKEN.into(),
        allow_transmit: false,
    };
    let stop = listen(mock_runner(calls.clone()), config, noop_log).unwrap();
    let s = TcpStream::connect(("127.0.0.1", port)).unwrap();
    let mut rd = BufReader::new(s.try_clone().unwrap());
    let mut w = s;
    let mut line = String::new();
    // wrong token first, on its own connection
    {
        let bad = TcpStream::connect(("127.0.0.1", port)).unwrap();
        let mut brd = BufReader::new(bad.try_clone().unwrap());
        let mut bw = bad;
        bw.write_all(b"{\"hello\":\"nope\"}\n").unwrap();
        let mut l = String::new();
        brd.read_line(&mut l).unwrap();
        assert!(l.contains("badToken"));
    }
    w.write_all(format!("{{\"hello\":\"{TOKEN}\"}}\n").as_bytes())
        .unwrap();
    rd.read_line(&mut line).unwrap();
    assert!(line.contains("nexus-automation"), "{line}");
    w.write_all(b"{\"id\":5,\"cmd\":\"get_snapshot\"}\n{\"id\":6,\"cmd\":\"send_cw\",\"confirmTransmit\":true}\n{\"id\":7,\"cmd\":\"halt_tx\"}\n").unwrap();
    let mut got = HashMap::new();
    for _ in 0..3 {
        line.clear();
        rd.read_line(&mut line).unwrap();
        let v: Value = serde_json::from_str(&line).unwrap();
        got.insert(v["id"].as_i64().unwrap(), v);
    }
    assert_eq!(got[&5]["ok"], true);
    assert_eq!(got[&6]["refused"], true, "transmit is off in this config");
    assert_eq!(got[&7]["ok"], true, "stop always runs");
    stop.store(true, Ordering::SeqCst);
}

#[test]
fn the_screens_are_the_apps_screens() {
    let src = include_str!("../../ui/src/features/registry.ts");
    let union = src
        .split_once("export type View =")
        .and_then(|(_, rest)| rest.split_once("\n\n"))
        .map(|(u, _)| u)
        .expect("registry.ts has `export type View =`");
    let named: Vec<&str> = union
        .split('|')
        .map(|v| v.trim().trim_matches('\'').trim())
        .filter(|v| !v.is_empty())
        .collect();
    let mut a: Vec<&str> = VIEWS.to_vec();
    let mut b = named.clone();
    a.sort();
    b.sort();
    assert_eq!(
        a, b,
        "VIEWS must list exactly the View union in registry.ts"
    );
}

#[test]
fn navigate_checks_the_screen_and_tells_the_window() {
    let seen: Arc<Mutex<Vec<(String, Value)>>> = Arc::new(Mutex::new(vec![]));
    let sink = seen.clone();
    shared().emit = Some(Arc::new(move |name: &'static str, v: Value| {
        sink.lock().unwrap().push((name.to_string(), v))
    }));
    assert!(
        bridge_command("bridge.navigate", &json!({"view": "nowhere"}))
            .unwrap()
            .is_err()
    );
    assert!(bridge_command("bridge.navigate", &json!({}))
        .unwrap()
        .is_err());
    let r = bridge_command("bridge.navigate", &json!({"view": "logbook"}))
        .unwrap()
        .unwrap();
    assert_eq!(r["asked"], "logbook");
    let r = bridge_command("bridge.navigate", &json!({"settings": "Radio ▸ Audio"}))
        .unwrap()
        .unwrap();
    assert_eq!(r["asked"], "settings");
    let got = seen.lock().unwrap().clone();
    assert!(got
        .iter()
        .any(|(n, v)| n == NAVIGATE_EVENT && v["view"] == "logbook"));
    assert!(got
        .iter()
        .any(|(n, v)| n == NAVIGATE_EVENT && v["settings"] == "Radio ▸ Audio"));
    assert!(
        bridge_command("get_snapshot", &json!({})).is_none(),
        "not a bridge command"
    );
}

#[test]
fn the_window_says_which_screen() {
    ui_event("automation-view", r#"{"view":"awards"}"#);
    assert_eq!(
        bridge_command("bridge.view", &json!({})).unwrap().unwrap()["view"],
        "awards"
    );
    ui_event("automation-view", r#"{"view":"not-a-screen"}"#);
    assert_eq!(
        bridge_command("bridge.view", &json!({})).unwrap().unwrap()["view"],
        "awards",
        "unknown names are ignored"
    );
}

#[test]
fn alerts_are_kept_and_read_since() {
    let before = bridge_command("bridge.alerts", &json!({}))
        .unwrap()
        .unwrap()["latestId"]
        .as_u64()
        .unwrap();
    alert("alert", r#"{"call":"VP2MCV","why":"new DXCC"}"#);
    alert("alert", "not json");
    let r = bridge_command("bridge.alerts", &json!({"since": before}))
        .unwrap()
        .unwrap();
    let list = r["alerts"].as_array().unwrap();
    assert!(list.len() >= 2);
    assert!(list.iter().any(|a| a["data"]["call"] == "VP2MCV"));
    assert!(list.iter().any(|a| a["data"] == "not json"));
    for _ in 0..(ALERTS_KEPT + 10) {
        alert("alert", "{}");
    }
    let all = bridge_command("bridge.alerts", &json!({"limit": 1000}))
        .unwrap()
        .unwrap();
    assert!(all["alerts"].as_array().unwrap().len() <= ALERTS_KEPT);
}

#[test]
fn the_switch_writes_the_file_and_keeps_the_token() {
    let dir = std::env::temp_dir().join(format!("nx-bridge-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    write_config(&dir, true, false).unwrap();
    let a = Config::parse(&std::fs::read_to_string(dir.join(CONFIG_FILE)).unwrap()).expect("on");
    assert_eq!(
        (a.port, a.allow_transmit, a.token.len()),
        (DEFAULT_PORT, false, 64)
    );
    write_config(&dir, true, true).unwrap();
    let b = Config::parse(&std::fs::read_to_string(dir.join(CONFIG_FILE)).unwrap()).unwrap();
    assert_eq!(
        (b.token.clone(), b.allow_transmit),
        (a.token.clone(), true),
        "same token, transmit on"
    );
    write_config(&dir, false, true).unwrap();
    let t = std::fs::read_to_string(dir.join(CONFIG_FILE)).unwrap();
    assert!(Config::parse(&t).is_none(), "off");
    assert!(
        t.contains("\"allowTransmit\": false"),
        "off never leaves transmit allowed"
    );
    let _ = std::fs::remove_dir_all(&dir);
}
