# Web Server Guide

This guide explains how to use TrailInk's built-in web server. It moves files
on and off the SD card. That is all it does -- device settings and Wi-Fi are
configured on the device itself, not in a browser.

## Overview

The web server is available while the device is in **File Transfer** or
**Calibre Wireless** mode. It can:

- Upload, download, rename, move, and delete files on the SD card
- Create folders
- Accept WebDAV clients and Calibre wireless uploads
- Report device status (firmware, IP, free heap, uptime)

It cannot change device settings or Wi-Fi credentials. Those pages were removed
(2026-08-05) so there is one settings surface, not two that can disagree. Use
**Settings** on the device.

The server does not require authentication. Use it only on trusted private
networks or in hotspot mode when you control who is connected.

## Starting File Transfer

1. From the Home screen, select **File Transfer**.
2. Choose one of the available modes:

| Mode | Use when |
|------|----------|
| **Join Network** | You want the reader to join an existing Wi-Fi network. |
| **Calibre Wireless** | You want to receive books from the CrossPoint Calibre plugin workflow. |
| **Create Hotspot** | You want the reader to create its own open Wi-Fi network. |

## Join Network Mode

1. Select **Join Network**.
2. If you have saved Wi-Fi credentials, CrossPoint first tries the last
   connected network, then other visible saved networks in signal-strength
   order. Press **Back** to cancel or **Confirm** to stop auto-connect and show
   the network list.
3. If the network list is shown, pick a 2.4 GHz Wi-Fi network from the scan
   results.
4. Enter the password if prompted.
5. Save credentials if you want the reader to reconnect automatically next time.

After connection, the reader shows:

- The connected SSID
- A QR code for the web URL
- The direct IP URL, for example `http://192.168.1.102/`
- The mDNS fallback URL, usually `http://crosspoint.local/`

Use either URL from a phone, tablet, or computer on the same network.

## Create Hotspot Mode

1. Select **Create Hotspot**.
2. Connect your phone or computer to the open Wi-Fi network:

```text
CrossPoint-Reader
```

3. Open the URL shown on the reader. `http://crosspoint.local/` is preferred
   when supported; the fallback IP is typically `http://192.168.4.1/`.

The reader displays one QR code for joining the hotspot and another QR code for
opening the web interface.

## Calibre Wireless Mode

Calibre Wireless starts the same web server in station mode, then displays setup
instructions and upload progress on the reader. Use this mode with the
CrossPoint Calibre plugin or other clients that speak the documented WebSocket
upload protocol.

For Calibre OPDS browsing, add `/opds` to the catalog URL. OPDS servers are
configured on the device (**Settings > System > OPDS Servers**,
`src/activities/settings/SettingsActivity.cpp:75`); the browser no longer has a
card for them.

## Web Interface

The browser UI has two pages.

### Home

The Home page shows firmware status, network mode, IP address, device type,
uptime, and free heap.

### File Manager

The File Manager page can:

- Browse SD-card folders
- Upload files over HTTP multipart POST
- Create folders
- Download files
- Rename files
- Move files into existing folders
- Delete one or more selected files or empty folders

Uploads go over HTTP only. The rewritten page dropped the WebSocket upload
path; the port-81 WebSocket server still runs, because Calibre clients use it.
Whether that costs upload speed is unmeasured -- comparing the two on a large
file would settle it.

Existing files with the same name are overwritten by uploads. When EPUB files
are overwritten, moved, renamed, or deleted through the web server, the matching
book cache is cleared so stale metadata is not reused.

## Command Line Use

Power users can use `curl`, WebDAV clients, or WebSocket clients while the web
server is running.

Endpoint details are documented in [webserver-endpoints.md](./webserver-endpoints.md).

## Security Notes

- The HTTP server runs on port 80.
- The WebSocket upload server runs on port 81.
- There is no authentication.
- Anyone on the same network can access the web interface while it is running.
- The server stops when you exit File Transfer or Calibre Wireless mode.
- Hotspot mode creates an open network for connectivity fallback; disconnect when done.

## Tips

1. Use **Create Hotspot** when no trusted network is available.
2. Prefer `crosspoint.local` when available, but keep the displayed IP address as a fallback.
   That hostname is the literal value in the code, not a stale name.
3. Move closer to the router if upload progress stalls in Join Network mode.
4. Copy custom `.cpfont` families to `/.fonts/` or `/fonts/` on the SD card. The
   web font manager was removed; the device still loads SD fonts at boot.
5. Exit File Transfer mode when finished to conserve battery.

## Related Documentation

- [User Guide](../USER_GUIDE.md)
- [Webserver Endpoints](./webserver-endpoints.md)
- [SD Card Fonts](./sd-card-fonts.md)
- [Troubleshooting](./troubleshooting.md)
