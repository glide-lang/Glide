// Rust tokio equivalent of park_unpark_coro_clean. Two spawned tasks
// ping-pong an i32 via tokio::sync::mpsc with capacity 1; main awaits
// a one-shot done chan with elapsed-time reading. Measures pure
// task-to-task park/unpark via tokio mpsc.

use std::time::Instant;
use tokio::sync::mpsc;
use tokio::sync::oneshot;

const N: i32 = 100_000;
const SENTINEL: i32 = -1;

async fn pinger(
    mut rx: mpsc::Receiver<i32>,
    tx: mpsc::Sender<i32>,
    done: oneshot::Sender<i64>,
) {
    // Warm-up.
    for _ in 0..100 {
        tx.send(0).await.unwrap();
        let _ = rx.recv().await.unwrap();
    }
    let t0 = Instant::now();
    for i in 0..N {
        tx.send(i).await.unwrap();
        let _ = rx.recv().await.unwrap();
    }
    let elapsed = t0.elapsed().as_nanos() as i64;
    tx.send(SENTINEL).await.unwrap();
    done.send(elapsed).unwrap();
}

async fn ponger(mut rx: mpsc::Receiver<i32>, tx: mpsc::Sender<i32>) {
    while let Some(v) = rx.recv().await {
        if v == SENTINEL {
            return;
        }
        tx.send(v + 1).await.unwrap();
    }
}

#[tokio::main(flavor = "multi_thread")]
async fn main() {
    let (to_pong_tx, to_pong_rx) = mpsc::channel::<i32>(1);
    let (from_pong_tx, from_pong_rx) = mpsc::channel::<i32>(1);
    let (done_tx, done_rx) = oneshot::channel::<i64>();

    tokio::spawn(pinger(from_pong_rx, to_pong_tx, done_tx));
    tokio::spawn(ponger(to_pong_rx, from_pong_tx));

    let elapsed = done_rx.await.unwrap();
    let per_cycle = elapsed / (N as i64);
    println!(
        "coro-coro park-unpark {} cycles total_ns: {} avg_ns/cycle: {}",
        N, elapsed, per_cycle
    );
}
