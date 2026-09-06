#ifndef CLIENT_H
#define CLIENT_H

#include <quiche.h>

struct client {
  quiche_conn *quiche;
  quiche_config *quiche_config;
  struct sockaddr local;
  struct sockaddr peer;
  socklen_t local_len;
  socklen_t peer_len;
  uint8_t scid;
};

void client_init(struct client *client);
void client_advance(struct client *client);

#endif // CLIENT_H
