#include "verifier.h"
#include "ev.h"
#include "log.h"
#include "ngtcp2/ngtcp2.h"
#include "processor.h"
#include "verifier_quic.h"
#include <arpa/inet.h>
#include <assert.h>
#include <liburing/io_uring.h>
#include <netdb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

static pthread_mutex_t mutex;
static struct addrinfo *remote_addr;

#define MAX_DATAGRAM_SIZE 1350
#define MAX_IDLE_TIMEOUT 1000
#define RETRY_BACKOFF 100

void init_addr() {
  pthread_mutex_lock(&mutex);
  if (remote_addr != NULL)
    goto unlock;

  struct addrinfo hints = {0};
  hints.ai_family = AF_INET;
  hints.ai_flags = AI_NUMERICSERV;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;

  if (getaddrinfo("127.0.0.1", "4433", &hints, &remote_addr)) {
    perror("getaddrinfo");
    exit(1);
  }

unlock:
  pthread_mutex_unlock(&mutex);
}

static void on_socket_timeout(ev_io_req_t *req) {
  struct verifier *verifier;
  log_debug("QUIC timeout");

  // TODO: handle timeout / timeout update errors

  verifier = req->data;
  verifier_quic_handle_expiry(verifier);
}

static void on_socket_timeout_update(ev_io_req_t *req) {
  // struct verifier *verifier;
  log_debug("QUIC timeout updated");
  // TODO: handle timeout / timeout update errors
  processor_free_req(req);
}

static void verifier_flush_egress(struct verifier *verifier) {
  ev_io_req_t *req;
  ssize_t timeout_nsec, timeout_sec;

  if ((timeout_nsec = verifier_quic_flush(verifier)) < 0) {
    exit(EXIT_FAILURE);
  }

  timeout_sec = timeout_nsec / NGTCP2_SECONDS;
  timeout_nsec -= timeout_sec * NGTCP2_SECONDS;
  verifier->expiry.tv_sec = timeout_sec;
  verifier->expiry.tv_nsec = timeout_nsec;

  // update if not first time
  req = &verifier->timer_req;
  if (req->opcode == IORING_OP_TIMEOUT) {
    req = processor_get_req(verifier->processor);
    req->opcode = IORING_OP_TIMEOUT_REMOVE;
    req->flags = IORING_TIMEOUT_UPDATE;
    req->addr = (uintptr_t)verifier;
    req->off = (uintptr_t)&verifier->expiry;
    req->callback = on_socket_timeout_update;
  } else {
    req->opcode = IORING_OP_TIMEOUT;
    req->addr = (uintptr_t)&verifier->expiry;
    req->off = 0;
    req->flags = 0;
    req->callback = on_socket_timeout;
    req->data = verifier;
  }
  req->fd = 0; // verifier->sock_fd;
  req->len = 1;
  log_debug("arming quic timer to sec=%ld nsec=%ld", verifier->expiry.tv_sec,
            verifier->expiry.tv_nsec);
  ev_iou_req(verifier->loop, req);
}

static void on_socket_read(ev_io_req_t *req) {
  struct verifier *verifier;

  verifier = req->data;
  if (req->result < 0) {
    log_error("Failed to read: %s", strerror(-req->result));
    exit(1);
  }

  verifier_quic_read(verifier, req->result);

  // re-arm read
  req = &verifier->recv_req;
  ev_iou_req(verifier->loop, req);

  // drain writes
  verifier_flush_egress(verifier);
}

static void on_socket_connect(ev_io_req_t *req) {
  struct verifier *verifier;

  // TODO: attempt next candidate from getaddrinfo
  if (req->result < 0) {
    log_error("Failed to connect: %s", strerror(-req->result));
    exit(1);
  }

  log_debug("Connected");
  verifier = req->data;

  if (verifier_quic_init(verifier, &verifier->remote_addr,
                         verifier->remote_addr_len, &verifier->local_addr,
                         verifier->local_addr_len)) {
    log_error("QUIC initialization failed");
    exit(1);
  }

  verifier->conn_ref.get_conn = verifier_get_conn;
  verifier->conn_ref.user_data = verifier;

  req = &verifier->recv_req;
  req->opcode = IORING_OP_READ;
  req->fd = verifier->sock_fd;
  req->addr = (uintptr_t)verifier->readbuf;
  req->off = 0;
  req->len = sizeof(verifier->readbuf);
  req->flags = 0;
  req->callback = on_socket_read;
  req->data = verifier;
  ev_iou_req(verifier->loop, req);

  verifier_flush_egress(verifier);
}

static void on_socket_create(ev_io_req_t *req) {
  struct verifier *verifier;
  verifier = req->data;

  // TODO: attempt next candidate from getaddrinfo
  if (req->result < 0) {
    log_error("Failed to create socket: %s", strerror(-req->result));
    exit(1);
  }

  verifier->sock_fd = req->result;
  verifier->local_addr_len = sizeof(struct sockaddr_in);
  log_debug("Created socket: %d", verifier->sock_fd);
  if (getsockname(verifier->sock_fd, (struct sockaddr *)&verifier->local_addr,
                  &verifier->local_addr_len) != 0) {
    perror("getsockname");
    exit(1);
  }

  verifier->remote_addr_len = remote_addr->ai_addrlen;
  memcpy(&verifier->remote_addr, remote_addr->ai_addr, remote_addr->ai_addrlen);
  log_debug("Remote Verifier IP: %s (addrlen=%d)",
            inet_ntoa(((struct sockaddr_in *)&verifier->remote_addr)->sin_addr),
            verifier->remote_addr_len);

  req = &verifier->conn_req;
  req->opcode = IORING_OP_CONNECT;
  req->fd = verifier->sock_fd;
  req->addr = (uintptr_t)&verifier->remote_addr;
  req->off = verifier->remote_addr_len;
  req->len = 0;
  req->flags = 0;
  req->callback = on_socket_connect;
  req->data = verifier;
  ev_iou_req(verifier->loop, req);
}

static void verifier_connect(struct verifier *verifier) {
  ev_io_req_t *req;
  req = &verifier->conn_req;
  req->opcode = IORING_OP_SOCKET;
  req->fd = AF_INET;
  req->off = SOCK_DGRAM;
  req->len = IPPROTO_UDP;
  req->flags = 0;
  req->callback = on_socket_create;
  req->data = verifier;
  ev_iou_req(verifier->loop, req);
}

void verifier_verify_hash(struct verifier *verifier, size_t pathlen,
                          char pathname[pathlen], size_t hashlen,
                          char hash[hashlen]) {
  size_t slot_id;
  slot_id = verifier_quic_get_slot(verifier);
  verifier_quic_slot_writev(verifier, slot_id, (uint8_t *)hash, hashlen);
  verifier_quic_slot_writev(verifier, slot_id, (uint8_t *)pathname, pathlen);
  verifier_quic_flush(verifier);
}

int verifier_init(struct verifier *verifier, struct processor *processor,
                  ev_loop_t *ev) {
  size_t i, size;
  log_debug("Initializing verifier client");
  verifier->processor = processor;
  log_debug("Porcessor address: %p", processor);
  verifier->loop = ev;
  verifier->last_timer_req = NULL;
  verifier->timer_req.opcode = 0;

  size = sizeof(verifier->hash_req_slots) / sizeof(verifier->hash_req_slots[0]);
  for (i = 0; i < size; i++) {
    verifier->hash_req_slots[i].stream.stream_id = -1;
    verifier->hash_req_slots[i].next = &verifier->hash_req_slots[i + 1];
  }

  init_addr();
  verifier_ssl_init(verifier);
  memset(&verifier->conn_req, 0, sizeof(verifier->conn_req));
  verifier_connect(verifier);

  return 0;
}
