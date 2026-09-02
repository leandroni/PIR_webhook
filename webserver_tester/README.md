# Call Logger

A tiny Flask app that logs **every HTTP request made to it**, no matter what
URL or method was used, and shows a live-updating web page with:

- **Time** the request was received
- **Method** (GET, POST, ...)
- **URL / path** requested (including query string)
- **Source IP**

The page (`/`) polls itself every 2 seconds and updates the table. The
polling requests and the "clear log" button are excluded from the log
itself so they don't clutter it.

## Quick start (local test)

```bash
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
python3 app.py
```

Then open `http://<server-ip>:8080/` in a browser. Any request to the
server — `curl http://<server-ip>:8080/anything/at/all` — will show up in
the table.

## Deploying on Ubuntu as a systemd service

1. Copy the project to the server, e.g. into `/opt/call_logger`:
   ```bash
   sudo mkdir -p /opt/call_logger
   sudo cp app.py requirements.txt /opt/call_logger/
   cd /opt/call_logger
   ```

2. Create a virtual environment and install dependencies:
   ```bash
   sudo python3 -m venv venv
   sudo ./venv/bin/pip install -r requirements.txt
   ```

3. Set ownership so the service user can run it (the sample unit file uses
   `www-data`):
   ```bash
   sudo chown -R www-data:www-data /opt/call_logger
   ```

4. Install the systemd unit:
   ```bash
   sudo cp call-logger.service /etc/systemd/system/call-logger.service
   sudo systemctl daemon-reload
   sudo systemctl enable --now call-logger
   ```

5. Check it's running:
   ```bash
   sudo systemctl status call-logger
   curl http://127.0.0.1:8080/
   ```

6. If you have a firewall (ufw) enabled, allow the port:
   ```bash
   sudo ufw allow 8080/tcp
   ```

## Putting it behind Nginx (optional, recommended for real deployments)

The built-in Flask server (`app.run(...)`) is fine for this kind of
low-traffic diagnostic tool, but if you want it on port 80/443 with TLS,
put it behind Nginx as a reverse proxy and keep Flask listening on
`127.0.0.1:8080`. Example server block:

```nginx
server {
    listen 80;
    server_name your-domain.example;

    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }
}
```

The app already reads `X-Forwarded-For` if present, so the log will show
the real client IP rather than Nginx's own address (127.0.0.1) once this
header is set.

## Data storage

Every request is persisted to a SQLite database, `call_logger.db`,
created automatically in the same folder as `app.py` the first time the
app starts (you'll see `Created new database at ...` printed on first
run). The page/API only display the most recent 1000 rows, but all rows
remain in the database until you hit **Clear log** (which runs `DELETE
FROM logs`).

To inspect it directly:
```bash
sqlite3 call_logger.db "SELECT * FROM logs ORDER BY id DESC LIMIT 20;"
```

If you redeploy to `/opt/call_logger` per the systemd instructions above,
the database file lives at `/opt/call_logger/call_logger.db` — back that
file up if you want to preserve history across reinstalls.

## Notes

- There's no authentication on the page or on `/api/clear` — anyone who
  can reach the server can view or clear the log. Fine for internal/lab
  use; put it behind a VPN, firewall rule, or add basic auth if it'll be
  exposed more broadly.
- Every request (any path, any method) gets a `200 OK` response so
  whatever is calling this server doesn't get errors — only the log
  matters.
