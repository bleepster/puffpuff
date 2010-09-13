/* necessary for getsubopt() on linux                                    */
/* man page for getsubopt(3) suggests using _XOPEN_SOURCE 500            */
/* but doing this breaks event.h code, googling throught mailing         */
/* led me to:                                                            */
/* http://www.linuxsa.org.au/pipermail/linuxsa/2005-February/077172.html */
/* which suggests using _GNU_SOURCE - apparently it works :)             */
#if defined (__linux__)
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <errno.h>

#if defined (DEBUG_SYSLOG)
  #include <syslog.h>
  #define DPRINT_DEBUG LOG_DEBUG
  #define DPRINT_ERROR LOG_ERR
  #define DPRINT(priority, text, args...) syslog(priority, text, ##args)
#elif defined (DEBUG_CONSOLE)
  #define DPRINT_DEBUG stdout
  #define DPRINT_ERROR stderr
  #define DPRINT(file, text, args...) fprintf(file, text, ##args)
#else
  #define DPRINT_DEBUG
  #define DPRINT_ERROR
  #define DPRINT(x,y,z...)
#endif

#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/time.h>
#include <time.h>
#include <event.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <pthread.h>

#define DEF_DELAY 1000000
#define DEF_INSTANCES 1


enum {
    SERVER = 0,
    CLIENT
};


char *const token[] = {
    [SERVER] = "server",
    [CLIENT] = "client",
    NULL
};


typedef struct _connection
{
    struct sockaddr_storage bindAddr;
    struct sockaddr_storage servAddr;
    socklen_t bindAddrSize;
    char *buffer;
    int buf_len;
    int s;
    int ipver;
    int established;
    int transport;
    struct timeval current;
    struct timeval prev;
    time_t delay;
    struct event r_event;
    struct event w_event;
    struct event_base *e_base;
} connection;


typedef struct _client
{
    int estcount;
    connection *cons;
} Client;


typedef struct _event_timeout
{
    struct event e;
    struct timeval tv;
} event_timeout;


/* this function is redundant with server.c, needs merging */
int get_ip_subopt(char **server, char **client, char *arg)
{
    int unknown = 0;
    char *value = NULL;

    while(*arg != '\0' && !unknown) {
        switch(getsubopt(&arg, token, &value)) {
            case SERVER:
                *server = value;
                break;

            case CLIENT:
                *client = value;
                break;

            default:
                unknown = 1;
                break;
        }
    }

    return (unknown);
}


void print_usage(char *cmd)
{
    DPRINT(DPRINT_ERROR,"Usage: %s [parameters]\n", cmd);
    DPRINT(DPRINT_ERROR,"Required parameters:\n");
    DPRINT(DPRINT_ERROR,"\t[%s%s]\n", 
        "-4 server=<IPv4 address>,client=<IPv4 address>",
        " | -6 server=<IPv6 address>,client=<IPv6 address>");
    DPRINT(DPRINT_ERROR,"\t[-p <port number>]\n");
    DPRINT(DPRINT_ERROR,"\t[-t <transport protocol (tcp|udp)>]\n");
    DPRINT(DPRINT_ERROR,"Optional parameters:\n");
    DPRINT(DPRINT_ERROR,"\t[-S <size of data>]\n");
    DPRINT(DPRINT_ERROR,"\t[-i <number of thread instances>]\n");
    DPRINT(DPRINT_ERROR,"\t[-d <delay time>]\n");
    DPRINT(DPRINT_ERROR,"\t[-T <duration (in seconds)>]\n");
    DPRINT(DPRINT_ERROR,"\t[-b <run in background>]\n");
    DPRINT(DPRINT_ERROR,"Defaults:\n");
    DPRINT(DPRINT_ERROR,"\tDelay: 1000000 usec\n");
    DPRINT(DPRINT_ERROR,"\tNumber of instances: 1\n");
    DPRINT(DPRINT_ERROR,"\tProcess will run in the foreground\n");
}


void recv_udp(connection *c)
{
    size_t ret;

    ret = recvfrom(c->s, (void *)c->buffer, c->buf_len, 0,
        (struct sockaddr *)&c->servAddr, &c->bindAddrSize);

    if(ret == -1)
        DPRINT(DPRINT_DEBUG, "[%s] recvfrom() failed!\n", __FUNCTION__);
    else
#if defined (__amd64__)
        DPRINT(DPRINT_DEBUG, "[%s] recevied [%ld] bytes\n",
            __FUNCTION__, ret);
#elif defined (__i386__)
        DPRINT(DPRINT_DEBUG, "[%s] recevied [%d] bytes\n",
            __FUNCTION__, ret);
#endif
}


void recv_tcp(connection *c)
{
    size_t ret;

    ret = recv(c->s, (void *)c->buffer, c->buf_len, 0);

    if(ret == -1)
        DPRINT(DPRINT_DEBUG, "[%s] recv() failed!\n", __FUNCTION__);
    else
#if defined (__amd64__)
        DPRINT(DPRINT_DEBUG, "[%s] recevied [%ld] bytes\n",
            __FUNCTION__, ret);
#elif defined (__i386__)
        DPRINT(DPRINT_DEBUG, "[%s] recevied [%d] bytes\n",
            __FUNCTION__, ret);
#endif
}


void send_udp(connection *c)
{
    size_t ret;

    gettimeofday(&c->current, NULL);
    if(c->current.tv_sec > c->prev.tv_sec ||
        (c->current.tv_usec - c->prev.tv_usec) > c->delay) {
            memcpy(&c->prev, &c->current, sizeof(struct timeval));
            
            ret = sendto(c->s, c->buffer, c->buf_len, 0, 
                      (struct sockaddr *)&c->servAddr, c->bindAddrSize);

            if(ret == -1)
                DPRINT(DPRINT_DEBUG, "[%s] sendto() failed!\n", __FUNCTION__);
            else
#if defined (__amd64__)
                DPRINT(DPRINT_DEBUG, "[%s] sent [%ld] bytes\n",
                    __FUNCTION__, ret);
#elif defined (__i386__)
                DPRINT(DPRINT_DEBUG, "[%s] sent [%d] bytes\n",
                    __FUNCTION__, ret);
#endif
    }
}


void send_tcp(connection *c)
{
    size_t ret;

    gettimeofday(&c->current, NULL);
    if(c->current.tv_sec > c->prev.tv_sec ||
        (c->current.tv_usec - c->prev.tv_usec) > c->delay) {
            memcpy(&c->prev, &c->current, sizeof(struct timeval));

            ret = send(c->s, c->buffer, c->buf_len, 0);

            if(ret == -1)
                DPRINT(DPRINT_DEBUG, "[%s] send() failed!\n", __FUNCTION__);
            else
#if defined (__amd64__)
                DPRINT(DPRINT_DEBUG, "[%s] sent [%ld] bytes\n",
                    __FUNCTION__, ret);
#elif defined (__i386__)
                DPRINT(DPRINT_DEBUG, "[%s] sent [%d] bytes\n",
                    __FUNCTION__, ret);
#endif
    }
}


void cb_rw(int fd, short event, void *args)
{
    connection *c = (connection *)args;

    if(event == EV_READ) {
        if(c->transport == SOCK_STREAM)
            recv_tcp(c);
        else
            recv_udp(c);
    }
   
    if(event == EV_WRITE) {
        if(c->transport == SOCK_STREAM)
            send_tcp(c);
        else
            send_udp(c);
    }
}


int prep_connection(connection *c)
{
    c->s = socket(c->ipver, c->transport, 0);
    if(c->s < 0) {
         DPRINT(DPRINT_ERROR, "[%s] socket() failed!\n", __FUNCTION__);
         return (-1);
    }

    if(bind(c->s, (struct sockaddr*)&c->bindAddr, c->bindAddrSize) < 0) {
        DPRINT(DPRINT_ERROR, "[%s] bind() failed!\n", __FUNCTION__);
        close(c->s);
        return (-1);
    }

    if(c->transport == SOCK_STREAM && connect(c->s,
          (struct sockaddr *)&c->servAddr, c->bindAddrSize) < 0) {
              DPRINT(DPRINT_ERROR, "[%s] connect() failed!\n", __FUNCTION__);
              close(c->s);
              return (-1);
    }

    gettimeofday(&c->prev, NULL);

    return (0);
}


void cb_keyboard_int(int fd, short event, void *arg)
{
    int recv_sz = 0;
    char read_buff[80];
    struct event_base *b = (struct event_base *)arg;
    
    recv_sz = read(fd, (void *)read_buff, sizeof(read_buff));
    if(recv_sz > 0) {
        if(recv_sz == 1 && read_buff[0] == '\n') {
            event_base_loopbreak(b);
        }
    }
}


void cb_timeout(int fd, short event, void *arg)
{
    struct event_base *b = (struct event_base *)arg;
    event_base_loopbreak(b);
}


int main(int argc, char **argv)
{
    char *sip = NULL;
    char *cip = NULL;
    int i, opt;
    int ipver = 0;
    int port = 0;
    int buf_len = 0;
    int delay = DEF_DELAY;
    time_t tm_out = 0;
    int icount = DEF_INSTANCES;
    int len;
    int transport = 0;
    int background = 0;

    struct sockaddr_in sin;
    struct sockaddr_in6 sin6;
    struct event_base *e_base = NULL;
    struct event e_ki;

    event_timeout e_timeout;
    connection c;
    Client cl;
    connection *cons_p;

    /*traffic_gw tg;*/

    while((opt = getopt(argc, argv, "4:6:p:t:S:d:T:i:hb")) != -1)
    {
        switch(opt) {
            case '4':
                ipver = AF_INET;
                if(get_ip_subopt(&sip, &cip, optarg) != 0) {
                    print_usage(argv[0]);
                    return (1);
                }
                break;

            case '6':
                ipver = AF_INET6;
                if(get_ip_subopt(&sip, &cip, optarg) != 0) {
                    print_usage(argv[0]);
                    return (1);
                }
                break;

            case 'p':
                port = (int) strtol(optarg, (char **)NULL, 10);
                break;

            case 'S':
                buf_len = (int) strtol(optarg, (char **)NULL, 10);
                break; 

            case 'd':
                delay = (int) strtol(optarg, (char **)NULL, 10);
                break;

            case 't':
                len = strlen(argv[optind - 1]);

                if(!strncmp(argv[optind - 1], "tcp", len))
                    transport = SOCK_STREAM;
                else if(!strncmp(argv[optind - 1], "udp", len))
                    transport = SOCK_DGRAM;
                break;

            case 'T':
                tm_out = (time_t) strtol(optarg, (char **)NULL, 10);
                break;

            case 'i':
                icount = (int) strtol(optarg, (char **)NULL, 10);
                break;

            case 'b':
                background = 1;
                break;

            case 'h':
                print_usage(argv[0]);
                return (0);
                break;
     
            default:
                print_usage(argv[0]);
                return (1);
                break;
        }
    }

    if(cip == NULL || sip == NULL || port == 0 ||
         buf_len == 0 || transport == 0) {
        DPRINT(DPRINT_ERROR,"parameters are not valid\n");
        print_usage(argv[0]);
        return (1);
    } 

    memset(&c, 0, sizeof(connection));
    c.buffer = NULL;
    c.buf_len = buf_len;
    c.delay = delay;
    c.ipver = ipver;
    c.transport = transport;

    if(c.ipver == AF_INET) {
        memset(&sin, 0, sizeof(struct sockaddr_in));

        sin.sin_family = AF_INET;
        if(inet_pton(AF_INET, sip, (void *)&sin.sin_addr) < 0) 
        {
            DPRINT(DPRINT_ERROR, "[%s] failed to convert [%s] address\n", 
                __FUNCTION__, sip);
            return (1);
        }
        sin.sin_port = htons(port);
        memcpy(&c.servAddr, &sin, sizeof(struct sockaddr_storage));

        if(inet_pton(AF_INET, cip, (void *)&sin.sin_addr) < 0) {
            DPRINT(DPRINT_ERROR, "[%s] failed to convert [%s] address\n", 
                __FUNCTION__, cip);
            return (1);
        }

        sin.sin_port = htons(0);
        memcpy(&c.bindAddr, &sin, sizeof(struct sockaddr_storage));

        c.bindAddrSize = sizeof(struct sockaddr_in);
    }
    else if (c.ipver == AF_INET6) {
        memset(&sin6, 0, sizeof(struct sockaddr_in6));

        sin6.sin6_family = AF_INET6;
        if(inet_pton(AF_INET6, sip, (void *)&sin6.sin6_addr) < 0) {
            DPRINT(DPRINT_ERROR, "[%s] failed to convert [%s] address\n", 
                __FUNCTION__, sip);
            return (1);
        }
        sin6.sin6_port = htons(port);
        memcpy(&c.servAddr, &sin6, sizeof(struct sockaddr_storage));

        if(inet_pton(AF_INET6, cip, (void *)&sin6.sin6_addr) < 0) {
            DPRINT(DPRINT_ERROR, "[%s] failed to convert [%s] address\n", 
                __FUNCTION__, cip);
            return (1);
        }
        sin6.sin6_port = htons(0);
        memcpy(&c.bindAddr, &sin6, sizeof(struct sockaddr_storage));

        c.bindAddrSize = sizeof(struct sockaddr_in6);
    }

    if (background)
        daemon(0,0);

    e_base = event_base_new();
    if(e_base == NULL) {
        DPRINT(DPRINT_ERROR, "[%s] unable to initialize event base\n", 
            __FUNCTION__);
        return (1);
    }

    if(!background) {
    /* initialize keyboard interupt event handler */
        event_set(&e_ki, STDIN_FILENO, (EV_READ | EV_PERSIST), 
            cb_keyboard_int, e_base);
        event_base_set(e_base, &e_ki);
        event_add(&e_ki, NULL);
    }

    /* initialize timeout event handler */
    e_timeout.tv.tv_usec = 0;
    e_timeout.tv.tv_sec = tm_out;
    event_set(&e_timeout.e, -1, 0, cb_timeout, e_base);
    event_base_set(e_base, &e_timeout.e);
    event_add(&e_timeout.e, &e_timeout.tv);

    bzero(&cl, sizeof(Client));

    cl.cons = (connection *) calloc(icount, sizeof(connection));
    cons_p = cl.cons;

    for(i = 0; i < icount; ++i) {
        memcpy(&cons_p[i], &c, sizeof(connection));

        if(prep_connection(&cons_p[i]) < 0) {
            DPRINT(DPRINT_ERROR, "[%s] [%d] connection failed \n",
                __FUNCTION__, i);
        }
        else {
            ++cl.estcount;
            cons_p[i].established = 1;
            cons_p[i].buffer = (char *) malloc(cons_p[i].buf_len);

            event_set(&cons_p[i].r_event, cons_p[i].s,
                (EV_READ | EV_PERSIST), cb_rw, &cons_p[i]);
            event_base_set(e_base, &cons_p[i].r_event);
            event_add(&cons_p[i].r_event, NULL);

            event_set(&cons_p[i].w_event, cons_p[i].s,
                (EV_WRITE | EV_PERSIST), cb_rw, &cons_p[i]);
            event_base_set(e_base, &cons_p[i].w_event);
            event_add(&cons_p[i].w_event, NULL);
        }
    }

    DPRINT(DPRINT_DEBUG, "[%s] %d connections established\n", __FUNCTION__,
        cl.estcount);

    event_base_dispatch(e_base);

    DPRINT(DPRINT_DEBUG, "[%s] cleaning up...\n", __FUNCTION__);

    /* clean up */
    event_del(&e_timeout.e);
    event_del(&e_ki);

    for(i = 0; i < icount; ++i) {
        if(cons_p[i].established) {
            event_del(&cons_p[i].r_event);
            event_del(&cons_p[i].w_event);
            close(cons_p[i].s);
            free(cons_p[i].buffer);
        }
    }

    event_base_free(e_base);
    free(cons_p);
    return (0);
}
