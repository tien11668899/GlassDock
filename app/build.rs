//! Builds GlassDock's resources (its icons, version information and manifest, from app.rc) with
//! the Windows SDK's resource compiler and links them into the exe.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let version = env::var("CARGO_PKG_VERSION").unwrap();
    let mut numbers: Vec<String> = version
        .split(|c| c == '.' || c == '-')
        .take(3)
        .map(|part| part.parse::<u16>().unwrap_or(0).to_string())
        .collect();
    numbers.resize(4, "0".into());
    let script = fs::read_to_string(dir.join("app.rc"))
        .unwrap()
        .replace("@DIR@", &dir.display().to_string().replace('\\', "\\\\"))
        .replace("@VERSION@", &version)
        .replace("@NUMBERS@", &numbers.join(","));
    let rc = out.join("app.rc");
    fs::write(&rc, script).unwrap();
    let res = out.join("app.res");
    let status = Command::new(resource_compiler())
        .args(["/nologo", "/fo"])
        .arg(&res)
        .arg(&rc)
        .status()
        .expect("rc.exe (from the Windows SDK) was not found");
    assert!(status.success(), "rc.exe failed on app.rc");
    println!("cargo:rustc-link-arg-bins={}", res.display());
    for file in ["app.rc", "app.manifest", "icons/app.ico", "icons/tray.ico"] {
        println!("cargo:rerun-if-changed={file}");
    }
}

/// rc.exe of the newest Windows SDK that has one, else whichever is on PATH.
fn resource_compiler() -> PathBuf {
    let base = env::var("ProgramFiles(x86)").unwrap_or_else(|_| r"C:\Program Files (x86)".into());
    let bin = Path::new(&base).join(r"Windows Kits\10\bin");
    let mut found: Vec<PathBuf> = fs::read_dir(bin)
        .into_iter()
        .flatten()
        .flatten()
        .map(|entry| entry.path().join(r"x64\rc.exe"))
        .filter(|path| path.exists())
        .collect();
    found.sort();
    found.pop().unwrap_or_else(|| PathBuf::from("rc.exe"))
}
