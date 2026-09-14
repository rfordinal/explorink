# Webserver Endpoints

This document describes the HTTP, WebSocket, WebDAV, and discovery endpoints
available while ExplorInk is in File Transfer mode.

- HTTP server: port 80
- WebSocket upload server: port 81
- UDP discovery listener: port 8134
- WebDAV: port 80, handled by the same HTTP server

Examples use `explorink.local`. The real hostname is per-device --
`explorink-x4.local`, `explorink-x4pro.local`, `explorink-t5s3pro.local` --
built from `DeviceIdentity::activeBoardId()`
(`lib/DeviceIdentity/DeviceIdentity.cpp`) so several ExplorInk devices on one LAN stay
distinguishable; see `CrossPointWebServerActivity.cpp`'s `apHostname()`. If
mDNS does not resolve on your network, use the IP address shown on the device
screen.

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

Removed 2026-09-14: the whole "Connect to Calibre" network mode
(`CalibreConnectActivity`, `NetworkMode::CONNECT_CALIBRE`) and its menu entry.
It ran the same web server in STA mode with Calibre-specific instructions on
screen; ExplorInk is not an ebook reader (`docs/thesis.md`), so a Calibre
wireless-transfer workflow was never in scope. `GET /api/status`, file
management and WebSocket upload are unaffected -- those serve the file manager,
not Calibre.

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
curl http://explorink.local/api/status
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
curl "http://explorink.local/api/files?path=/Books"
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
curl -OJ "http://explorink.local/download?path=/Books/MyBook.epub"
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
curl -X POST -F "file=@mybook.epub" "http://explorink.local/upload?path=/Books"
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
curl -X POST -d "name=NewFolder&path=/" http://explorink.local/mkdir
```

Form parameters:

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `name` | Yes | - | New folder name |
| `path` | No | `/` | Parent folder |

### `POST /rename`

Renames a file.

```bash
curl -X POST -d "path=/Books/old.epub&name=new.epub" http://explorink.local/rename
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
curl -X POST -d "path=/Books/mybook.epub&dest=/Read" http://explorink.local/move
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
curl -X POST -d "path=/Books/mybook.epub" http://explorink.local/delete
curl -X POST -d 'paths=["/Books/old.epub","/OldFolder"]' http://explorink.local/delete
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

The WebSocket path is used for fast binary uploads from the file manager.

Connection:

```text
ws://explorink.local:81/
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

### `GET` sent one byte per file, and then reset the board

Two separate defects, found 2026-09-05 and 2026-09-06 pulling logs off a
LilyGo T5 S3 Pro. Both are fixed. Both were upstream's.

**One byte per file.** `handleGet()` ended in `client.write(file)`. That worked
while `Storage.open()` returned an SdFat `FsFile`, which derives from `Stream`:
the call matched `NetworkClient::write(Stream&)` and streamed the file.
Upstream's `6ff5fcd9` (2026-02-28, thread-safe `HalFile`) wrapped it, and
`HalFile` derives from `Print`, not `Stream` (`lib/hal/HalStorage.h`). The
overload stopped matching, the compiler took `HalFile::operator bool()`,
promoted the `true` to `uint8_t` and wrote a single `0x01`. The call site never
changed; its meaning did, and nothing warned, because every step is a legal
conversion.

The reply still carried the real `Content-Length`, so clients waited for a body
that never came and reported a truncated transfer rather than an error. That is
why it read as a flaky network for six months. `WebDAVHandler.cpp` now calls
`Storage.readFileToStream(path, client)`. Verified on hardware 2026-09-06: a
7401-byte `power.csv` returns exactly 7401 bytes.

**Then a big file reset the board.** With bytes actually flowing, a 732 765-byte
tile reset the device 16 s in, 545 kB delivered:

```text
[179485] [DBG] [DAV] GET /trailink/base/13/4485/2842.tib
E (195459) task_wdt: Task watchdog got triggered ... - loopTask (CPU 1)
E (195459) task_wdt: Aborting.
```

`SDCardManager::readFileToStream()` streamed in a loop that never blocked, so
`loopTask` could not feed the watchdog. Fixed in the SDK fork, which now yields
every 100 ms (`freeink-sdk` `55a4958`, upstream PR Free-Ink/freeink-sdk#83).
The same file now completes in 6.7 s, and two downloads of it are byte-for-byte
identical.

**Yielding tripled throughput** rather than costing any: ~34 kB/s before the
reset, ~109 kB/s after. Holding the core starved the networking stack.

**Still open: a large `GET` blocks `loopTask` for about 7 s.** Measured
2026-09-06, `New max loop duration: 7197 ms` for that tile. Under the watchdog
now, but it is one reason `loop_max_ms` reads in seconds and why the task
watchdog cannot be tightened. Moving the transfer off `loopTask` would settle
it; nothing has measured what that would cost.

The one-byte bug is still live in CrossPoint `develop`
(`src/network/WebDAVHandler.cpp`, `client.write(file)`) and is reported there.

## UDP Discovery

The server listens on UDP port `8134`. When it receives the text payload
`hello`, it replies to the sender with:

```text
explorink (on <hostname>);81
```

The final field is the WebSocket upload port.

## Network Modes

### Station Mode (STA)

- Device joins an existing 2.4 GHz Wi-Fi network.
- The per-device hostname (`explorink-x4pro.local`, ...) is advertised with
  mDNS when available.
- `/api/status` returns `"mode": "STA"` and RSSI in dBm.

### Access Point Mode (AP)

- Device creates an open hotspot named after itself, e.g. `ExplorInk-X4Pro`.
- The device shows a Wi-Fi QR code and URL QR code.
- The fallback IP is typically `192.168.4.1`.
- `/api/status` returns `"mode": "AP"` and `"rssi": 0`.
