# http_server.py answers the board's HTTPS POSTs, serves the browser
# dashboard (dashboard.html, read fresh off disk on every GET / -- see the
# comment above that in http_server.py), and responds to UDP discovery
# broadcasts. cryptography (requirements.txt) is optional at runtime but
# installed here so the dashboard's Cryptography tab works out of the box.
FROM python:3.12-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY http_server.py dashboard.html ./

# certs/ is bind-mounted at runtime (see docker-compose.yml), not baked in:
# the cert/key here must be byte-for-byte identical to the root cert the
# board's firmware has pinned (NetXDuo/App/https_ca_cert.h) -- it's
# per-deployment, per-board data, not part of the server image, and
# regenerating it (gen_https_cert.sh) shouldn't require a rebuild.
EXPOSE 8443/tcp
EXPOSE 7000/udp

ENTRYPOINT ["python3", "http_server.py"]
