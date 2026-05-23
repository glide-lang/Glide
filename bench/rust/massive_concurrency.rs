// Rust tokio equivalent of massive_concurrency. Spawn N async tasks
// each blocked on a oneshot::Receiver that will never receive; sleep
// 500ms then sample RSS. Reports bytes-per-task.

use std::env;
use std::time::{Duration, Instant};
use tokio::sync::watch;
use tokio::time::sleep;

#[cfg(target_os = "windows")]
#[link(name = "psapi")]
extern "system" {
    fn GetProcessMemoryInfo(handle: *mut u8, info: *mut u8, size: u32) -> i32;
}
#[cfg(target_os = "windows")]
#[link(name = "kernel32")]
extern "system" {
    fn GetCurrentProcess() -> *mut u8;
}
#[cfg(target_os = "windows")]
fn rss_bytes() -> i64 {
    use std::mem::size_of;
    #[repr(C)]
    struct ProcMemCounters {
        cb: u32,
        page_fault_count: u32,
        peak_working_set_size: usize,
        working_set_size: usize,
        quota_peak_paged_pool_usage: usize,
        quota_paged_pool_usage: usize,
        quota_peak_non_paged_pool_usage: usize,
        quota_non_paged_pool_usage: usize,
        pagefile_usage: usize,
        peak_pagefile_usage: usize,
    }
    unsafe {
        let mut pmc: ProcMemCounters = std::mem::zeroed();
        pmc.cb = size_of::<ProcMemCounters>() as u32;
        if GetProcessMemoryInfo(GetCurrentProcess(), &mut pmc as *mut _ as *mut u8, pmc.cb) == 0 {
            return -1;
        }
        pmc.working_set_size as i64
    }
}

#[cfg(target_os = "linux")]
fn rss_bytes() -> i64 {
    use std::fs;
    let s = match fs::read_to_string("/proc/self/statm") {
        Ok(s) => s,
        Err(_) => return -1,
    };
    let mut it = s.split_whitespace();
    it.next(); // size
    let res: i64 = it.next().and_then(|v| v.parse().ok()).unwrap_or(-1);
    if res < 0 {
        return -1;
    }
    let page = 4096_i64;
    res * page
}

#[cfg(target_os = "macos")]
fn rss_bytes() -> i64 {
    // Best-effort: parse `ps -o rss=` for current pid.
    use std::process::Command;
    let pid = std::process::id();
    let out = Command::new("ps")
        .arg("-o")
        .arg("rss=")
        .arg("-p")
        .arg(pid.to_string())
        .output();
    match out {
        Ok(o) => {
            let s = String::from_utf8_lossy(&o.stdout);
            let kb: i64 = s.trim().parse().unwrap_or(-1);
            if kb < 0 { -1 } else { kb * 1024 }
        }
        Err(_) => -1,
    }
}

async fn parker(_rx: watch::Receiver<i32>) {
    loop {
        sleep(Duration::from_secs(3600)).await;
    }
}

#[tokio::main(flavor = "multi_thread")]
async fn main() {
    let n: usize = env::var("GLIDE_MASSIVE_N")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(100_000);

    let baseline = rss_bytes();
    let t0 = Instant::now();

    let (_tx, rx) = watch::channel::<i32>(0);
    for _ in 0..n {
        let r = rx.clone();
        tokio::spawn(parker(r));
    }
    sleep(Duration::from_millis(500)).await;

    let after = rss_bytes();
    let elapsed = t0.elapsed().as_millis();
    let delta = after - baseline;
    let per_task = delta / (n as i64);
    println!(
        "massive {} tasks elapsed_ms: {} rss_baseline_mb: {} \
         rss_after_mb: {} delta_mb: {} bytes_per_task: {}",
        n,
        elapsed,
        baseline / (1024 * 1024),
        after / (1024 * 1024),
        delta / (1024 * 1024),
        per_task
    );
}
