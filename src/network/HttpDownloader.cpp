#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <base64.h>

#include <cctype>
#include <cstdint>
#include <functional>
#include <string>

#include "DownloadResumePolicy.h"

#if defined(FREEINK_NET_WOLFSSL)
#include <SecureHttpClient.h>

extern "C" void wolfSSL_Arduino_Serial_Print(const char* const msg) { LOG_DBG("WOLFSSL", "%s", msg); }
#else
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#endif

namespace {
#if !defined(FREEINK_NET_WOLFSSL)
// RX holds the response headers. Smaller buffers leave enough contiguous heap
// for mbedTLS on redirect-heavy OPDS feeds while still preserving the headers
// we read directly (Location, Content-Length).
constexpr int HTTP_RX_BUF = 2048;
constexpr int HTTP_TX_BUF = 512;
#endif
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room. esp_http_client's timeout_ms is uint32, so unlike Arduino
// HTTPClient's uint16 setTimeout it doesn't silently truncate.
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr size_t READ_CHUNK = 1024;
constexpr int MAX_REDIRECTS = 5;
// The heap is at its emptiest the instant a connection dies; let the TLS
// teardown's frees settle before asking for a fresh handshake.
constexpr uint32_t RESUME_DELAY_MS = 250;

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;  // returns false to abort the transfer
  HttpDownloader::ProgressCallback progress;
  bool* cancelFlag = nullptr;
  size_t total = 0;
  size_t downloaded = 0;
};

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

#if defined(FREEINK_NET_WOLFSSL)
// scheme://host of a URL, for logging. A resolved download URL is a bearer
// credential -- GitHub's release CDN signs it with X-Amz-Signature -- so the
// full string must never reach a log a stolen device can be made to replay.
std::string urlOrigin(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return "<malformed url>";
  const size_t hostEnd = url.find('/', schemeEnd + 3);
  return hostEnd == std::string::npos ? url : url.substr(0, hostEnd);
}

// First-byte offset of a `Content-Range: bytes <first>-<last>/<complete>`
// header, or SIZE_MAX if the header is absent or not in that form. A 206 whose
// range does not start exactly where we asked would be spliced into the sink at
// the wrong offset, because no sink on this path can seek.
constexpr size_t NO_RANGE_START = SIZE_MAX;
size_t contentRangeStart(const std::string& header) {
  const std::string unit = "bytes ";
  if (header.compare(0, unit.size(), unit) != 0) return NO_RANGE_START;
  size_t i = unit.size();
  if (i >= header.size() || !isdigit(static_cast<unsigned char>(header[i]))) return NO_RANGE_START;
  size_t value = 0;
  for (; i < header.size() && isdigit(static_cast<unsigned char>(header[i])); ++i) {
    value = value * 10 + static_cast<size_t>(header[i] - '0');
  }
  // Anything but a '-' here means this is not a satisfied byte range (an
  // unsatisfied one reads `bytes */1234`), so refuse rather than guess.
  return (i < header.size() && header[i] == '-') ? value : NO_RANGE_START;
}
#endif

#if defined(FREEINK_NET_WOLFSSL)
// A GET that survives the connection dying mid-body. On the C3 the TLS stack
// runs out of contiguous heap partway through a multi-megabyte download and
// wolfSSL answers MEMORY_E, killing the connection with most of the transfer
// already done. Measured on an X3, 2026-11-04: free heap runs 3.6 to 8.6 kB for
// the whole download with a largest block of 2 to 5.6 kB, while at each
// handshake the largest block reads 19 to 28 kB. So the failure is a property
// of the connection, not of the board, and a fresh connection restores the
// precondition that failed -- which is why reconnecting is the mechanism here
// and not merely a retry.
//
// Every byte already handed to the sink is kept and the rest asked for with a
// Range request. That is sound only because every sink on this path is
// append-only (esp_ota_write, a file write, a string append) and none can seek
// or rewind, so sink.downloaded is the exact resume point -- and only as long
// as the server's answer is checked to actually start there, below.
HttpDownloader::DownloadError runGetWolf(const std::string& startUrl, const std::string& username,
                                         const std::string& password, Sink& sink) {
  std::string url = startUrl;
  download_resume::State resumeState;

  while (true) {
    freeink::SecureHttpClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setInsecure();
    if (!http.begin(url)) {
      LOG_ERR("HTTP", "wolfSSL bad URL: %s", urlOrigin(url).c_str());
      return HttpDownloader::HTTP_ERROR;
    }
    // Asking from byte N makes this attempt a continuation rather than a
    // restart. Sent on whatever URL this attempt starts from, so the signed CDN
    // URL a redirect hands out is what gets resumed.
    const size_t resumeFrom = sink.downloaded;
    if (resumeFrom > 0) {
      http.addHeader("Range", "bytes=" + std::to_string(resumeFrom) + "-");
    }
    // setUserAgent replaces SecureHttpClient's built-in UA; addHeader would
    // append a second User-Agent header, which strict servers reject (aiohttp
    // answers 400 "Duplicate 'User-Agent' header found").
    http.setUserAgent("TrailInk-ESP32-" TRAILINK_VERSION);
    if (!username.empty() && !password.empty()) {
      const std::string credentials = username + ":" + password;
      const String encoded = base64::encode(credentials.c_str());
      http.addHeader("Authorization", std::string("Basic ") + encoded.c_str());
    }

    // Redirects are counted per attempt, not across the whole transfer: a
    // signed-redirector answers 30x on every new connection, and a budget
    // shared with the resumes would blame "too many redirects" for what is
    // really a download that reconnected a few times.
    int redirects = 0;
    bool rangeChecked = resumeFrom == 0;
    bool rangeMismatch = false;

    LOG_DBG("HTTP", "wolfSSL GET: %s", urlOrigin(url).c_str());
    const int status = http.GET(
        [&http, &sink, &rangeChecked, &rangeMismatch, resumeFrom](const uint8_t* data, size_t len) {
          if (http.getStatus() != 200 && http.getStatus() != 206) return true;
          // Checked at the first body byte rather than before the request
          // returns, because this is the last point at which refusing costs
          // nothing: one byte further and the sink has already taken it.
          if (!rangeChecked) {
            const size_t start = contentRangeStart(http.getHeader("content-range"));
            if (start != resumeFrom) {
              rangeMismatch = true;
              return false;
            }
            rangeChecked = true;
          }
          if (!sink.write(data, len)) return false;
          sink.downloaded += len;
          if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
          return true;
        },
        [&sink]() { return sink.cancelFlag && *sink.cancelFlag; });

    // The total is read here and not from inside the callback, so a connection
    // that dies after the headers and before the first body byte still leaves a
    // size behind -- that is exactly the attempt the resume exists for. Only
    // the first attempt may set it: a 206's Content-Length counts the bytes in
    // that range, not the whole body.
    if (sink.total == 0 && resumeFrom == 0 && http.hasContentLength()) sink.total = http.getContentLength();

    if (http.aborted()) return HttpDownloader::ABORTED;
    // A range that does not start where we asked also stops the callback, so it
    // is separated from a sink refusal before the latter is reported.
    if (rangeMismatch) {
      LOG_ERR("HTTP", "wolfSSL 206 does not start at %zu, refusing to splice", resumeFrom);
      return HttpDownloader::HTTP_ERROR;
    }
    // The sink refusing data is the caller's decision, never a transport
    // failure, so it is checked before anything that could retry. The OTA
    // release-list parser stops itself this way once it has matched.
    if (http.callbackAborted()) return HttpDownloader::FILE_ERROR;

    if (isRedirect(status)) {
      if (++redirects > MAX_REDIRECTS) {
        LOG_ERR("HTTP", "wolfSSL too many redirects");
        return HttpDownloader::HTTP_ERROR;
      }
      const std::string location = http.getHeader("location");
      if (location.empty() || !freeink::SecureHttpClient::resolveUrl(url, location, url)) {
        LOG_ERR("HTTP", "wolfSSL bad redirect: %d", status);
        return HttpDownloader::HTTP_ERROR;
      }
      continue;
    }

    download_resume::Attempt attempt;
    attempt.status = status;
    attempt.bodyComplete = http.responseComplete();
    attempt.resumedFrom = resumeFrom;
    attempt.downloaded = sink.downloaded;
    attempt.total = sink.total;
    attempt.onStartUrl = url == startUrl;

    switch (download_resume::decide(attempt, resumeState)) {
      case download_resume::Step::Done:
        return HttpDownloader::OK;
      case download_resume::Step::ReResolve:
        LOG_INF("HTTP", "wolfSSL %d on resume, re-resolving from the start URL", status);
        url = startUrl;
        vTaskDelay(pdMS_TO_TICKS(RESUME_DELAY_MS));
        continue;
      case download_resume::Step::Retry:
        LOG_INF("HTTP", "wolfSSL resume %d: have %zu of %zu bytes", resumeState.resumes, sink.downloaded, sink.total);
        vTaskDelay(pdMS_TO_TICKS(RESUME_DELAY_MS));
        continue;
      case download_resume::Step::Fail:
        break;
    }

    if (status < 0) {
      LOG_ERR("HTTP", "wolfSSL request failed: %s", urlOrigin(url).c_str());
    } else if (resumeFrom > 0 && status == 200) {
      LOG_ERR("HTTP", "wolfSSL server ignored Range at %zu bytes", resumeFrom);
    } else if (status != 200 && status != 206) {
      LOG_ERR("HTTP", "wolfSSL unexpected status: %d", status);
    } else {
      LOG_ERR("HTTP", "wolfSSL incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
    }
    return HttpDownloader::HTTP_ERROR;
  }
}
#endif

#if !defined(FREEINK_NET_WOLFSSL)
// Streams a GET body through sink.write in READ_CHUNK pieces. Uses the manual
// open/fetch_headers/read path rather than esp_http_client_perform(): perform()
// pushes the whole body through an event callback and reports a chunked body
// that ends early as ESP_ERR_HTTP_INCOMPLETE_DATA, whereas the read loop streams
// large/slow files and surfaces a short read directly.
HttpDownloader::DownloadError runGet(const std::string& url, const std::string& username, const std::string& password,
                                     Sink& sink) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  // Verify HTTPS against the bundled CA roots. This build has esp-tls
  // CONFIG_ESP_TLS_INSECURE off, so an unverified TLS handshake can't be set
  // up at all; the model is public servers over verified https and local
  // servers over plain http (esp_http_client picks the transport from the URL
  // scheme, so http:// needs no cert config). The prior setInsecure() worked
  // only because Arduino's ssl_client drives mbedtls directly.
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = true;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("HTTP", "client init failed");
    return HttpDownloader::HTTP_ERROR;
  }

  esp_http_client_set_header(client, "User-Agent", "TrailInk-ESP32-" TRAILINK_VERSION);
  if (!username.empty() && !password.empty()) {
    // Preemptive Basic auth, like the prior addHeader; don't wait for a 401.
    const std::string credentials = username + ":" + password;
    const String header = "Basic " + base64::encode(credentials.c_str());
    esp_http_client_set_header(client, "Authorization", header.c_str());
  }

  // open()/read() does not auto-follow redirects (only perform() does), so step
  // 30x responses manually. OPDS download endpoints and the GitHub release CDN
  // both redirect.
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    LOG_ERR("HTTP", "open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }
  int64_t contentLength = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  for (int hop = 0; isRedirect(status) && hop < MAX_REDIRECTS; ++hop) {
    if (esp_http_client_set_redirection(client) != ESP_OK) break;
    esp_http_client_close(client);
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "redirect open failed: %s", esp_err_to_name(err));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    contentLength = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
  }

  if (status != 200) {
    LOG_ERR("HTTP", "unexpected status: %d", status);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  // fetch_headers returns 0 for a chunked response (no Content-Length); leave
  // total at 0 so progress stays silent and the size check is skipped.
  sink.total = contentLength > 0 ? static_cast<size_t>(contentLength) : 0;

  auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("HTTP", "OOM: %u byte read buffer", (unsigned)READ_CHUNK);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  while (true) {
    if (sink.cancelFlag && *sink.cancelFlag) {
      esp_http_client_cleanup(client);
      return HttpDownloader::ABORTED;
    }
    const int read = esp_http_client_read(client, buf.get(), READ_CHUNK);
    if (read < 0) {
      LOG_ERR("HTTP", "read error after %zu bytes", sink.downloaded);
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    if (read == 0) break;  // all data received
    if (!sink.write(reinterpret_cast<const uint8_t*>(buf.get()), read)) {
      esp_http_client_cleanup(client);
      return HttpDownloader::FILE_ERROR;
    }
    sink.downloaded += read;
    if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
  }

  const bool complete = esp_http_client_is_complete_data_received(client);
  esp_http_client_cleanup(client);
  if (!complete) {
    LOG_ERR("HTTP", "incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}
#endif  // !FREEINK_NET_WOLFSSL

// All HTTP(S) fetches go through wolfSSL when it is the active TLS stack: it
// speaks TLS 1.3 and reads large bodies from servers where the esp_http_client/
// mbedTLS path fails to connect or stalls mid-stream. Plain-http URLs still use a
// WiFiClient inside runGetWolf, so this is safe for non-TLS targets too.
HttpDownloader::DownloadError runGetSecure(const std::string& url, const std::string& username,
                                           const std::string& password, Sink& sink) {
#if defined(FREEINK_NET_WOLFSSL)
  return runGetWolf(url, username, password, sink);
#else
  return runGet(url, username, password, sink);
#endif
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; };
  return runGetSecure(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  outContent.clear();  // start clean; the sink appends, so don't carry prior content
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) {
    outContent.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  return runGetSecure(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = onData;
  return runGetSecure(url, username, password, sink) == OK;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool* cancelFlag,
                                                             const std::string& username, const std::string& password) {
  LOG_DBG("HTTP", "Downloading: %s -> %s", url.c_str(), destPath.c_str());

  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }
  HalFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    return FILE_ERROR;
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.cancelFlag = cancelFlag;
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };

  const DownloadError result = runGetSecure(url, username, password, sink);
  // Close before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove.
  file.close();

  if (result != OK) {
    Storage.remove(destPath.c_str());
    return result;
  }
  if (sink.downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", sink.downloaded);
  return OK;
}
