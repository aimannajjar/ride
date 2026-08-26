#include "rides.h"

#ifndef QUEUE_H
#define QUEUE_H

void queue_init(void);
int queue_add(struct event *);
int queue_consume(struct event *);

#endif // QUEUE_H
