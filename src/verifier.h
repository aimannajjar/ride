#ifndef VERIFIER_H
#define VERIFIER_H
#include "ev.h"
#include "ngtcp2/ngtcp2.h"
#include "ngtcp2/ngtcp2_crypto.h"
#include "openssl/base.h"
#include <linux/time_types.h>
#include <stdint.h>
#include <sys/socket.h>

#define MAX_CONCURRENT_WRITES 4

struct quic_stream {
  int64_t stream_id;
  uint8_t data[256];      // app data to send
  uint8_t writebuf[1452]; // quic packet to send
  size_t datalen;         // length of app data
  size_t nwrite;
};

struct verifier_hash_req {
  ev_io_req_t send_req;
  struct quic_stream stream;
  struct verifier_hash_req *next;
};

struct verifier {
  struct processor *processor;
  ev_loop_t *loop;
  ev_io_req_t conn_req;
  ev_io_req_t recv_req;
  ev_io_req_t timer_req;
  ev_io_req_t *last_timer_req;

  struct __kernel_timespec expiry; // next timeout
  uint8_t readbuf[65535];          // read buffer
  struct verifier_hash_req hash_req_slots[MAX_CONCURRENT_WRITES];

  // ngtcp2
  SSL_CTX *ssl_ctx;
  SSL *ssl;
  ngtcp2_conn *conn;
  ngtcp2_crypto_conn_ref conn_ref;
  ngtcp2_ccerr last_error;

  // socket addresses
  struct sockaddr_in local_addr;
  struct sockaddr_in remote_addr;
  socklen_t remote_addr_len;
  socklen_t local_addr_len;

  int sock_fd;
};

int verifier_init(struct verifier *verifier, struct processor *processor,
                  ev_loop_t *ev);
void verifier_verify_hash(struct verifier *verifier, size_t pathlen,
                          char pathname[pathlen], size_t hashlen,
                          char hash[hashlen]);

#endif
