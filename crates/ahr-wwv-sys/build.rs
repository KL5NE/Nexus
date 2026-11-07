//! Build the vendored AHR-Decoder (C++17 core + plain C API) with CMake and emit
//! the link directives Cargo needs.
//!
//! The decoder is portable C++17 with no third-party dependencies, so this is
//! simpler than `tempo-fast-sys`: no Fortran, no FFTW. What it does share with
//! that crate is the C++ runtime question, answered the same way:
//!
//! - Linux: dynamic `stdc++` (the system's).
//! - macOS: the OS's `c++` (AppleClang compiles the core there).
//! - Windows GNU cross-built from Linux (the release path): STATIC `stdc++`, so
//!   the binary needs no `libstdc++-6.dll`, with CMake pointed at the repo's
//!   MinGW toolchain file — the same rule as libtempo's cross build.
//! - Native Windows GNU (MSYS2): dynamic `stdc++`, as libtempo's native path.
//!
//! Nothing here runs at Nexus run time; the decoder never touches a clock (see
//! `vendor/ahr-decoder/capi/include/ahr_wwv.h`).

use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let src = manifest.join("vendor/ahr-decoder");
    // Windows verbatim paths (\\?\D:\...) confuse CMake/make; see tempo-fast-sys.
    let src = strip_verbatim(&src);
    let out = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR")).join("ahr-build");
    let test_synth = env::var_os("CARGO_FEATURE_TEST_SYNTH").is_some();
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    let target_env = env::var("CARGO_CFG_TARGET_ENV").unwrap_or_default();
    let cross_windows_gnu = !cfg!(windows) && target_os == "windows" && target_env == "gnu";

    let mut cfg = Command::new("cmake");
    cfg.arg("-S")
        .arg(&src)
        .arg("-B")
        .arg(&out)
        .arg("-DCMAKE_BUILD_TYPE=Release")
        .arg(format!(
            "-DAHRWWV_CAPI_TEST_SYNTH={}",
            if test_synth { "ON" } else { "OFF" }
        ));
    if cross_windows_gnu {
        let toolchain = manifest.join("../../libtempo/mingw-w64.cmake");
        assert!(
            toolchain.exists(),
            "missing MinGW toolchain file: {}",
            toolchain.display()
        );
        cfg.arg(format!("-DCMAKE_TOOLCHAIN_FILE={}", toolchain.display()));
    }
    if let Ok(generator) = env::var("CMAKE_GENERATOR") {
        cfg.args(["-G", &generator]);
    } else if cfg!(windows) {
        cfg.args([
            "-G",
            if which("ninja") {
                "Ninja"
            } else {
                "MinGW Makefiles"
            },
        ]);
    }
    println!("cargo:rerun-if-env-changed=CMAKE_GENERATOR");
    run(&mut cfg);

    let mut build = Command::new("cmake");
    build
        .arg("--build")
        .arg(&out)
        .args(["--parallel", "--target", "ahrwwv_capi"]);
    run(&mut build);
    if test_synth {
        let mut b = Command::new("cmake");
        b.arg("--build")
            .arg(&out)
            .args(["--parallel", "--target", "ahrwwv_testsynth"]);
        run(&mut b);
    }

    // Single-config generators put archives in the build dir; multi-config in Release/.
    println!("cargo:rustc-link-search=native={}", out.display());
    println!(
        "cargo:rustc-link-search=native={}",
        out.join("Release").display()
    );
    if test_synth {
        println!("cargo:rustc-link-lib=static=ahrwwv_testsynth");
    }
    println!("cargo:rustc-link-lib=static=ahrwwv_capi");
    println!("cargo:rustc-link-lib=static=ahrwwv_core");
    // The C++ runtime, chosen exactly as tempo-fast-sys does for libtempo.
    if target_os == "macos" {
        println!("cargo:rustc-link-lib=dylib=c++");
    } else if cross_windows_gnu {
        if let Some(dir) = mingw_gcc_lib_dir() {
            println!("cargo:rustc-link-search=native={dir}");
        }
        println!("cargo:rustc-link-lib=static=stdc++");
    } else if target_env != "msvc" {
        println!("cargo:rustc-link-lib=dylib=stdc++");
    }
    println!("cargo:rerun-if-changed={}", src.display());
}

fn strip_verbatim(p: &Path) -> PathBuf {
    let s = p.to_string_lossy();
    PathBuf::from(s.strip_prefix(r"\\?\").unwrap_or(&s))
}

/// The MinGW gcc library directory holding `libstdc++.a`, for the cross build.
fn mingw_gcc_lib_dir() -> Option<String> {
    let out = Command::new("x86_64-w64-mingw32-gcc")
        .arg("-print-file-name=libstdc++.a")
        .output()
        .ok()?;
    let path = PathBuf::from(String::from_utf8(out.stdout).ok()?.trim());
    path.is_absolute()
        .then(|| path.parent().map(|d| d.display().to_string()))
        .flatten()
}

fn which(tool: &str) -> bool {
    Command::new(tool)
        .arg("--version")
        .output()
        .is_ok_and(|o| o.status.success())
}

fn run(cmd: &mut Command) {
    let status = cmd
        .status()
        .unwrap_or_else(|e| panic!("failed to run {cmd:?}: {e}"));
    assert!(status.success(), "{cmd:?} failed with {status}");
}
