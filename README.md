# wled-bambu-chamber-light

A WLED usermod that mirrors WLED's on/off state onto a Bambu Lab 3D
printer's chamber light, by connecting *directly* to the printer's own
local MQTT broker (TLS, `bblp` / LAN Access Code auth). No Home
Assistant, no bridge script, no cloud account — this usermod is itself
the MQTT client that talks to the printer.

This repo is intentionally small: it's just the usermod source
(`bambu_chamber_light.cpp`, `library.json`) plus a GitHub Actions
workflow that assembles a full WLED build and compiles it for you. You
don't need PlatformIO, a toolchain, or a WLED source checkout on your
own machine — GitHub's runners do that.

## 1. Printer-side prerequisite

The printer's local MQTT broker and LAN Access Code are only exposed
once **Developer Mode / LAN Only Mode** is enabled: touchscreen →
Settings → WLAN (or General → Developer Mode, depending on firmware
version). That screen also shows the **LAN Access Code**, and confirms
the printer's **serial number** and **IP address**.

## 2. Get this repo onto GitHub

Create a new empty repository (e.g. `wled-bambu-chamber-light`) under
your own account, then push these files to it — either via GitHub's
web UI ("Add file → Upload files", drag in this folder's contents), or
from a shell:

```bash
cd wled-bambu-chamber-light
git init
git add .
git commit -m "Bambu chamber light sync usermod"
git branch -M main
git remote add origin git@github.com:<you>/wled-bambu-chamber-light.git
git push -u origin main
```

## 3. Let GitHub Actions build the firmware

Pushing to `main` triggers `.github/workflows/build.yml` automatically.
It:

1. Checks out this repo and a pinned WLED release (`v16.0.1` — the
   latest stable tag as of this writing).
2. Drops the usermod into WLED's `usermods/` folder and writes a
   `platformio_override.ini` that builds the standard `esp32dev`
   environment plus this usermod (nothing else about the stock build
   is changed).
3. Installs PlatformIO and runs `pio run`.
4. Uploads the resulting `firmware.bin` as a workflow artifact.

Watch it run under the repo's **Actions** tab. When it finishes (a
few minutes — most of the time is toolchain download), open the run,
scroll to **Artifacts**, and download `WLED_esp32dev_bambu`. Unzip it
to get `firmware.bin`.

You can re-run the build anytime from the Actions tab
("Run workflow") without pushing a new commit, e.g. after bumping
`WLED_REF` in the workflow to pick up a newer WLED release.

## 4. Flash it onto your existing WLED device

Since this is an **existing WLED install**, use WLED's own OTA update
rather than a full serial reflash — it replaces only the firmware
partition and leaves your saved presets, segments, and `cfg.json`
(WiFi credentials, effects, etc.) in place:

1. In the WLED web UI: **Config → Security & Setup**.
2. Scroll to **Manual OTA Update**, choose `firmware.bin`, click
   **Update**.
3. The device reboots automatically once flashing completes.

If OTA is disabled on your device, or you'd rather do a wired update,
`esptool.py write_flash 0x10000 firmware.bin` over USB works too (this
one doesn't erase the filesystem partition either, so settings survive
— only a full `esptool erase_flash` would wipe them, which you don't
need here).

## 5. Configure it

After reboot, go to **Config → Usermods** in the WLED UI. A new
"BambuChamberLight" section appears with:

- **enabled** — turn the whole thing on
- **printer-ip** — the printer's LAN IP
- **printer-serial** — the printer's serial number
- **access-code** — the LAN Access Code from step 1
- **led-node** — `chamber_light` by default; some models expose the
  toolhead light as `work_light` instead — switch this if the chamber
  light doesn't respond
- **invert** — flip the mapping (printer light off when WLED is on,
  etc.)

None of this needs a rebuild to change later — it's saved to
`cfg.json` on the device like any other usermod setting.

## How it behaves

- Opens its own MQTT session to `<printer-ip>:8883` (TLS, self-signed
  cert accepted via `setInsecure()`, `bblp` / access-code auth) — this
  is exactly how every other LAN-only Bambu integration connects.
- Reconnects at most once every 30 seconds if the printer is
  unreachable (powered off, wrong IP, etc.), so a dead printer never
  blocks WLED's own LED output.
- Every WLED on/off transition (and once right after connecting, to
  resync) sends one `ledctrl` MQTT publish; this is one-directional,
  WLED → printer, with no polling or read-back from the printer.
- Current sync status is surfaced in WLED's own Info panel.

## Making changes later

Edit `bambu_chamber_light.cpp`, commit, push — the Action rebuilds
automatically and a fresh `firmware.bin` artifact appears on that run.
