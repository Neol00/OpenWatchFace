# notify-server

Tiny notification queue for the ESP32-S3 watch. Runs on any always-on machine on
your LAN (a Raspberry Pi, an SBC, a small Linux server — anything that runs Rust).
The phone POSTs notifications; the watch GETs (and drains) them on each wake.

```
iPhone Shortcut (or curl)  --POST /notify-->  [queue]  --GET /notify-->  watch
```

## Endpoints
| Method | Path      | Auth                     | Purpose                              |
|--------|-----------|--------------------------|--------------------------------------|
| POST   | `/notify` | `Bearer <NOTIFY_TOKEN>`  | Add a notification `{app,title,body}`|
| GET    | `/notify` | `Bearer <NOTIFY_TOKEN>`  | Return all pending + clear them      |
| GET    | `/health` | none                     | Liveness check (`ok`)                |

Items expire after 15 min (TTL) and the queue is capped at 100 items.

## Build & run
```sh
# one-time: install Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

cd notify-server
NOTIFY_TOKEN='pick-a-long-random-secret' cargo run --release
# listens on 0.0.0.0:8080 (override with NOTIFY_ADDR=0.0.0.0:9000)
```
The release binary is a single static-ish file in `target/release/notify-server`
you can run on boot (systemd) later.

## Test the loop with curl (before involving the watch/iPhone)
Assume the server is at `192.168.1.50:8080` and token `secret`.

```sh
# health (no auth)
curl http://192.168.1.50:8080/health        # -> ok

# POST a test notification (what the iPhone Shortcut will do)
curl -X POST http://192.168.1.50:8080/notify \
  -H 'Authorization: Bearer secret' \
  -H 'Content-Type: application/json' \
  -d '{"app":"Messages","title":"Mom","body":"Call me back"}'      # -> 201

# GET as the watch would — returns the item then clears the queue
curl http://192.168.1.50:8080/notify -H 'Authorization: Bearer secret'
# -> {"items":[{"id":...,"app":"Messages","title":"Mom","body":"Call me back","ts":...}]}

# GET again immediately — empty, because the first GET drained it
curl http://192.168.1.50:8080/notify -H 'Authorization: Bearer secret'
# -> {"items":[]}
```

## Security model

- A **Bearer token** gates both `/notify` endpoints (constant-time compare).
- The server speaks **plain HTTP and binds to `127.0.0.1` by default**, so it is
  *not* directly reachable from other machines. TLS is terminated by a front (see
  below). This means a misconfigured firewall can never expose cleartext HTTP.
- The watch connects over **HTTPS** and **pins the Let's Encrypt root CA**
  (`notify_ca.h` in the firmware), so the token + notification bodies are encrypted
  in transit and the watch verifies it's the real server (no man-in-the-middle).
- The host that terminates TLS sees plaintext — the normal trust boundary. (Layering
  end-to-end body encryption on top is possible later if you don't trust the host.)

## Exposing it publicly — Tailscale Funnel (no port-forwarding, any network)

This is the intended setup when you **can't port-forward** (no static public IPv4).
Tailscale Funnel publishes the local HTTP server over **real HTTPS** on your
`*.ts.net` name, reachable from anywhere — Tailscale auto-provisions a Let's Encrypt
cert (the same root the firmware pins), so no manual certs.

On the machine running notify-server:

```sh
# 1. Install + log in to Tailscale (once).
curl -fsSL https://tailscale.com/install.sh | sh
sudo tailscale up

# 2. In the Tailscale admin console (https://login.tailscale.com/admin):
#    - enable MagicDNS + HTTPS certificates for your tailnet (Settings > DNS),
#    - enable the "Funnel" node attribute for this machine (Access controls / ACL).

# 3. Find this machine's funnel hostname:
tailscale status            # -> <host>.<tailnet>.ts.net

# 4. Run the server bound to localhost (the default), then funnel it:
NOTIFY_TOKEN='your-long-secret' ./target/release/notify-server   # 127.0.0.1:8080
sudo tailscale funnel 8080                                        # HTTPS :443 -> :8080
#   (older syntax: `tailscale funnel 443 on` / `tailscale serve https / http://127.0.0.1:8080`)
```

Then set the firmware's `NOTIFY_URL` to:
```
https://<host>.<tailnet>.ts.net/notify
```
That's it — the watch reaches it on cellular, a café, anywhere. Verify from any
network:
```sh
curl https://<host>.<tailnet>.ts.net/health                      # -> ok
```

## Exposing it publicly — reverse proxy (if you CAN port-forward a domain)

Same firmware, just a different front. Point a domain at your IP, forward 80/443,
and let **Caddy** get a Let's Encrypt cert automatically. `Caddyfile`:

```
notify.example.com {
    reverse_proxy 127.0.0.1:8080
}
```
```sh
caddy run        # gets+renews the cert, terminates TLS, proxies to the server
```
Set `NOTIFY_URL` to `https://notify.example.com/notify`. The firmware pins the same
Let's Encrypt root, so no code change between the Funnel and reverse-proxy setups.

## Run it on boot (systemd)

`/etc/systemd/system/notify-server.service`:
```ini
[Unit]
Description=notify-server
After=network-online.target

[Service]
Environment=NOTIFY_TOKEN=your-long-secret
ExecStart=/home/you/notify-server/target/release/notify-server
Restart=on-failure

[Install]
WantedBy=multi-user.target
```
```sh
sudo systemctl enable --now notify-server
```
(Tailscale's own service already restarts the funnel on boot once configured.)
