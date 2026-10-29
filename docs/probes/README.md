# Protocol probes (dev-only, Python)

These scripts check our reading of each exchange's docs against live data. They aren't
part of the service.

```sh
python3 -m venv .venv && .venv/bin/pip install websockets==13.1 certifi
# python.org builds of Python ship without a CA bundle:
export SSL_CERT_FILE=$(.venv/bin/python -c "import certifi;print(certifi.where())")
.venv/bin/python -I docs/probes/probe_raw_frames.py      # first raw frames per venue
.venv/bin/python -I docs/probes/probe_protocols.py 60    # 60 s: checksum, sequence, depth checks
```

`probe_2026-10-29.txt` holds the outputs cited in `docs/DESIGN.md`. Rerun both scripts on
the deployment VM before going live (DESIGN §9 item 5).
