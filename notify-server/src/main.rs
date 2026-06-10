//! notify-server — a tiny notification queue for the ESP32-S3 watch.
//!
//! Flow:
//!   iPhone Shortcut (or curl)  --POST /notify-->  [in-memory queue]  --GET /notify-->  watch
//!
//! The watch's GET *drains* the queue (returns all pending items and clears
//! them), so each notification is delivered once. Items also expire after a TTL
//! so nothing lingers if the watch never fetches.
//!
//! Security model (public-facing via an HTTPS front):
//!   * A shared bearer token (env NOTIFY_TOKEN) gates BOTH endpoints — without
//!     the right `Authorization: Bearer <token>` header, requests are rejected.
//!     The check is constant-time (see `authorized`) to avoid timing leaks.
//!   * This server speaks PLAIN HTTP and binds to 127.0.0.1 by default, so it is
//!     NOT directly reachable from other machines. TLS is terminated by a front:
//!       - Tailscale Funnel  (`tailscale funnel 8080`) — real Let's Encrypt cert
//!         for your <host>.<tailnet>.ts.net name, reachable from ANY network with
//!         no port-forwarding / static IP. This is the intended setup.
//!       - or a reverse proxy (Caddy/nginx) with its own Let's Encrypt cert, for
//!         anyone who CAN port-forward a domain.
//!     Either way the watch connects over HTTPS and the token + bodies are
//!     encrypted in transit. Binding to localhost means the ONLY way in is through
//!     that TLS front — a misconfigured firewall can't expose cleartext HTTP.
//!   * The VPS/host itself still sees plaintext (it terminates TLS); that's the
//!     normal trust boundary. End-to-end body encryption could be layered on later.
//!
//! Run (behind Tailscale Funnel, on the host machine):
//!   NOTIFY_TOKEN=your-secret cargo run --release      # binds 127.0.0.1:8080
//!   tailscale funnel 8080                             # exposes it over HTTPS
//! Override the bind with NOTIFY_ADDR (e.g. NOTIFY_ADDR=0.0.0.0:8080 only if you
//! deliberately want LAN access in front of your own reverse proxy).

use std::{
    collections::VecDeque,
    sync::{Arc, Mutex},
    time::{SystemTime, UNIX_EPOCH},
};

use axum::{
    extract::State,
    http::{HeaderMap, StatusCode},
    routing::{get, post},
    Json, Router,
};
use serde::{Deserialize, Serialize};

/// How long a queued notification lives before it's dropped (seconds).
const ITEM_TTL_SECS: u64 = 15 * 60;
/// Cap the queue so a flood can't grow memory without bound.
const MAX_ITEMS: usize = 100;

/// One notification as delivered to the watch.
#[derive(Clone, Serialize)]
struct Notification {
    /// Monotonic-ish id (epoch millis at receipt); handy for de-dup on the watch.
    id: u64,
    /// e.g. the app/source ("Messages", "Mail", ...). Optional.
    app: String,
    /// Title line.
    title: String,
    /// Body text.
    body: String,
    /// Unix seconds when received.
    ts: u64,
}

/// What a POST /notify accepts. `app`/`title` optional so a Shortcut can send
/// just a `body` if that's all it has.
#[derive(Deserialize)]
struct IncomingNotification {
    #[serde(default)]
    app: String,
    #[serde(default)]
    title: String,
    #[serde(default)]
    body: String,
}

#[derive(Clone)]
struct AppState {
    queue: Arc<Mutex<VecDeque<Notification>>>,
    token: Arc<String>,
}

fn now_secs() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

fn now_millis() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

/// Constant-ish bearer-token check. Returns true if the request is authorized.
fn authorized(headers: &HeaderMap, token: &str) -> bool {
    let Some(val) = headers.get(axum::http::header::AUTHORIZATION) else {
        return false;
    };
    let Ok(val) = val.to_str() else { return false };
    let expected = format!("Bearer {token}");
    // Length check first, then byte compare (avoids trivial early-out leak).
    val.len() == expected.len()
        && val
            .bytes()
            .zip(expected.bytes())
            .fold(0u8, |acc, (a, b)| acc | (a ^ b))
            == 0
}

/// Drop expired items from the front of the queue.
fn prune(queue: &mut VecDeque<Notification>) {
    let cutoff = now_secs().saturating_sub(ITEM_TTL_SECS);
    while let Some(front) = queue.front() {
        if front.ts < cutoff {
            queue.pop_front();
        } else {
            break;
        }
    }
}

/// POST /notify — phone submits a notification.
async fn post_notify(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(incoming): Json<IncomingNotification>,
) -> StatusCode {
    if !authorized(&headers, &state.token) {
        return StatusCode::UNAUTHORIZED;
    }
    let n = Notification {
        id: now_millis(),
        app: incoming.app,
        title: incoming.title,
        body: incoming.body,
        ts: now_secs(),
    };
    let mut q = state.queue.lock().unwrap();
    prune(&mut q);
    if q.len() >= MAX_ITEMS {
        q.pop_front(); // drop oldest to make room
    }
    q.push_back(n);
    StatusCode::CREATED
}

#[derive(Serialize)]
struct DrainResponse {
    items: Vec<Notification>,
}

/// GET /notify — watch fetches and drains all pending notifications.
async fn get_notify(
    State(state): State<AppState>,
    headers: HeaderMap,
) -> Result<Json<DrainResponse>, StatusCode> {
    if !authorized(&headers, &state.token) {
        return Err(StatusCode::UNAUTHORIZED);
    }
    let mut q = state.queue.lock().unwrap();
    prune(&mut q);
    let items: Vec<Notification> = q.drain(..).collect();
    Ok(Json(DrainResponse { items }))
}

/// GET /health — unauthenticated liveness check (no data exposed).
async fn health() -> &'static str {
    "ok"
}

#[tokio::main]
async fn main() {
    let token = std::env::var("NOTIFY_TOKEN").unwrap_or_else(|_| {
        eprintln!(
            "WARNING: NOTIFY_TOKEN not set — using insecure default 'changeme'. \
             Set NOTIFY_TOKEN to a strong secret."
        );
        "changeme".to_string()
    });
    // Bind to LOCALHOST by default: the only reachable path in is the HTTPS front
    // (Tailscale Funnel / reverse proxy). This prevents accidental cleartext-HTTP
    // exposure on the LAN or a public interface. Override with NOTIFY_ADDR only if
    // you intentionally front it with your own proxy that needs a LAN bind.
    let addr = std::env::var("NOTIFY_ADDR").unwrap_or_else(|_| "127.0.0.1:8080".to_string());

    let state = AppState {
        queue: Arc::new(Mutex::new(VecDeque::new())),
        token: Arc::new(token),
    };

    let app = Router::new()
        .route("/notify", post(post_notify).get(get_notify))
        .route("/health", get(health))
        .with_state(state);

    let listener = tokio::net::TcpListener::bind(&addr)
        .await
        .unwrap_or_else(|e| panic!("failed to bind {addr}: {e}"));
    println!("notify-server listening on http://{addr}");
    println!("  POST /notify   (Authorization: Bearer <token>)  body: {{app,title,body}}");
    println!("  GET  /notify   (Authorization: Bearer <token>)  drains queue");
    println!("  GET  /health   (open)");

    axum::serve(listener, app)
        .with_graceful_shutdown(shutdown_signal())
        .await
        .unwrap();
}

async fn shutdown_signal() {
    let _ = tokio::signal::ctrl_c().await;
    println!("\nshutting down");
}
