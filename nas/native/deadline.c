// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include "deadline.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/tcp.h>
#include <stddef.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void sample_progress(struct nf_deadline *d, const struct timespec *now) {
    struct tcp_info info;
    socklen_t length=sizeof(info);
    memset(&info,0,sizeof(info));
    if (getsockopt(d->socket,6,TCP_INFO,&info,&length) ||
        length<offsetof(struct tcp_info,tcpi_bytes_received)+sizeof(info.tcpi_bytes_received)) return;
    if (info.tcpi_bytes_received!=d->received || info.tcpi_bytes_acked!=d->acknowledged) {
        d->received=info.tcpi_bytes_received;d->acknowledged=info.tcpi_bytes_acked;
        d->progress=*now;
    }
}
static void *watch(void *opaque) {
    struct nf_deadline *d=opaque;
    pthread_mutex_lock(&d->mutex);
    while (!d->stopping) {
        if (d->socket<0 || d->expired) {
            pthread_cond_wait(&d->changed,&d->mutex);continue;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC,&now);
        sample_progress(d,&now);
        int64_t idle=(int64_t)(now.tv_sec-d->progress.tv_sec)*1000+
            (now.tv_nsec-d->progress.tv_nsec)/1000000;
        if (idle>=(int64_t)d->timeout_ms) {
            /* Wake the owning protocol thread, never free its context here. */
            shutdown(d->socket,SHUT_RDWR);d->expired=1;continue;
        }
        now.tv_nsec+=100000000;
        if (now.tv_nsec>=1000000000) { ++now.tv_sec;now.tv_nsec-=1000000000; }
        pthread_cond_timedwait(&d->changed,&d->mutex,&now);
    }
    pthread_mutex_unlock(&d->mutex);
    return NULL;
}
int nf_deadline_init(struct nf_deadline *d) {
    memset(d,0,sizeof(*d));d->socket=-1;
    int rc=pthread_mutex_init(&d->mutex,NULL);
    if (rc) return -rc;
    pthread_condattr_t attr;
    rc=pthread_condattr_init(&attr);
    if (rc) { pthread_mutex_destroy(&d->mutex);return -rc; }
    rc=pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);
    if (!rc) rc=pthread_cond_init(&d->changed,&attr);
    pthread_condattr_destroy(&attr);
    if (rc) { pthread_mutex_destroy(&d->mutex);return -rc; }
    rc=pthread_create(&d->thread,NULL,watch,d);
    if (rc) { pthread_cond_destroy(&d->changed);pthread_mutex_destroy(&d->mutex);return -rc; }
    d->started=1;
    return 0;
}
int nf_deadline_begin(struct nf_deadline *d,int socket,unsigned timeout_ms) {
    if (!d->started || socket<0) return -ENOTCONN;
    int fd=fcntl(socket,F_DUPFD_CLOEXEC,0);
    if (fd<0) return -errno;
    pthread_mutex_lock(&d->mutex);
    d->socket=fd;d->timeout_ms=timeout_ms;d->expired=0;
    d->received=0;d->acknowledged=0;
    clock_gettime(CLOCK_MONOTONIC,&d->progress);
    sample_progress(d,&d->progress);
    pthread_cond_signal(&d->changed);
    pthread_mutex_unlock(&d->mutex);
    return 0;
}
int nf_deadline_end(struct nf_deadline *d) {
    pthread_mutex_lock(&d->mutex);
    int rc=d->expired ? -ETIMEDOUT : 0;
    struct pollfd p={.fd=d->socket,.events=POLLRDHUP};
    if (!rc && poll(&p,1,0)>0 && (p.revents&(POLLHUP|POLLERR|POLLRDHUP))) rc=-ENOTCONN;
    if (d->socket>=0) close(d->socket);
    d->socket=-1;
    pthread_cond_signal(&d->changed);
    pthread_mutex_unlock(&d->mutex);
    return rc;
}
void nf_deadline_destroy(struct nf_deadline *d) {
    if (!d->started) return;
    nf_deadline_end(d);
    pthread_mutex_lock(&d->mutex);
    d->stopping=1;pthread_cond_signal(&d->changed);
    pthread_mutex_unlock(&d->mutex);
    pthread_join(d->thread,NULL);
    pthread_cond_destroy(&d->changed);pthread_mutex_destroy(&d->mutex);
    d->started=0;
}
