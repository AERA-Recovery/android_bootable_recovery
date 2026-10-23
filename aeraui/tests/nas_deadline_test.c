// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include "../../nas/native/deadline.h"
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
    struct nf_deadline d;
    assert(!nf_deadline_init(&d));
    int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    assert(!nf_deadline_begin(&d,pair[0],150));
    struct pollfd p={.fd=pair[0],.events=POLLIN};
    assert(poll(&p,1,2000)>0);
    char value;
    assert(read(pair[0],&value,1)==0);
    assert(nf_deadline_end(&d)==-ETIMEDOUT);
    close(pair[0]);close(pair[1]);

    int listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!bind(listener,(struct sockaddr *)&address,sizeof(address)));
    socklen_t length=sizeof(address);
    assert(!getsockname(listener,(struct sockaddr *)&address,&length));
    assert(!listen(listener,1));
    int client=socket(AF_INET,SOCK_STREAM,0);assert(client>=0);
    assert(!connect(client,(struct sockaddr *)&address,length));
    int server=accept(listener,NULL,NULL);assert(server>=0);
    assert(!nf_deadline_begin(&d,client,400));
    /* A slow, progressing transfer must outlive the inactivity deadline. */
    for (int i=0;i<12;++i) {
        assert(send(server,"x",1,MSG_NOSIGNAL)==1);
        assert(read(client,&value,1)==1);
        usleep(100000);
    }
    assert(!nf_deadline_end(&d));
    usleep(500000);
    assert(send(server,"y",1,MSG_NOSIGNAL)==1 && read(client,&value,1)==1);
    assert(!nf_deadline_begin(&d,client,1000));
    close(server);
    usleep(50000);
    assert(nf_deadline_end(&d)==-ENOTCONN);
    close(client);close(listener);
    nf_deadline_destroy(&d);
    puts("NAS deadlines: stalled socket wakes, TCP progress survives, disarm and EOF passed.");
    return 0;
}
