// Rust tokio equivalent of spawn_1m. Spawn N async tasks each
// completing immediately; main awaits a barrier. Measures the
// spawn + dispatch + complete pipeline of the tokio scheduler.

use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::Arc;
use std::time::Instant;
use tokio::sync::Notify;

#[tokio::main(flavor = "multi_thread")]
async fn main() {
    const N: i64 = 1_000_000;
    let counter = Arc::new(AtomicI64::new(N));
    let done = Arc::new(Notify::new());

    let t0 = Instant::now();
    for _ in 0..N {
        let counter = counter.clone();
        let done = done.clone();
        tokio::spawn(async move {
            if counter.fetch_sub(1, Ordering::AcqRel) == 1 {
                done.notify_one();
            }
        });
    }
    done.notified().await;
    let elapsed = t0.elapsed().as_millis();
    println!("spawn+drain {}: {} ms", N, elapsed);
}
