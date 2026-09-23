#ifndef CLIENT_H
#define CLIENT_H

#include "ride.h"
#include "worker.h"
#include <quiche.h>

struct client {
  // quiche structs
  quiche_conn *quiche_conn;
  quiche_config *quiche_config;

  // socket addresses
  struct sockaddr_storage local_addr;
  struct sockaddr remote_addr;
  socklen_t remote_addr_len;
  socklen_t local_addr_len;

  // next expiry event
  struct timespec expiry;

  // conn/sock id
  uint8_t scid[16];
  size_t stream_id;
  int sock_fd;
};

struct verifier_config client_verifier_config();
void client_init(struct client *client, struct job_generic *init_job);
int client_verify_request(struct client *client,
                          const struct job_verify *job_verify);

#endif // CLIENT_H
