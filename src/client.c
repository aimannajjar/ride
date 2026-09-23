#include "client.h"
#include "log.h"
#include "ride.h"
#include "worker.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <quiche.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static pthread_mutex_t mutex;
static struct addrinfo *remote_addr;

#define RIDE_ALPN "\x07ride0.1"
#define RIDE_ALPN_LEN 8
#define MAX_DATAGRAM_SIZE 1350
#define MAX_IDLE_TIMEOUT 1000
#define RETRY_BACKOFF 100

struct verifier_config client_verifier_config() {
  return (struct verifier_config){
      .requires_background_job = true,
  };
}

void init_addr() {
  pthread_mutex_lock(&mutex);
  if (remote_addr != NULL)
    goto unlock;

  struct addrinfo hints = {0};
  hints.ai_family = PF_UNSPEC;
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

int flush_out(struct client *client) {
  uint8_t out[MAX_DATAGRAM_SIZE] = {0};
  quiche_send_info info;
  log_trace("flushing egress");
  while (true) {
    ssize_t n = quiche_conn_send(client->quiche_conn, out, sizeof out, &info);
    log_trace("quiche_conn_send: %zd", n);
    if (n == QUICHE_ERR_DONE)
      return 0;

    if (n < 0) {
      log_error("Failed to write packet: %zd", n);
      return n;
    }

    uint8_t dcil, scid, dcid, token, type;
    size_t scid_len, dcid_len, token_len;
    uint32_t version;
    dcil = 0;

    quiche_header_info(out, sizeof out, dcil, &version, &type, &scid, &scid_len,
                       &dcid, &dcid_len, &token, &token_len);

    struct sockaddr *dst = (struct sockaddr *)&info.to;
    socklen_t len = info.to_len;

    n = sendto(client->sock_fd, out, n, 0, dst, len);
    if (n < 0) {
      log_error("Failed to write packet: %zd", n);
      return n;
    }

    log_debug("sent %zd bytes", n);
    return n;
  }
}

int client_receive(struct client *client) {
  struct sockaddr_storage peer_addr = {0};
  unsigned char buf[65535] = {0};
  socklen_t peer_addr_len = sizeof(peer_addr);
  ssize_t n = recvfrom(client->sock_fd, buf, sizeof buf, 0,
                       (struct sockaddr *)&peer_addr, &peer_addr_len);

  if (n < 0) {
    if (errno == EWOULDBLOCK) {
      return -EWOULDBLOCK;
    }
    perror("recvfrom");
    return n;
  }

  log_debug("received %zd bytes.. feed into quiche", n);

  quiche_recv_info info = {.from = (struct sockaddr *)&peer_addr,
                           .from_len = peer_addr_len,
                           .to = (struct sockaddr *)&client->local_addr,
                           .to_len = client->local_addr_len};
  n = quiche_conn_recv(client->quiche_conn, buf, n, &info);
  log_debug("quiche_conn_recv: %zd", n);
  if (n < 0)
    return n;
  return 0;
}

// reads all available data
// returns 0 when done
// negative when errors
int client_process_receive(struct client *client) {
  log_trace("processing receives");
  ssize_t n;
  while ((n = client_receive(client)) != -EWOULDBLOCK) {
    if (n == QUICHE_ERR_DONE)
      return 0;
    else if (n < 0) {
      log_debug("error recv: %zd", n);
    }
    continue;
  }
  log_trace("recieve would block");
  return n;
}

static int client_new_quiche_conn(struct client *client) {
  // Generate new SCID
  if (getrandom(client->scid, sizeof(client->scid), 0) < 0) {
    perror("getrandom");
    exit(1);
  }

  static const char hexchars[] = "0123456789abcdef";
  char scid_str[2 * sizeof(client->scid) + 1];
  size_t n = 0;
  for (int i = 0; i < sizeof(client->scid); i++)
    n += snprintf(scid_str + n, 3, "%c%c", hexchars[client->scid[i] > 4],
                  hexchars[client->scid[i] & 0xf]);

  log_debug("My SCID: %s", scid_str);

  // create quiche connection
  client->quiche_conn = quiche_connect(
      "127.0.0.1", client->scid, sizeof client->scid,
      (struct sockaddr *)&client->local_addr, client->local_addr_len,
      &client->remote_addr, client->remote_addr_len, client->quiche_config);

  if (!client->quiche_conn) {
    log_error("could not create quiche connection");
    return -1;
  }

  log_debug("Quic connection created; sock fd=%d", client->sock_fd);

  return 0;
}

int64_t client_advance(void *arg, int flags, int *out_sock_fd);

void client_init(struct client *client, struct job_generic *background_job) {
  client->quiche_conn = NULL;
  client->quiche_config = quiche_config_new(QUICHE_PROTOCOL_VERSION);
  client->local_addr_len = sizeof client->local_addr;
  client->stream_id = 0;

  quiche_config_set_application_protos(client->quiche_config,
                                       (uint8_t *)RIDE_ALPN, RIDE_ALPN_LEN);
  quiche_config_set_max_idle_timeout(client->quiche_config, MAX_IDLE_TIMEOUT);
  quiche_config_set_max_recv_udp_payload_size(client->quiche_config,
                                              MAX_DATAGRAM_SIZE);
  quiche_config_set_max_send_udp_payload_size(client->quiche_config,
                                              MAX_DATAGRAM_SIZE);
  quiche_config_set_initial_max_data(client->quiche_config, 10000000);
  quiche_config_set_initial_max_stream_data_bidi_local(client->quiche_config,
                                                       10000000);
  quiche_config_set_initial_max_stream_data_uni(client->quiche_config,
                                                10000000);
  quiche_config_set_initial_max_streams_bidi(client->quiche_config, 10000);
  quiche_config_set_initial_max_streams_uni(client->quiche_config, 10000);
  quiche_config_set_disable_active_migration(client->quiche_config, true);
  quiche_config_enable_early_data(client->quiche_config);

  background_job->callback = client_advance;
  background_job->callback_arg = client;
}

static int client_new_connection(struct client *client) {
  int sock_fd = 0;

  // create socket for this client instance
  init_addr();
  client->remote_addr_len = 0;
  for (; remote_addr != NULL; remote_addr = remote_addr->ai_next) {
    if ((sock_fd = socket(remote_addr->ai_family, remote_addr->ai_socktype,
                          remote_addr->ai_protocol)) < 0)

      continue;

    memcpy(&client->remote_addr, remote_addr->ai_addr, remote_addr->ai_addrlen);
    client->remote_addr_len = remote_addr->ai_addrlen;
    client->sock_fd = sock_fd;
    break;
  }

  if (!sock_fd) {
    log_debug("could not create socket");
    exit(1);
  }

  if (getsockname(client->sock_fd, (struct sockaddr *)&client->local_addr,
                  &client->local_addr_len) != 0) {
    perror("getsockname");
    exit(1);
  }

  if (fcntl(client->sock_fd, F_SETFL, O_NONBLOCK)) {
    perror("fcntl: error enabling non-block");
    exit(1);
  }

  if (client_new_quiche_conn(client)) {
    log_error("could not create quich conn");
    return 1;
  }

  return 0;
}

void client_close_connection(struct client *client) {
  quiche_conn_free(client->quiche_conn);
  client->quiche_conn = NULL;
  client->sock_fd = 0;
  // TODO: close and collect old sockets
}

int64_t client_advance(void *arg, int flags, int *out_sock_fd) {
  int64_t timeout;
  size_t proto_len;
  bool was_connected;
  const uint8_t *proto = NULL;
  struct client *client = (struct client *)arg;

  if (client->quiche_conn && quiche_conn_is_closed(client->quiche_conn)) {
    // stale connection
    log_info("Connection closed, reconnecting");
    client_close_connection(client);
    *out_sock_fd = 0;

    // TODO: implement exponential backoff algorithm
    return RETRY_BACKOFF;
  }

  if (!client->quiche_conn) {
    log_debug("Connecting...");
    if (client_new_connection(client)) {
      log_error("could not create quich conn");
      return RETRY_BACKOFF;
    }
    *out_sock_fd = client->sock_fd;
  }

  was_connected = quiche_conn_is_established(client->quiche_conn);

  // Process time out if we've timed out
  if (flags & VBG_TIMEOUT) {
    if (quiche_conn_is_established(client->quiche_conn)) {
      quiche_conn_send_ack_eliciting(client->quiche_conn);
    }
    quiche_conn_on_timeout(client->quiche_conn);
  }

  // process receives
  if (flags & VBG_RECV) {
    client_process_receive(client);
  }

  // process sends
  flush_out(client);

  if (!was_connected && quiche_conn_is_established(client->quiche_conn)) {
    quiche_conn_application_proto(client->quiche_conn, &proto, &proto_len);
    log_info("Connected to remote verifier");
  }

  timeout = quiche_conn_timeout_as_millis(client->quiche_conn);
  timeout = timeout < MAX_IDLE_TIMEOUT / 2 ? timeout : MAX_IDLE_TIMEOUT / 2;
  if (!timeout)
    timeout = 1;
  else if (timeout < 0)
    timeout = RETRY_BACKOFF;

#ifdef DEBUG
  // log loop progress
  quiche_stats stats;
  const uint8_t *trace_id;
  size_t trace_id_size;
  quiche_conn_stats(client->quiche_conn, &stats);
  quiche_conn_trace_id(client->quiche_conn, &trace_id, &trace_id_size);
  log_debug("conn id=\"%.*s\" sock_fd=%d established=%d, closed=%d timeout=%ld",
            (int)trace_id_size, trace_id, client->sock_fd,
            quiche_conn_is_established(client->quiche_conn),
            quiche_conn_is_closed(client->quiche_conn), timeout);

  log_debug("sent bytes=%zd, "
            "stream blocked sends: %zd, stream blocked recvs: %zd",
            stats.sent_bytes, stats.stream_data_blocked_sent_count,
            stats.stream_data_blocked_recv_count);
#endif

  return timeout;
}

int client_verify_request(struct client *client,
                          const struct job_verify *job_verify) {
  if (!client->quiche_conn ||
      !quiche_conn_is_established(client->quiche_conn) ||
      quiche_conn_is_closed(client->quiche_conn)) {
    log_error("could not send verify requst (connection closed): %s",
              job_verify->param.path);
    return -1;
  }
  uint64_t err;
  ssize_t n;

  err = 0;
  if ((n = quiche_conn_stream_send(
           client->quiche_conn, client->stream_id, job_verify->payload,
           sizeof job_verify->payload, true, &err)) < 0 &&
      n != QUICHE_ERR_DONE) {
    log_error("could not send verify requst (ret=%zd, err=%ld): %s", n, err,
              job_verify->param.path);
    return 0;
  }
  log_debug("sent verify requst (ret=%zd): %s", err, job_verify->param.path);

  client->stream_id += 2;

  // TODO: limit stats to debug builds
  quiche_stats stats;
  const uint8_t *trace_id;
  size_t trace_id_size;
  quiche_conn_stats(client->quiche_conn, &stats);
  quiche_conn_trace_id(client->quiche_conn, &trace_id, &trace_id_size);
  log_trace("conn \"%.*s\" stats: sent bytes=%zd, stream blocked sends: "
            "%zd, stream blocke dreceives: %zd",
            (int)trace_id_size, trace_id, stats.sent_bytes,
            stats.stream_data_blocked_sent_count,
            stats.stream_data_blocked_recv_count);

  flush_out(client);
  return 0;
}
