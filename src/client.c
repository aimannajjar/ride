#include <asm-generic/errno.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <quiche.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include <sys/epoll.h>

static pthread_mutex_t mutex;
static struct addrinfo *remote_addr;

#define RIDE_ALPN "\x07ride0.1"
#define RIDE_ALPN_LEN 8

struct client {
  // quiche structs
  quiche_conn *quiche_conn;
  quiche_config *quiche_config;

  // socket addresses
  struct sockaddr_storage local_addr;
  struct sockaddr remote_addr;
  socklen_t remote_addr_len;
  socklen_t local_addr_len;

  // conn/sock id
  uint8_t scid[16];
  int sock_fd;
};

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
  uint8_t out[1350] = {0};
  quiche_send_info info;
  while (true) {
    ssize_t n = quiche_conn_send(client->quiche_conn, out, sizeof out, &info);
    if (n == QUICHE_ERR_DONE)
      return 0;

    if (n < 0) {
      printf("Failed to write packet: %zd\n", n);
      return n;
    }

    struct sockaddr *dst = (struct sockaddr *)&info.to;
    socklen_t len = info.to_len;

    n = sendto(client->sock_fd, out, n, 0, dst, len);
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

  quiche_recv_info info = {.from = (struct sockaddr *)&peer_addr,
                           .from_len = peer_addr_len,
                           .to = (struct sockaddr *)&client->local_addr,
                           .to_len = client->local_addr_len};
  n = quiche_conn_recv(client->quiche_conn, buf, n, &info);
  if (n < 0)
    return n;
  return 0;
}

static int client_new_quiche_conn(struct client *client) {
  // Generate new SCID
  if (getrandom(client->scid, sizeof(client->scid), 0) < 0) {
    perror("getrandom");
    exit(1);
  }

#ifdef USERSPACE_DEBUG
  static const char hexchars[] = "0123456789abcdef";
  printf("My SCID: ");
  for (int i = sizeof(client->scid) - 1; i > 0; i--)
    printf("%c%c", hexchars[client->scid[i] >> 4],
           hexchars[client->scid[i] & 0xf]);
  printf("\n");
#endif

  // create quiche connection
  client->quiche_conn = quiche_connect(
      "127.0.0.1", client->scid, sizeof client->scid,
      (struct sockaddr *)&client->local_addr, client->local_addr_len,
      &client->remote_addr, client->remote_addr_len, client->quiche_config);

  if (!client->quiche_conn) {
    fprintf(stderr, "could not create quiche connection\n");
    return -1;
  }

  return 0;
}

void client_init(struct client *client) {
  client->quiche_conn = NULL;
  client->quiche_config = quiche_config_new(QUICHE_PROTOCOL_VERSION);
  client->local_addr_len = sizeof client->local_addr;

  quiche_config_set_application_protos(client->quiche_config,
                                       (uint8_t *)RIDE_ALPN, RIDE_ALPN_LEN);
}

static int client_connect(struct client *client) {
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
    printf("could not create socket\n");
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
    fprintf(stderr, "could not create quich conn\n");
    return 1;
  }
  return 0;
}

// TODO: (wip) make async, implement draining and timeouts
//        and integrate into worker existing task system
void client_advance(struct client *client) {
  int n, c;
  bool prev_connected;

  if (!client->quiche_conn) {
    printf("Connecting...\n");
    if (client_connect(client)) {
      fprintf(stderr, "could not create quich conn\n");
      return;
    }
  }

  prev_connected = quiche_conn_is_established(client->quiche_conn);
  //TODO: make async and fault tolerante
  c = 10000;
  while (!quiche_conn_is_established(client->quiche_conn) && c--) {
    if ((n = flush_out(client)) < 0) {
      fprintf(stderr, "failed to connect: %d\n", n);
      return;
    }

    if ((n = client_receive(client)) == -EWOULDBLOCK) {
      continue;
    }
  }

  // by this point we should be connected
  if (!quiche_conn_is_established(client->quiche_conn)) {
    printf("connection error\n");
    quiche_conn_free(client->quiche_conn);
    client->quiche_conn = NULL;
    init_addr();
    return;
  }

  if (!prev_connected) {
    printf("Connected!\n");
  }
  // flush_out(client);
  // client_receive(client);
}
