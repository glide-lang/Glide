// HTTP hello server — Axum on Tokio (multi-thread, 4 worker threads).
// Apples-to-apples comparison with bench/http_hello_multi.glide.
// Build: cargo build --release --bin axum_hello

use axum::{routing::get, Router};

async fn hello() -> &'static str {
    "hello\n"
}

fn main() {
    let workers: usize = std::env::var("HTTP_WORKERS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(4);
    let port: u16 = std::env::var("HTTP_PORT")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(8080);

    let rt = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(workers)
        .enable_all()
        .build()
        .unwrap();

    rt.block_on(async move {
        let app = Router::new().route("/", get(hello));
        let listener = tokio::net::TcpListener::bind(("0.0.0.0", port))
            .await
            .expect("bind");
        eprintln!("axum: listening on :{} across {} worker(s)", port, workers);
        axum::serve(listener, app).await.unwrap();
    });
}
