/* udp_proxy.c
 *
 * Copyright (C) 2006-2013 wolfSSL Inc.
 *
 * This file is part of udp_proxy.
 *
 * udp_proxy is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * udp_proxy is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

/* udp_proxy.c 
 * gcc -Wall udp_proxy.c -o udp_proxy -levent 
 * ./udp_proxy -p 12345 -s 127.0.0.1:11111  
 * for use with CyaSSL example server with client talking to proxy on port 12345
 * ./examples/server/server -u
 * ./examples/client/client -u -p 12345
*/

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <sysexits.h>
#include <assert.h>

#include <event2/event.h>


/* datagram msg size */
#define MSG_SIZE 1500 

struct event_base* base;               /* main base */
struct sockaddr_in proxy, server;      /* proxy address and server address */
int serverLen = sizeof(server);        /* server address len */
int dropPacket  = 0;                   /* dropping packet interval */
int delayPacket = 0;                   /* delay packet interval */

typedef struct proxy_ctx {
    int  clientFd;       /* from client to proxy, downstream */
    int  serverFd;       /* form server to proxy, upstream   */
} proxy_ctx;


typedef struct delay_packet {
    char           msg[MSG_SIZE];   /* msg to delay */
    int            msgLen;          /* msg size */
    int            sendCount;       /* msg count for when to stop the delay */
    int            peerFd;          /* fd to later send on */
    proxy_ctx*     ctx;             /* associated context */
} delay_packet;

delay_packet  tmpDelay;            /* our tmp holder */
delay_packet* currDelay = NULL;    /* current packet to delay */


static char* serverSide = "server";
static char* clientSide = "client";


static char* GetRecordType(const char* msg)
{
    if (msg[0] == 0x16) {
        if (msg[13] == 0x01)
            return "Client Hello";
        else if (msg[13] == 0x00)
            return "Hello Request";
        else if (msg[13] == 0x03)
            return "Hello Verify Request";
        else if (msg[13] == 0x04)
            return "Session Ticket";
        else if (msg[13] == 0x0b)
            return "Certificate";
        else if (msg[13] == 0x0d)
            return "Certificate Request";
        else if (msg[13] == 0x0f)
            return "Certificate Verify";
        else if (msg[13] == 0x02)
            return "Server Hello";
        else if (msg[13] == 0x0e)
            return "Server Hello Done";
        else if (msg[13] == 0x10)
            return "Client Key Exchange";
        else if (msg[13] == 0x0c)
            return "Server Key Exchange";
        else
            return "Encrypted Handshake Message";
    }
    else if (msg[0] == 0x14)
        return "Change Cipher Spec";
    else if (msg[0] == 0x17)
        return "Application Data";
    else if (msg[0] == 0x15)
        return "Alert";

    return "Unknown";
}


/* msg callback, send along to peer or do manipulation */
static void Msg(evutil_socket_t fd, short which, void* arg)
{
    static int msgCount = 0;

    char       msg[MSG_SIZE];
    proxy_ctx* ctx = (proxy_ctx*)arg;
    int        ret = recv(fd, msg, MSG_SIZE, 0);

    if (ret == 0)
        printf("read 0\n");
    else if (ret < 0)
        printf("read < 0\n");
    else {
        int peerFd;
        char* side;   /* from message side */

        if (ctx->serverFd == fd) {
            peerFd = ctx->clientFd;
            side   = serverSide;
        }
        else {
            peerFd = ctx->serverFd;
            side   = clientSide;
        }

        printf("got %s from %s\n", GetRecordType(msg), side);

        msgCount++;

        /* is it now time to send along delayed packet */
        if (delayPacket && currDelay && currDelay->sendCount == msgCount) {
            printf("*** sending on delayed packet\n");
            send(currDelay->peerFd, currDelay->msg, currDelay->msgLen, 0);
            currDelay = NULL;
        }

        /* should we delay the current packet */
        if (delayPacket && (msgCount % delayPacket) == 0) {
            printf("*** but delaying this packet\n");
            if (currDelay == NULL)
               currDelay = &tmpDelay;
            else {
               printf("*** oops, still have a packet in delay\n");
               assert(0);
            }
            memcpy(currDelay->msg, msg, ret);
            currDelay->msgLen = ret;
            currDelay->sendCount = msgCount + delayPacket;
            currDelay->peerFd = peerFd;
            currDelay->ctx = ctx;
            return;
        }

        /* should we drop current packet altogether */
        if (dropPacket && (msgCount % dropPacket) == 0) {
            printf("*** but dropping this packet\n");
            return;
        }

        /* forward along */
        send(peerFd, msg, ret, 0);
    }
}


/* new client callback */
static void newClient(evutil_socket_t fd, short which, void* arg)
{
    int ret, on = 1;
    struct sockaddr_in client;
    socklen_t len = sizeof(client);
    char msg[MSG_SIZE];
    int  msgLen;
    struct event* cliEvent;
    struct event* srvEvent;

    proxy_ctx* ctx = (proxy_ctx*)malloc(sizeof(proxy_ctx));
    if (ctx == NULL) {
        perror("malloc failed");
        exit(EXIT_FAILURE);
    }

    /* let's 'connect' to client so main loop doesn't here about this
       'connection' again, also allows pairing with upStream 'connect' */
    msgLen = recvfrom(fd, msg, MSG_SIZE, 0, (struct sockaddr*)&client, &len);
    printf("got %s from client, first msg\n", GetRecordType(msg));
    ctx->clientFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx->clientFd < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    setsockopt(ctx->clientFd,SOL_SOCKET,SO_REUSEADDR,&on,(socklen_t)sizeof(on));
#ifdef SO_REUSEPORT
    setsockopt(ctx->clientFd,SOL_SOCKET,SO_REUSEPORT,&on,(socklen_t)sizeof(on));
#endif

    ret = bind(ctx->clientFd, (struct sockaddr*)&proxy, sizeof(proxy));
    if (ret < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    ret = connect(ctx->clientFd, (struct sockaddr*)&client, len);
    if (ret < 0) {
        perror("connect failed");
        exit(EXIT_FAILURE);
    }

    /* need to set up server socket too */
    ctx->serverFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx->serverFd < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    ret = connect(ctx->serverFd, (struct sockaddr*)&server, sizeof(server));
    if (ret < 0) {
        perror("connect failed");
        exit(EXIT_FAILURE);
    }

    /* client and server both use same Msg relay callback */
    cliEvent = event_new(base, ctx->clientFd, EV_READ|EV_PERSIST, Msg, ctx); 
    if (cliEvent == NULL) {
        perror("event_new failed for cliEvent");
        exit(EXIT_FAILURE);
    }
    event_add(cliEvent, NULL);

    srvEvent = event_new(base, ctx->serverFd, EV_READ|EV_PERSIST, Msg, ctx); 
    if (srvEvent == NULL) {
        perror("event_new failed for srvEvent");
        exit(EXIT_FAILURE);
    }
    event_add(srvEvent, NULL);

    /* send along initial client message */
    ret = send(ctx->serverFd, msg, msgLen, 0);
    if (ret < 0) {
        perror("send failed");
        exit(EXIT_FAILURE);
    }
}


static void Usage(void)
{
    printf("udp_proxy \n");

    printf("-?                  Help, print this usage\n");
    printf("-p <num>            Proxy port to 'listen' on\n");
    printf("-s <server:port>    Server address in dotted decimal:port\n");
    printf("-d <num>            Drop every <num> packet, default 0\n");
    printf("-y <num>            Delay every <num> packet, default 0\n");
}


int main(int argc, char** argv)
{
    int sockfd, ret, ch, on = 1;
    struct event* mainEvent;
    short port = -1;
    char* serverString = NULL;

    while ( (ch = getopt(argc, argv, "?p:s:d:y:")) != -1) {
        switch (ch) {
            case '?' :
                Usage();
                exit(EXIT_SUCCESS);
                break;

            case 'p' :
                port = atoi(optarg);
                break;

            case 'd' :
                dropPacket = atoi(optarg);
                break;

            case 'y' :
                delayPacket = atoi(optarg);
                break;

            case 's' :
                serverString = optarg;
                break;

            default:
                Usage();
                exit(EX_USAGE);
                break;
        }
    }

    if (port == -1) {
        printf("need to set 'listen port'\n");
        Usage();
        exit(EX_USAGE);
    }

    if (serverString == NULL) {
        printf("need to set server address string\n");
        Usage();
        exit(EX_USAGE);
    }

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
    memset(&proxy, 0, sizeof(proxy));
    proxy.sin_family = AF_INET;
    proxy.sin_addr.s_addr = htonl(INADDR_ANY);
    proxy.sin_port = htons(port);

    memset(&server, 0, sizeof(server));
    ret = evutil_parse_sockaddr_port(serverString, (struct sockaddr*)&server,
                                     &serverLen);
    if (ret < 0) {
        perror("parse_sockaddr_port failed");
        exit(EXIT_FAILURE);
    }

    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, (socklen_t)sizeof(on));
#ifdef SO_REUSEPORT
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &on, (socklen_t)sizeof(on));
#endif

    ret = bind(sockfd, (struct sockaddr*)&proxy, sizeof(proxy));
    if (ret < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    base = event_base_new();
    if (base == NULL) {
        perror("event_base_new failed");
        exit(EXIT_FAILURE);
    }

    mainEvent = event_new(base, sockfd, EV_READ|EV_PERSIST, newClient, NULL);
    if (mainEvent == NULL) {
        perror("event_new failed for mainEvent");
        exit(EXIT_FAILURE);
    }
    event_add(mainEvent, NULL);

    event_base_dispatch(base);

    printf("done with dispatching\n");

    return 0;
}