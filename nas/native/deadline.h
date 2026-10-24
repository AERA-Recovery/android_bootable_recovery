// SPDX-License-Identifier: Apache-2.0
#ifndef AERA_NETFS_DEADLINE_H
#define AERA_NETFS_DEADLINE_H
#include <pthread.h>
#include <stdint.h>
#include <time.h>

struct nf_deadline {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t thread;
    int socket, stopping, expired, started;
    unsigned timeout_ms;
    uint64_t received, acknowledged;
    struct timespec progress;
};
int nf_deadline_init(struct nf_deadline *);
int nf_deadline_begin(struct nf_deadline *, int socket, unsigned timeout_ms);
int nf_deadline_end(struct nf_deadline *);
void nf_deadline_destroy(struct nf_deadline *);
#endif
