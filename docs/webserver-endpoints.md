# Webserver Endpoints

This document describes the HTTP, WebSocket, WebDAV, and discovery endpoints
available while TrailInk is in File Transfer or Calibre Wireless mode.

- HTTP server: port 80
- WebSocket upload server: port 81
- UDP discovery listener: port 8134
- WebDAV: port 80, handled by the same HTTP server

Examples use `crosspoint.local`. That is the literal mDNS hostname in the code
(`src/activities/network/CrossPointWebServerActivity.cpp:25`), not a stale
name. If mDNS does not resolve on your network, use the IP address shown on the
device screen.

## HTTP Pages

| Method | Path | Purpose |
|--------|------|---------|
| `GET` | `/` | Home/status page |
| `GET` | `/files` | File manager page |

## What this server is not

The HTTP server is a config and file-transfer surface, nothing more. It is not
a reader shell, and it is not where reader features get a second UI.

On `develop` it is still the only way to get files onto the SD card over the
air. A BLE map-tile push exists on the `ble-map-transfer` branch and is not
merged here yet; when it lands, HTTP stops being the tile path.

Routes are registered in one place, `src/network/CrossPointWebServer.cpp:134-154`.
Read that list before trusting this document.

Endpoints removed from the fork (2026-08-05), and why:

| Removed | Reason |
|---|---|
| `GET /fonts`, `GET/POST /api/fonts*` | SD-font manager is a reader feature. SD fonts still load at boot; only the web UI for installing them is gone. |
| `GET/POST /api/opds*` | OPDS is an ebook catalog. The device-side OPDS subsystem still exists (`src/OpdsServerStore.cpp` and 19 other files) and is a separate removal. |
| `GET /js/jszip.min.js` | Only fed the in-browser EPUB converter that lived in `FilesPage.html`. |
| `GET /settings`, `GET/POST /api/settings` | Device settings belong in the device menus. One settings surface, not two that can disagree. |
| `GET/POST /api/wifi*` | The Wi-Fi card lived on the settings page and had no other consumer. Wi-Fi is chosen on the device (`src/activities/network/WifiSelectionActivity.cpp`) -- you need Wi-Fi before the browser is reachable anyway. |

`FilesPage.html` was also rewritten as a plain file manager (list, upload,
download, mkdir, rename, move, delete). The EPUB-to-image conversion, cover
extraction and OPF parsing it used to do in the browser are gone. The file
JSON API it talks to is unchanged.

Flash cost before and after, measured from the `*CompressedSize` constants in
the generated headers:

| Blob | Before | After |
|---|---|---|
| `FilesPageHtml` | 48 985 B | 4 241 B |
| `jszip_minJs` | 28 379 B | removed |
| `SettingsPageHtml` | 5 647 B | removed |
| `FontsPageHtml` | 3 285 B | removed |
| `HomePageHtml` | 1 446 B | 1 419 B |
| **total** | **87 742 B** | **5 660 B** |

Whole image went 3 909 616 B to 3 797 821 B, about 109 KB. That figure is
approximate -- the baseline build predated one unrelated commit. The 82 082 B
blob figure is exact.

The image dropped ~13.6 KB when the settings and Wi-Fi handlers went, while
their HTML blob was only 5 157 B. The remaining ~8.4 KB is C++: five handler
bodies, each with its own ArduinoJson serialize/deserialize instantiation.
Likely but unverified -- a map-file diff would confirm it.

## Device Status

### `GET /api/status`

```bash
curl http://crosspoint.local/api/status
```

Response:

```json
{
  "version": "1.0.0",
  "ip": "192.168.1.100",
  "mode": "STA",
  "rssi": -45,
  "freeHeap": 123456,
  "uptime": 3600,
  "device": "X4"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `version` | string | Firmware version |
| `ip` | string | Device IP address |
| `mode` | string | `"STA"` for joined Wi-Fi or `"AP"` for hotspot mode |
| `rssi` | number | Wi-Fi RSSI in dBm; `0` in AP mode |
| `freeHeap` | number | Free heap in bytes |
| `uptime` | number | Seconds since boot |
| `device` | string | `"X3"` or `"X4"` hardware detection |

## Every response is gzipped, so `curl` needs `--compressed`

Without the flag `curl` writes the raw deflate stream to the terminal, which
prints as binary noise and reads exactly like a broken server or a wrong port.
It is neither. **Every example on this page assumes the flag**, and it is left
off below only to keep the lines short:

```bash
curl -s --compressed "http://<device-ip>/api/files?path=/"
```

Cost one wasted call on 2026-09-04 pulling a power log off a T5 S3 Pro. A
browser sends `Accept-Encoding` on its own, so this only ever bites a script.

## File Management

### `GET /api/files`

Lists files and folders under a directory.

```bash
curl "http://crosspoint.local/api/files?path=/Books"
```

Query parameters:

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `path` | No | `/` | Directory to list |

Response:

```json
[
  {"name":"MyBook.epub","size":1234567,"isDirectory":false,"isEpub":true},
  {"name":"Notes","size":0,"isDirectory":true,"isEpub":false}
]
```

Hidden dotfiles are omitted unless the device setting `showHiddenFiles` is
enabled. `System Volume Information` and `XTCache` are always hidden/protected.

### `GET /download`

Downloads a file from the SD card.

```bash
curl -OJ "http://crosspoint.local/download?path=/Books/MyBook.epub"
```

Query parameters:

| Parameter | Required | Description |
|-----------|----------|-------------|
| `path` | Yes | File path to download |

Protected dotfiles, `System Volume Information`, and `XTCache` cannot be
downloaded. EPUB files are served as `application/epub+zip`; other files use
`application/octet-stream`.

### `POST /upload`

Uploads a file with HTTP multipart form data.

```bash
curl -X POST -F "file=@mybook.epub" "http://crosspoint.local/upload?path=/Books"
```

Query parameters:

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `path` | No | `/` | Destination directory |

Successful response:

```text
File uploaded successfully: mybook.epub
```

Notes:

- Existing files with the same name are overwritten.
- EPUB cache data for the uploaded path is cleared after a successful upload.
- HTTP upload uses a 4 KB write buffer before flushing to the SD card.

### `POST /mkdir`

Creates a folder.

```bash
curl -X POST -d "name=NewFolder&path=/" http://crosspoint.local/mkdir
```

Form parameters:

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `name` | Yes | - | New folder name |
| `path` | No | `/` | Parent folder |

### `POST /rename`

Renames a file.

```bash
curl -X POST -d "path=/Books/old.epub&name=new.epub" http://crosspoint.local/rename
```

Form parameters:

| Parameter | Required | Description |
|-----------|----------|-------------|
| `path` | Yes | Existing file path |
| `name` | Yes | New file name, not a path |

Only files can be renamed through this endpoint. The old EPUB cache path is
cleared before the rename.

### `POST /move`

Moves a file into an existing folder.

```bash
curl -X POST -d "path=/Books/mybook.epub&dest=/Read" http://crosspoint.local/move
```

Form parameters:

| Parameter | Required | Description |
|-----------|----------|-------------|
| `path` | Yes | Existing file path |
| `dest` | Yes | Existing destination folder |

Only files can be moved through this endpoint. The old EPUB cache path is
cleared before the move.

### `POST /delete`

Deletes one or more files or empty folders.

```bash
curl -X POST -d "path=/Books/mybook.epub" http://crosspoint.local/delete
curl -X POST -d 'paths=["/Books/old.epub","/OldFolder"]' http://crosspoint.local/delete
```

Form parameters:

| Parameter | Required | Description |
|-----------|----------|-------------|
| `path` | Yes, unless `paths` is provided | Single path to delete |
| `paths` | Yes, unless `path` is provided | JSON array of paths to delete |

Protected items cannot be deleted. Non-empty folders are rejected. EPUB cache
data for deleted files is cleared.

## WebSocket Upload

### Port 81

The WebSocket path is used for fast binary uploads from the file manager and
Calibre plugin workflows.

Connection:

```text
ws://crosspoint.local:81/
```

Protocol:

1. Client sends text: `START:<filename>:<size>:<path>`
2. Server replies `READY`
3. Client sends binary chunks
4. Server sends `PROGRESS:<received>:<total>` every 64 KB or at completion
5. Server sends `DONE` when complete or `ERROR:<message>` on failure

Example session:

```text
Client -> START:mybook.epub:1234567:/Books
Server -> READY
Client -> [binary chunk]
Server -> PROGRESS:65536:1234567
...
Server -> DONE
```

Error messages include:

| Message | Cause |
|---------|-------|
| `ERROR:Upload already in progress` | A second upload was started before the first completed |
| `ERROR:Invalid START format` | Malformed START message or invalid size token |
| `ERROR:Failed to create file` | Destination file could not be opened |
| `ERROR:No upload in progress` | Binary data arrived without a matching START |
| `ERROR:Upload overflow` | Client sent more bytes than declared |
| `ERROR:Write failed - disk full?` | SD write failed |

Incomplete WebSocket uploads are deleted on disconnect or error.

## WebDAV

The same HTTP server registers a WebDAV-compatible handler for file manager clients.

Supported methods:

```text
OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, MKCOL, MOVE, COPY, LOCK, UNLOCK
```

Notes:

- `PUT` writes to a temporary `.davtmp` file first, then renames it into place.
- Protected paths are rejected.
- `LOCK` and `UNLOCK` are accepted for client compatibility only. The server
  does not implement full WebDAV Class 2 locking semantics such as persistent
  locks or lock discovery.

## UDP Discovery

The server listens on UDP port `8134`. When it receives the text payload
`hello`, it replies to the sender with:

```text
crosspoint (on <hostname>);81
```

The final field is the WebSocket upload port.

## Network Modes

### Station Mode (STA)

- Device joins an existing 2.4 GHz Wi-Fi network.
- `crosspoint.local` is advertised with mDNS when available.
- `/api/status` returns `"mode": "STA"` and RSSI in dBm.

### Access Point Mode (AP)

- Device creates an open hotspot named `CrossPoint-Reader`.
- The device shows a Wi-Fi QR code and URL QR code.
- The fallback IP is typically `192.168.4.1`.
- `/api/status` returns `"mode": "AP"` and `"rssi": 0`.

### Calibre Wireless

Calibre Wireless starts the same web server in STA mode and displays setup
instructions plus WebSocket upload progress on the device screen.
