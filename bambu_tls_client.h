#pragma once

/*
 * BambuTlsClient
 * -----------------------------------------------------------------------
 * A tiny Arduino `Client` implementation backed by ESP-IDF's esp_tls
 * (mbedTLS). WLED's ESP32 Arduino framework ships without the core
 * WiFiClientSecure library, but esp_tls is always linked in (esp-tls /
 * esp_http_client depend on it), so this gives PubSubClient a TLS socket
 * with no extra library dependency.
 *
 * Deliberately minimal — it does exactly what this usermod needs:
 *   - connect(host/ip, port) with a bounded handshake timeout
 *   - NO certificate verification (esp_tls runs with VERIFY_NONE when no
 *     CA cert / bundle is supplied), matching WiFiClientSecure's
 *     setInsecure() — the Bambu printer presents a self-signed cert on
 *     the LAN and every LAN Bambu integration connects this way
 *   - non-blocking reads (so a quiet or dead socket never stalls WLED's
 *     main loop), small internal RX buffer
 *   - best-effort bounded writes
 *
 * It is not a general-purpose TLS client: no session reuse, no SNI-based
 * verification, no client certificates.
 */

#include <Arduino.h>
#include <Client.h>
#include <IPAddress.h>
#include <string.h>
#include <fcntl.h>
#include "esp_tls.h"

#ifndef ESP_TLS_ERR_SSL_WANT_READ
#define ESP_TLS_ERR_SSL_WANT_READ  -0x6900
#endif
#ifndef ESP_TLS_ERR_SSL_WANT_WRITE
#define ESP_TLS_ERR_SSL_WANT_WRITE -0x6880
#endif

class BambuTlsClient : public Client {
  public:
    BambuTlsClient() {}
    ~BambuTlsClient() { stop(); }

    // Parity with WiFiClientSecure; this client only ever runs insecure.
    void setInsecure() { /* no-op: no CA cert is ever configured */ }
    void setConnectTimeout(uint32_t ms) { _timeoutMs = ms; }

    int connect(IPAddress ip, uint16_t port) override {
      char host[16];
      snprintf(host, sizeof(host), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
      return connect(host, port);
    }

    int connect(const char *host, uint16_t port) override {
      stop();

      esp_tls_cfg_t cfg = {};
      cfg.skip_common_name = true;     // we do not verify the peer at all
      cfg.non_block        = true;
      cfg.timeout_ms       = (int)_timeoutMs;

      _tls = esp_tls_init();
      if (!_tls) return 0;

      int ret = esp_tls_conn_new_sync(host, (int)strlen(host), (int)port, &cfg, _tls);
      if (ret != 1) {
        esp_tls_conn_destroy(_tls);
        _tls = nullptr;
        return 0;
      }

      // Force the underlying socket non-blocking so reads never hang.
      int fd = -1;
      if (esp_tls_get_conn_sockfd(_tls, &fd) == ESP_OK && fd >= 0) {
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl != -1) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
      }

      _rxHead = _rxTail = 0;
      _peerClosed = false;
      return 1;
    }

    using Print::write;   // keep Print::write(const char*) / write(const char*, size_t) visible

    size_t write(uint8_t b) override { return write(&b, 1); }

    size_t write(const uint8_t *buf, size_t size) override {
      if (!_tls || !buf || size == 0) return 0;
      size_t sent = 0;
      uint32_t start = millis();
      while (sent < size) {
        int ret = esp_tls_conn_write(_tls, buf + sent, size - sent);
        if (ret > 0) { sent += (size_t)ret; start = millis(); continue; }
        if (ret == 0) continue;
        if (ret == ESP_TLS_ERR_SSL_WANT_WRITE || ret == ESP_TLS_ERR_SSL_WANT_READ) {
          if (millis() - start > 3000) break;   // stop blocking WLED's loop
          delay(2);
          continue;
        }
        _peerClosed = true;                     // hard error
        break;
      }
      return sent;
    }

    int available() override {
      fillRx();
      return (int)(_rxTail - _rxHead);
    }

    int read() override {
      if (_rxHead == _rxTail) fillRx();
      if (_rxHead == _rxTail) return -1;
      return _rxBuf[_rxHead++];
    }

    int read(uint8_t *buf, size_t size) override {
      size_t n = 0;
      while (n < size) {
        if (_rxHead == _rxTail) fillRx();
        if (_rxHead == _rxTail) break;
        buf[n++] = _rxBuf[_rxHead++];
      }
      return n ? (int)n : -1;
    }

    int peek() override {
      if (_rxHead == _rxTail) fillRx();
      if (_rxHead == _rxTail) return -1;
      return _rxBuf[_rxHead];
    }

    void flush() override { /* esp_tls has no user-space TX buffer to flush */ }

    void stop() override {
      if (_tls) { esp_tls_conn_destroy(_tls); _tls = nullptr; }
      _rxHead = _rxTail = 0;
      _peerClosed = false;
    }

    uint8_t connected() override {
      // Still "connected" while buffered RX data remains to be drained.
      if (_rxHead != _rxTail) return 1;
      return (_tls && !_peerClosed) ? 1 : 0;
    }

    operator bool() override { return connected(); }

  private:
    void fillRx() {
      if (!_tls) return;
      if (_rxHead == _rxTail) { _rxHead = _rxTail = 0; }
      if (_rxTail == sizeof(_rxBuf)) {
        if (_rxHead == 0) return;               // buffer full
        memmove(_rxBuf, _rxBuf + _rxHead, _rxTail - _rxHead);
        _rxTail -= _rxHead;
        _rxHead = 0;
      }
      int ret = esp_tls_conn_read(_tls, _rxBuf + _rxTail, sizeof(_rxBuf) - _rxTail);
      if (ret > 0) {
        _rxTail += (size_t)ret;
      } else if (ret == 0) {
        _peerClosed = true;                     // peer closed the connection
      } else if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
        // nothing available right now
      } else {
        _peerClosed = true;                     // hard error
      }
    }

    esp_tls_t *_tls = nullptr;
    uint32_t   _timeoutMs = 4000;
    bool       _peerClosed = false;
    uint8_t    _rxBuf[512];
    size_t     _rxHead = 0;
    size_t     _rxTail = 0;
};
