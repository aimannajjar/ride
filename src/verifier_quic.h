#include "log.h"
#include "verifier.h"
#include <assert.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_boringssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <stdint.h>
#include <string.h>

#define RIDE_ALPN "\x07ride0.1"
#define RIDE_ALPN_LEN 8
#define MAX_TIMEOUT 2000

static uint64_t verifier_quic_flush(struct verifier *verifier);

static ngtcp2_conn *verifier_get_conn(ngtcp2_crypto_conn_ref *conn_ref) {
  struct verifier *verifier = conn_ref->user_data;
  return verifier->conn;
}

static uint64_t timestamp(void) {
  struct timespec tp;

  if (clock_gettime(CLOCK_MONOTONIC, &tp) != 0) {
    log_error("clock_gettime: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  return (uint64_t)tp.tv_sec * NGTCP2_SECONDS + (uint64_t)tp.tv_nsec;
}

static int verifier_send_packet(struct verifier *verifier, const uint8_t *data,
                                size_t datalen) {
  struct iovec iov = {
      .iov_base = (uint8_t *)data,
      .iov_len = datalen,
  };
  struct msghdr msg = {0};
  ssize_t nwrite;

  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  do {
    // TODO: make async
    nwrite = sendmsg(verifier->sock_fd, &msg, 0);
  } while (nwrite == -1 && errno == EINTR);

  if (nwrite == -1) {
    log_error("sendmsg: %s\n", strerror(errno));

    return -1;
  }

  return 0;
}

static size_t verifier_prep_vec(struct verifier *verifier,
                                struct quic_stream **stream, int *pfin,
                                ngtcp2_vec *datav, size_t datavcnt) {
  // TODO: this should grab the next pending stream write
  size_t slot_id = 0;
  struct quic_stream *s;

  if (datavcnt == 0) {
    return 0;
  }

  s = &verifier->hash_req_slots[slot_id].stream;
  if (s->stream_id != -1 && s->nwrite < s->datalen) {
    *stream = s;
    *pfin = 1;
    datav->base = s->data + s->nwrite;
    datav->len = s->datalen - s->nwrite;
    return 1;
  } else if (s->stream_id != -1) {
    // done with stream, reset it
    s->stream_id = -1;
    s->datalen = 0;
    s->nwrite = 0;
  }

  *stream = NULL;
  *pfin = 0;
  datav->base = NULL;
  datav->len = 0;

  return 0;
}

static int verifier_flush_out(struct verifier *verifier) {
  struct quic_stream *stream;
  ngtcp2_tstamp ts = timestamp();
  ngtcp2_pkt_info pi;
  ngtcp2_ssize nwrite;
  uint8_t buf[1452];
  ngtcp2_path_storage ps;
  ngtcp2_vec datav;
  size_t datavcnt;
  int64_t stream_id;
  ngtcp2_ssize wdatalen;
  uint32_t flags;
  int fin;

  ngtcp2_path_storage_zero(&ps);

  for (;;) {
    stream = NULL;
    stream_id = -1;

    datavcnt = verifier_prep_vec(verifier, &stream, &fin, &datav, 1);

    if (stream)
      stream_id = stream->stream_id;

    flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
    if (fin) {
      flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
    }

    nwrite = ngtcp2_conn_writev_stream(verifier->conn, &ps.path, &pi, buf,
                                       sizeof(buf), &wdatalen, flags, stream_id,
                                       &datav, datavcnt, ts);
    if (nwrite < 0) {
      switch (nwrite) {
      case NGTCP2_ERR_WRITE_MORE:
        stream->nwrite += (size_t)wdatalen;
        continue;
      default:
        log_error("ngtcp2_conn_writev_stream: %s\n",
                  ngtcp2_strerror((int)nwrite));
        ngtcp2_ccerr_set_liberr(&verifier->last_error, (int)nwrite, NULL, 0);
        return -1;
      }
    }

    if (nwrite == 0) {
      if (stream)
        log_error("wrote %d/%d", stream->nwrite, stream->datalen);
      return 0;
    }

    if (wdatalen > 0)
      stream->nwrite += (size_t)wdatalen;

    if (verifier_send_packet(verifier, buf, (size_t)nwrite) != 0) {
      break;
    }
  }

  return 0;
}

static void rand_cb(uint8_t *dest, size_t destlen,
                    const ngtcp2_rand_ctx *rand_ctx) {
  int rv;
  (void)rand_ctx;

  rv = RAND_bytes(dest, (int)destlen);
  if (rv != 1) {
    assert(0);
    abort();
  }
}

static int get_new_connection_id_cb(ngtcp2_conn *conn, ngtcp2_cid *cid,
                                    ngtcp2_stateless_reset_token *token,
                                    size_t cidlen, void *user_data) {
  (void)conn;
  (void)user_data;

  if (RAND_bytes(cid->data, (int)cidlen) != 1) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  cid->datalen = cidlen;

  if (RAND_bytes(token->data, sizeof(token->data)) != 1) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

// TODO: adapt log_debug from log.h
static void log_printf(void *user_data, const char *fmt, ...) {
#if defined(LOG_DEBUG) || defined(LOG_TRACE)
  va_list ap;
  (void)user_data;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);

  fprintf(stderr, "\n");
#endif
}

static int verifier_quic_get_slot(struct verifier *verifier) {
  int rv;
  int64_t stream_id;
  size_t slot_id;

  // TODO: grab next available slot (will break when making send async)
  slot_id = 0;

  // open a stream
  rv = ngtcp2_conn_open_bidi_stream(verifier->conn, &stream_id, NULL);
  if (rv != 0)
    return rv;

  log_debug("opened new stream_id: %d", stream_id);
  verifier->hash_req_slots[slot_id].stream.stream_id = stream_id;
  return slot_id;
}

static void verifier_quic_slot_writev(struct verifier *verifier, size_t slot_id,
                                      uint8_t *data, size_t data_len) {

  struct quic_stream *stream;
  stream = &verifier->hash_req_slots[slot_id].stream;
  memcpy(stream->data + stream->datalen, data, data_len);
  stream->datalen += data_len;
}

static int verifier_quic_init(struct verifier *verifier,
                              const struct sockaddr_in *remote_addr,
                              socklen_t remote_addrlen,
                              const struct sockaddr_in *local_addr,
                              socklen_t local_addrlen) {
  ngtcp2_path path = {
      .local =
          {
              .addr = (struct sockaddr *)local_addr,
              .addrlen = local_addrlen,
          },
      .remote =
          {
              .addr = (struct sockaddr *)remote_addr,
              .addrlen = remote_addrlen,
          },
  };
  ngtcp2_callbacks callbacks = {
      .client_initial = ngtcp2_crypto_client_initial_cb,
      .recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb,
      .encrypt = ngtcp2_crypto_encrypt_cb,
      .decrypt = ngtcp2_crypto_decrypt_cb,
      .hp_mask = ngtcp2_crypto_hp_mask_cb,
      .recv_retry = ngtcp2_crypto_recv_retry_cb,
      .extend_max_local_streams_bidi = NULL, // extend_max_local_streams_bidi,
      .rand = rand_cb,
      .update_key = ngtcp2_crypto_update_key_cb,
      .delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
      .delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
      .version_negotiation = ngtcp2_crypto_version_negotiation_cb,
      .get_new_connection_id2 = get_new_connection_id_cb,
      .get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb,
  };
  ngtcp2_cid dcid, scid;
  ngtcp2_settings settings;
  ngtcp2_transport_params params;
  int rv;

  dcid.datalen = NGTCP2_MIN_INITIAL_DCIDLEN;
  if (RAND_bytes(dcid.data, (int)dcid.datalen) != 1) {
    log_error("RAND_bytes failed\n");
    return -1;
  }

  scid.datalen = 8;
  if (RAND_bytes(scid.data, (int)scid.datalen) != 1) {
    log_error("RAND_bytes failed\n");
    return -1;
  }

  ngtcp2_settings_default(&settings);

  settings.initial_ts = timestamp();
  settings.log_printf = log_printf;

  ngtcp2_transport_params_default(&params);

  params.initial_max_streams_uni = 3;
  params.initial_max_stream_data_bidi_local = 128 * 1024;
  params.initial_max_data = 1024 * 1024;
  params.max_idle_timeout = 30 * NGTCP2_SECONDS;

  rv = ngtcp2_conn_client_new(&verifier->conn, &dcid, &scid, &path,
                              NGTCP2_PROTO_VER_V1, &callbacks, &settings,
                              &params, NULL, verifier);
  if (rv != 0) {
    log_error("ngtcp2_conn_client_new: %s\n", ngtcp2_strerror(rv));
    return -1;
  }

  ngtcp2_conn_set_tls_native_handle(verifier->conn, verifier->ssl);

  return 0;
}

static int verifier_quic_handle_expiry(struct verifier *verifier) {
  int rv = ngtcp2_conn_handle_expiry(verifier->conn, timestamp());
  if (rv != 0) {
    log_error("ngtcp2_conn_handle_expiry: %s\n", ngtcp2_strerror(rv));
    return -1;
  }

  return 0;
}

static int verifier_quic_read(struct verifier *verifier, size_t len) {
  ngtcp2_pkt_info pi = {0};
  ngtcp2_path path;
  int rv;

  path.local.addrlen = verifier->local_addr_len;
  path.local.addr = (struct sockaddr *)&verifier->local_addr;
  path.remote.addrlen = verifier->remote_addr_len;
  path.remote.addr = (struct sockaddr *)&verifier->remote_addr;

  rv = ngtcp2_conn_read_pkt(verifier->conn, &path, &pi, verifier->readbuf,
                            (size_t)len, timestamp());
  if (rv != 0) {
    log_error("ngtcp2_conn_read_pkt: %s\n", ngtcp2_strerror(rv));
    if (!verifier->last_error.error_code) {
      if (rv == NGTCP2_ERR_CRYPTO) {
        ngtcp2_ccerr_set_tls_alert(&verifier->last_error,
                                   ngtcp2_conn_get_tls_alert2(verifier->conn),
                                   NULL, 0);
      } else {
        ngtcp2_ccerr_set_liberr(&verifier->last_error, rv, NULL, 0);
      }
    }
    return -1;
  }

  return 0;
}

static uint64_t verifier_quic_flush(struct verifier *verifier) {
  ngtcp2_tstamp expiry, now;
  uint64_t timeout;

  if (verifier_flush_out(verifier) != 0) {
    return -1;
  }

  expiry = ngtcp2_conn_get_expiry2(verifier->conn);
  now = timestamp();

  timeout = expiry < now ? MAX_TIMEOUT : (expiry - now);

  return timeout;
}

static int verifier_ssl_init(struct verifier *verifier) {
  verifier->ssl_ctx = SSL_CTX_new(TLS_client_method());
  if (!verifier->ssl_ctx) {
    log_error("SSL_CTX_new: %s", ERR_error_string(ERR_get_error(), NULL));
    exit(1);
  }

  if (ngtcp2_crypto_boringssl_configure_client_context(verifier->ssl_ctx) !=
      0) {
    log_error("ngtcp2_crypto_quictls_configure_client_context failed");
    exit(1);
  }

  verifier->ssl = SSL_new(verifier->ssl_ctx);
  if (!verifier->ssl) {
    log_error("SSL_new: %s\n", ERR_error_string(ERR_get_error(), NULL));
    exit(1);
  }

  SSL_set_app_data(verifier->ssl, &verifier->conn_ref);
  SSL_set_connect_state(verifier->ssl);
  SSL_set_alpn_protos(verifier->ssl, (const unsigned char *)RIDE_ALPN,
                      RIDE_ALPN_LEN);

  return 0;
}
