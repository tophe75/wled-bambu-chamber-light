#pragma once

/*
 * BambuTlsClient
 * -----------------------------------------------------------------------
 * A tiny Arduino `Client` that does a TLS 1.2 client handshake with NO
 * certificate verification (mbedTLS authmode = VERIFY_NONE), which is how
 * every LAN Bambu integration connects to the printer's self-signed
 * MQTT broker on :8883.
 *
 * Why this exists: WLED's ESP32 Arduino framework ships mbedTLS as
 * crypto-only — the SSL/TLS record + handshake layer is not in any
 * framework archive, and there is no WiFiClientSecure. The CI workflow
 * therefore drops the matching mbedTLS `ssl_*.c` sources into this
 * usermod (they link against the framework's existing libmbedcrypto /
 * libmbedx509), and this header drives them directly.
 *
 * Scope is deliberately tiny: one blocking-bounded handshake, a raw
 * non-blocking lwIP socket, an esp_random()-seeded CTR_DRBG, a small RX
 * buffer. No verification, no client certs, no session resumption.
 */

#include <Arduino.h>
#include <Client.h>
#include <IPAddress.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_system.h"

#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"

#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED -0x004E
#endif
#ifndef MBEDTLS_ERR_NET_RECV_FAILED
#define MBEDTLS_ERR_NET_RECV_FAILED -0x004C
#endif

class BambuTlsClient : public Client {
  public:
    BambuTlsClient() {}
    ~BambuTlsClient() { stop(); }

    // Parity with WiFiClientSecure; this client is always insecure.
    void setInsecure() {}
    void setConnectTimeout(uint32_t ms) { _timeoutMs = ms; }

    int connect(IPAddress ip, uint16_t port) override {
      char host[16];
      snprintf(host, sizeof(host), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
      return connect(host, port);
    }

    int connect(const char *host, uint16_t port) override {
      stop();
      if (!openSocket(host, port)) { stop(); return 0; }

      mbedtls_ssl_init(&_ssl);
      mbedtls_ssl_config_init(&_conf);
      mbedtls_ctr_drbg_init(&_drbg);
      _inited = true;

      if (mbedtls_ctr_drbg_seed(&_drbg, &BambuTlsClient::entropySource, nullptr, nullptr, 0) != 0) { stop(); return 0; }
      if (mbedtls_ssl_config_defaults(&_conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT) != 0) { stop(); return 0; }
      mbedtls_ssl_conf_authmode(&_conf, MBEDTLS_SSL_VERIFY_NONE);   // self-signed printer cert
      mbedtls_ssl_conf_rng(&_conf, mbedtls_ctr_drbg_random, &_drbg);
      if (mbedtls_ssl_setup(&_ssl, &_conf) != 0) { stop(); return 0; }
      mbedtls_ssl_set_hostname(&_ssl, host);   // SNI only; not verified
      mbedtls_ssl_set_bio(&_ssl, this, &BambuTlsClient::bioSend, &BambuTlsClient::bioRecv, nullptr);

      uint32_t deadline = millis() + _timeoutMs;
      int ret;
      while ((ret = mbedtls_ssl_handshake(&_ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) { stop(); return 0; }
        if ((int32_t)(millis() - deadline) >= 0) { stop(); return 0; }
        delay(5);
      }
      _connected = true;
      _rxHead = _rxTail = 0;
      return 1;
    }

    using Print::write;
    size_t write(uint8_t b) override { return write(&b, 1); }

    size_t write(const uint8_t *buf, size_t size) override {
      if (!_connected || !buf || size == 0) return 0;
      size_t off = 0;
      uint32_t deadline = millis() + 3000;
      while (off < size) {
        int ret = mbedtls_ssl_write(&_ssl, buf + off, size - off);
        if (ret > 0) { off += (size_t)ret; deadline = millis() + 3000; continue; }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
          if ((int32_t)(millis() - deadline) >= 0) break;
          delay(2);
          continue;
        }
        _connected = false;
        break;
      }
      return off;
    }

    int available() override { fillRx(); return (int)(_rxTail - _rxHead); }

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

    void flush() override {}

    void stop() override {
      if (_inited) {
        if (_connected) mbedtls_ssl_close_notify(&_ssl);
        mbedtls_ssl_free(&_ssl);
        mbedtls_ssl_config_free(&_conf);
        mbedtls_ctr_drbg_free(&_drbg);
        _inited = false;
      }
      if (_fd >= 0) { ::close(_fd); _fd = -1; }
      _connected = false;
      _rxHead = _rxTail = 0;
    }

    uint8_t connected() override {
      if (_rxHead != _rxTail) return 1;
      return _connected ? 1 : 0;
    }

    operator bool() override { return connected(); }

  private:
    static int entropySource(void *, unsigned char *buf, size_t len) {
      esp_fill_random(buf, len);
      return 0;
    }

    static int bioSend(void *ctx, const unsigned char *buf, size_t len) {
      int fd = static_cast<BambuTlsClient *>(ctx)->_fd;
      int n = ::send(fd, buf, len, 0);
      if (n >= 0) return n;
      if (errno == EWOULDBLOCK || errno == EAGAIN) return MBEDTLS_ERR_SSL_WANT_WRITE;
      return MBEDTLS_ERR_NET_SEND_FAILED;
    }

    static int bioRecv(void *ctx, unsigned char *buf, size_t len) {
      int fd = static_cast<BambuTlsClient *>(ctx)->_fd;
      int n = ::recv(fd, buf, len, 0);
      if (n > 0) return n;
      if (n == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
      if (errno == EWOULDBLOCK || errno == EAGAIN) return MBEDTLS_ERR_SSL_WANT_READ;
      return MBEDTLS_ERR_NET_RECV_FAILED;
    }

    bool openSocket(const char *host, uint16_t port) {
      char portstr[6];
      snprintf(portstr, sizeof(portstr), "%u", port);
      struct addrinfo hints;
      memset(&hints, 0, sizeof(hints));
      hints.ai_family   = AF_INET;
      hints.ai_socktype = SOCK_STREAM;
      struct addrinfo *res = nullptr;
      if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return false;

      _fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
      if (_fd < 0) { freeaddrinfo(res); return false; }

      struct timeval tv;
      tv.tv_sec  = _timeoutMs / 1000;
      tv.tv_usec = (_timeoutMs % 1000) * 1000;
      setsockopt(_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
      setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

      int rc = ::connect(_fd, res->ai_addr, res->ai_addrlen);
      freeaddrinfo(res);
      if (rc != 0) return false;

      int fl = fcntl(_fd, F_GETFL, 0);
      if (fl != -1) fcntl(_fd, F_SETFL, fl | O_NONBLOCK);
      return true;
    }

    void fillRx() {
      if (!_connected) return;
      if (_rxHead == _rxTail) { _rxHead = _rxTail = 0; }
      if (_rxTail == sizeof(_rxBuf)) {
        if (_rxHead == 0) return;
        memmove(_rxBuf, _rxBuf + _rxHead, _rxTail - _rxHead);
        _rxTail -= _rxHead;
        _rxHead = 0;
      }
      int ret = mbedtls_ssl_read(&_ssl, _rxBuf + _rxTail, sizeof(_rxBuf) - _rxTail);
      if (ret > 0) {
        _rxTail += (size_t)ret;
      } else if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        // nothing available right now
      } else {
        _connected = false;   // close-notify or hard error
      }
    }

    mbedtls_ssl_context     _ssl;
    mbedtls_ssl_config      _conf;
    mbedtls_ctr_drbg_context _drbg;
    bool     _inited     = false;
    bool     _connected  = false;
    int      _fd         = -1;
    uint32_t _timeoutMs  = 4000;
    uint8_t  _rxBuf[512];
    size_t   _rxHead = 0;
    size_t   _rxTail = 0;
};
