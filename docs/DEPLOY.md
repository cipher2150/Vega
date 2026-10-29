# Deploying to Oracle Cloud (Always Free, Ampere A1, India region)

This takes the service from a fresh Oracle Cloud account to a public HTTPS dashboard. The
target is an Ubuntu 24.04 **aarch64** VM, the same ISA as the M1 laptop and CI.

## 1. Create the VM

1. Console → **Compute → Instances → Create instance**.
2. **Image:** Canonical Ubuntu 24.04 (aarch64). **Shape:** `VM.Standard.A1.Flex`, 4 OCPU / 24 GB
   (the Always Free maximum). 2 OCPU / 12 GB also works.
3. **Region:** your home region (Mumbai `ap-mumbai-1` or Hyderabad `ap-hyderabad-1`). A1
   capacity is often exhausted ("Out of host capacity"). If so, retry later or try another
   availability domain.
4. **Networking:** let it create a VCN with a public subnet, and assign a public IPv4.
5. **SSH:** upload your public key. Then create the instance and note the public IP.

## 2. Open ports 80 and 443

The OCI firewall and the image's own firewall both block inbound traffic by default.
Open both:

1. **VCN security list:** Networking → VCN → subnet → Security List → *Add Ingress Rules*.
   Add source `0.0.0.0/0`, TCP, destination port `80`, and another rule for `443`.
2. **On the VM:** Oracle's Ubuntu image ships iptables rules that REJECT everything except
   SSH. This is the most common reason "it works but I can't reach it":
   ```sh
   sudo iptables -I INPUT 6 -m state --state NEW -p tcp --dport 80  -j ACCEPT
   sudo iptables -I INPUT 6 -m state --state NEW -p tcp --dport 443 -j ACCEPT
   sudo netfilter-persistent save
   ```

Port 8080 stays closed. Only Caddy talks to the service, over localhost.

## 3. Check the exchanges are reachable *from this VM* (do this first)

Exchange geo-blocking depends on the server's IP range, not on where you are. Run the
same probe used during design (DESIGN §9 item 5) on the VM:

```sh
sudo apt-get update && sudo apt-get install -y python3-venv git
git clone <your repo url> cbook && cd cbook
python3 -m venv .venv && .venv/bin/pip install websockets==13.1
.venv/bin/python -I docs/probes/probe_raw_frames.py
.venv/bin/python -I docs/probes/probe_protocols.py 60
```

(Ubuntu's Python uses the system CA store, so `SSL_CERT_FILE` isn't needed here.)

- **All four venues print frames:** keep every venue enabled.
- **A venue errors (HTTP 403, connection reset, timeout):** set `"enabled": false` for it in
  `config/production.json`. The rest of the service is unaffected; that venue just shows as
  `disabled` on the dashboard.

Also record the timer frequency, because it sets the latency-measurement resolution
(README "Measurement"):

```sh
printf '#include <cstdio>\nint main(){unsigned long f;asm volatile("mrs %%0, cntfrq_el0":"=r"(f));printf("%%lu\\n",f);}' > f.cpp
g++ -O2 f.cpp -o f && ./f
```

## 4. Clock sync

Exchange-lag numbers are only as good as the VM's clock. Ubuntu 24.04 uses `chrony`:

```sh
sudo apt-get install -y chrony && chronyc tracking   # note "System time" and "Root dispersion"
```

## 5a. Run with Docker (recommended)

```sh
sudo apt-get install -y docker.io && sudo usermod -aG docker $USER && newgrp docker
docker build -f deploy/Dockerfile -t cbook:latest .        # ~10-15 min on 2 OCPU; runs tests
docker run -d --name cbook --restart unless-stopped \
  -p 127.0.0.1:8080:8080 cbook:latest
docker logs -f cbook          # expect "connected (epoch 1)" for each venue
```

For pinning to work as configured (`book_cpu: 3`), the container must see CPU 3. That's
the default when you don't pass `--cpuset-cpus`.

## 5b. Or run with systemd (no Docker)

```sh
sudo apt-get install -y build-essential cmake ninja-build libssl-dev zlib1g-dev
cmake --preset release -G Ninja -DCB_BUILD_BENCH=OFF && cmake --build --preset release
ctest --preset release
sudo useradd --system --no-create-home cbook
sudo install -D build/release/cbook /opt/cbook/bin/cbook
sudo mkdir -p /opt/cbook/web /etc/cbook && sudo cp web/* /opt/cbook/web/
sudo cp config/production.json /etc/cbook/config.json
sudo cp deploy/consolidated-book.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now consolidated-book
journalctl -u consolidated-book -f
```

Optional, for a quieter book core: add `isolcpus=3 nohz_full=3` to
`GRUB_CMDLINE_LINUX_DEFAULT` in `/etc/default/grub`, run `sudo update-grub`, and reboot.
The kernel then schedules nothing else on CPU 3 except the pinned book thread.

## 6. HTTPS with Caddy

You need a DNS name that points at the VM. If you don't own a domain, `sslip.io` maps
`<ip-with-dashes>.sslip.io` to that IP, and Let's Encrypt will issue a certificate for it.

```sh
sudo apt-get install -y caddy
sudo cp deploy/Caddyfile /etc/caddy/Caddyfile
sudo sed -i 's/cbook.example.com/203-0-113-7.sslip.io/' /etc/caddy/Caddyfile   # your name
sudo mkdir -p /var/log/caddy && sudo systemctl reload caddy
```

Open `https://<name>/`. The page connects back to `wss://<name>/ws` through the same
proxy. Caddy upgrades WebSockets automatically.

## 7. Verify

- The dashboard shows each enabled venue as `connected` and its books as `live`.
- `book allocs` in the header stays at 0.
- `docker logs cbook` (or `journalctl`) shows no repeated `down:` warnings. An occasional
  reconnect is normal: exchanges reset connections.

## Operating notes

- **Record a session on the VM** for replay benchmarks on Ampere: run the binary with
  `live --record /var/lib/cbook/session.cbrec` for a few minutes, copy the file back, and
  `cbook replay --file session.cbrec`.
- **Memory:** measured resident size was 49.7 MB (`ps -o rss`, macOS, all four venues
  live, recording on, 2 minutes in). The largest pieces are simdjson's preallocated
  buffers (8 MiB frame limit for Coinbase, 1 MiB for each of the others) and the
  4 MiB event ring per venue. The free-tier VM has 24 GB.
- **CPU:** with `"idle": "spin"` the book thread uses 100% of its core by design. With
  `"backoff"` the whole process measured 7.4% CPU on the M1.
- **Updating:** `git pull && docker build ... && docker rm -f cbook && docker run ...`
  (same flags as above).
