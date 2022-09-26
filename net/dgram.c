/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"

#include "net/net.h"
#include "clients.h"
#include "monitor/monitor.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/option.h"
#include "qemu/sockets.h"
#include "qemu/iov.h"
#include "qemu/main-loop.h"
#include "qemu/cutils.h"

typedef struct NetDgramState {
    NetClientState nc;
    int fd;
    SocketReadState rs;
    struct sockaddr *dgram_dst; /* contains destination iff connectionless */
    bool read_poll;               /* waiting to receive data? */
    bool write_poll;              /* waiting to transmit data? */
} NetDgramState;

static void net_dgram_send_dgram(void *opaque);
static void net_dgram_writable(void *opaque);

static void net_dgram_update_fd_handler(NetDgramState *s)
{
    qemu_set_fd_handler(s->fd,
                        s->read_poll ? net_dgram_send_dgram : NULL,
                        s->write_poll ? net_dgram_writable : NULL,
                        s);
}

static void net_dgram_read_poll(NetDgramState *s, bool enable)
{
    s->read_poll = enable;
    net_dgram_update_fd_handler(s);
}

static void net_dgram_write_poll(NetDgramState *s, bool enable)
{
    s->write_poll = enable;
    net_dgram_update_fd_handler(s);
}

static void net_dgram_writable(void *opaque)
{
    NetDgramState *s = opaque;

    net_dgram_write_poll(s, false);

    qemu_flush_queued_packets(&s->nc);
}

static ssize_t net_dgram_receive_dgram(NetClientState *nc,
                                       const uint8_t *buf, size_t size)
{
    NetDgramState *s = DO_UPCAST(NetDgramState, nc, nc);
    ssize_t ret;

    do {
        if (s->dgram_dst) {
            ret = sendto(s->fd, buf, size, 0, s->dgram_dst,
                         sizeof(struct sockaddr_in));
        } else {
            ret = send(s->fd, buf, size, 0);
        }
    } while (ret == -1 && errno == EINTR);

    if (ret == -1 && errno == EAGAIN) {
        net_dgram_write_poll(s, true);
        return 0;
    }
    return ret;
}

static void net_dgram_send_completed(NetClientState *nc, ssize_t len)
{
    NetDgramState *s = DO_UPCAST(NetDgramState, nc, nc);

    if (!s->read_poll) {
        net_dgram_read_poll(s, true);
    }
}

static void net_dgram_rs_finalize(SocketReadState *rs)
{
    NetDgramState *s = container_of(rs, NetDgramState, rs);

    if (qemu_send_packet_async(&s->nc, rs->buf,
                               rs->packet_len,
                               net_dgram_send_completed) == 0) {
        net_dgram_read_poll(s, false);
    }
}

static void net_dgram_send_dgram(void *opaque)
{
    NetDgramState *s = opaque;
    int size;

    size = recv(s->fd, s->rs.buf, sizeof(s->rs.buf), 0);
    if (size < 0) {
        return;
    }
    if (size == 0) {
        /* end of connection */
        net_dgram_read_poll(s, false);
        net_dgram_write_poll(s, false);
        return;
    }
    if (qemu_send_packet_async(&s->nc, s->rs.buf, size,
                               net_dgram_send_completed) == 0) {
        net_dgram_read_poll(s, false);
    }
}

static int net_dgram_mcast_create(struct sockaddr_in *mcastaddr,
                                  struct in_addr *localaddr,
                                  Error **errp)
{
    struct ip_mreq imr;
    int fd;
    int val, ret;
#ifdef __OpenBSD__
    unsigned char loop;
#else
    int loop;
#endif

    if (!IN_MULTICAST(ntohl(mcastaddr->sin_addr.s_addr))) {
        error_setg(errp, "specified mcastaddr %s (0x%08x) "
                   "does not contain a multicast address",
                   inet_ntoa(mcastaddr->sin_addr),
                   (int)ntohl(mcastaddr->sin_addr.s_addr));
        return -1;
    }

    fd = qemu_socket(PF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        error_setg_errno(errp, errno, "can't create datagram socket");
        return -1;
    }

    /*
     * Allow multiple sockets to bind the same multicast ip and port by setting
     * SO_REUSEADDR. This is the only situation where SO_REUSEADDR should be set
     * on windows. Use socket_set_fast_reuse otherwise as it sets SO_REUSEADDR
     * only on posix systems.
     */
    val = 1;
    ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    if (ret < 0) {
        error_setg_errno(errp, errno, "can't set socket option SO_REUSEADDR");
        goto fail;
    }

    ret = bind(fd, (struct sockaddr *)mcastaddr, sizeof(*mcastaddr));
    if (ret < 0) {
        error_setg_errno(errp, errno, "can't bind ip=%s to socket",
                         inet_ntoa(mcastaddr->sin_addr));
        goto fail;
    }

    /* Add host to multicast group */
    imr.imr_multiaddr = mcastaddr->sin_addr;
    if (localaddr) {
        imr.imr_interface = *localaddr;
    } else {
        imr.imr_interface.s_addr = htonl(INADDR_ANY);
    }

    ret = setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                     &imr, sizeof(struct ip_mreq));
    if (ret < 0) {
        error_setg_errno(errp, errno,
                         "can't add socket to multicast group %s",
                         inet_ntoa(imr.imr_multiaddr));
        goto fail;
    }

    /* Force mcast msgs to loopback (eg. several QEMUs in same host */
    loop = 1;
    ret = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP,
                     &loop, sizeof(loop));
    if (ret < 0) {
        error_setg_errno(errp, errno,
                         "can't force multicast message to loopback");
        goto fail;
    }

    /* If a bind address is given, only send packets from that address */
    if (localaddr != NULL) {
        ret = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF,
                         localaddr, sizeof(*localaddr));
        if (ret < 0) {
            error_setg_errno(errp, errno,
                             "can't set the default network send interface");
            goto fail;
        }
    }

    qemu_socket_set_nonblock(fd);
    return fd;
fail:
    if (fd >= 0) {
        closesocket(fd);
    }
    return -1;
}

static void net_dgram_cleanup(NetClientState *nc)
{
    NetDgramState *s = DO_UPCAST(NetDgramState, nc, nc);
    if (s->fd != -1) {
        net_dgram_read_poll(s, false);
        net_dgram_write_poll(s, false);
        close(s->fd);
        s->fd = -1;
    }
    g_free(s->dgram_dst);
    s->dgram_dst = NULL;
}

static NetClientInfo net_dgram_socket_info = {
    .type = NET_CLIENT_DRIVER_DGRAM,
    .size = sizeof(NetDgramState),
    .receive = net_dgram_receive_dgram,
    .cleanup = net_dgram_cleanup,
};

static NetDgramState *net_dgram_fd_init_dgram(NetClientState *peer,
                                              const char *model,
                                              const char *name,
                                              int fd,
                                              Error **errp)
{
    NetClientState *nc;
    NetDgramState *s;

    nc = qemu_new_net_client(&net_dgram_socket_info, peer, model, name);

    s = DO_UPCAST(NetDgramState, nc, nc);

    s->fd = fd;
    net_socket_rs_init(&s->rs, net_dgram_rs_finalize, false);
    net_dgram_read_poll(s, true);

    return s;
}

static int net_dgram_mcast_init(NetClientState *peer,
                                const char *model,
                                const char *name,
                                SocketAddress *remote,
                                SocketAddress *local,
                                Error **errp)
{
    NetDgramState *s;
    int fd, ret;
    struct sockaddr_in *saddr;
    gchar *info_str;

    if (remote->type != SOCKET_ADDRESS_TYPE_INET) {
        error_setg(errp, "multicast only support inet type");
        return -1;
    }

    saddr = g_new(struct sockaddr_in, 1);
    if (convert_host_port(saddr, remote->u.inet.host, remote->u.inet.port,
                          errp) < 0) {
        g_free(saddr);
        return -1;
    }

    if (!local) {
        fd = net_dgram_mcast_create(saddr, NULL, errp);
        if (fd < 0) {
            g_free(saddr);
            return -1;
        }
        info_str = g_strdup_printf("mcast=%s:%d",
                                   inet_ntoa(saddr->sin_addr),
                                   ntohs(saddr->sin_port));
    } else {
        switch (local->type) {
        case SOCKET_ADDRESS_TYPE_INET: {
            struct in_addr localaddr;

            if (inet_aton(local->u.inet.host, &localaddr) == 0) {
                g_free(saddr);
                error_setg(errp, "localaddr '%s' is not a valid IPv4 address",
                           local->u.inet.host);
                return -1;
            }

            fd = net_dgram_mcast_create(saddr, &localaddr, errp);
            if (fd < 0) {
                g_free(saddr);
                return -1;
            }
            info_str = g_strdup_printf("mcast=%s:%d",
                                       inet_ntoa(saddr->sin_addr),
                                       ntohs(saddr->sin_port));
            break;
        }
        case SOCKET_ADDRESS_TYPE_FD: {
            int newfd;

            fd = monitor_fd_param(monitor_cur(), local->u.fd.str, errp);
            if (fd == -1) {
                g_free(saddr);
                return -1;
            }
            ret = qemu_socket_try_set_nonblock(fd);
            if (ret < 0) {
                g_free(saddr);
                error_setg_errno(errp, -ret, "%s: Can't use file descriptor %d",
                                 name, fd);
                return -1;
            }

            /*
             * fd passed: multicast: "learn" dgram_dst address from bound
             * address and save it. Because this may be "shared" socket from a
             * "master" process, datagrams would be recv() by ONLY ONE process:
             * we must "clone" this dgram socket --jjo
             */

            saddr = g_new(struct sockaddr_in, 1);

            if (convert_host_port(saddr, local->u.inet.host, local->u.inet.port,
                                  errp) < 0) {
                g_free(saddr);
                closesocket(fd);
                return -1;
            }

            /* must be bound */
            if (saddr->sin_addr.s_addr == 0) {
                error_setg(errp, "can't setup multicast destination address");
                g_free(saddr);
                closesocket(fd);
                return -1;
            }
            /* clone dgram socket */
            newfd = net_dgram_mcast_create(saddr, NULL, errp);
            if (newfd < 0) {
                g_free(saddr);
                closesocket(fd);
                return -1;
            }
            /* clone newfd to fd, close newfd */
            dup2(newfd, fd);
            close(newfd);

            info_str = g_strdup_printf("fd=%d (cloned mcast=%s:%d)",
                                       fd, inet_ntoa(saddr->sin_addr),
                                       ntohs(saddr->sin_port));
            break;
        }
        default:
            g_free(saddr);
            error_setg(errp, "only support inet or fd type for local");
            return -1;
        }
    }

    s = net_dgram_fd_init_dgram(peer, model, name, fd, errp);
    if (!s) {
        g_free(saddr);
        return -1;
    }

    g_assert(s->dgram_dst == NULL);
    s->dgram_dst = (struct sockaddr *)saddr;

    pstrcpy(s->nc.info_str, sizeof(s->nc.info_str), info_str);
    g_free(info_str);

    return 0;

}

static int net_dgram_init(NetClientState *peer,
                          const char *model,
                          const char *name,
                          SocketAddress *remote,
                          SocketAddress *local,
                          Error **errp)
{
    NetDgramState *s;
    int fd, ret;
    gchar *info_str;
    struct sockaddr *dgram_dst;

    /* detect multicast address */
    if (remote && remote->type == SOCKET_ADDRESS_TYPE_INET) {
        struct sockaddr_in mcastaddr;

        if (convert_host_port(&mcastaddr, remote->u.inet.host,
                              remote->u.inet.port, errp) < 0) {
            return -1;
        }

        if (IN_MULTICAST(ntohl(mcastaddr.sin_addr.s_addr))) {
            return net_dgram_mcast_init(peer, model, name, remote, local,
                                           errp);
        }
    }

    /* unicast address */
    if (!local) {
        error_setg(errp, "dgram requires local= parameter");
        return -1;
    }

    if (remote) {
        if (local->type == SOCKET_ADDRESS_TYPE_FD) {
            error_setg(errp, "don't set remote with local.fd");
            return -1;
        }
        if (remote->type != local->type) {
            error_setg(errp, "remote and local types must be the same");
            return -1;
        }
    } else {
        if (local->type != SOCKET_ADDRESS_TYPE_FD) {
            error_setg(errp, "type=inet requires remote parameter");
            return -1;
        }
    }

    switch (local->type) {
    case SOCKET_ADDRESS_TYPE_INET: {
        struct sockaddr_in laddr_in, raddr_in;

        if (convert_host_port(&laddr_in, local->u.inet.host, local->u.inet.port,
                              errp) < 0) {
            return -1;
        }

        if (convert_host_port(&raddr_in, remote->u.inet.host,
                              remote->u.inet.port, errp) < 0) {
            return -1;
        }

        fd = qemu_socket(PF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            error_setg_errno(errp, errno, "can't create datagram socket");
            return -1;
        }

        ret = socket_set_fast_reuse(fd);
        if (ret < 0) {
            error_setg_errno(errp, errno,
                             "can't set socket option SO_REUSEADDR");
            closesocket(fd);
            return -1;
        }
        ret = bind(fd, (struct sockaddr *)&laddr_in, sizeof(laddr_in));
        if (ret < 0) {
            error_setg_errno(errp, errno, "can't bind ip=%s to socket",
                             inet_ntoa(laddr_in.sin_addr));
            closesocket(fd);
            return -1;
        }
        qemu_socket_set_nonblock(fd);

        dgram_dst = g_malloc(sizeof(raddr_in));
        memcpy(dgram_dst, &raddr_in, sizeof(raddr_in));

        info_str = g_strdup_printf("udp=%s:%d/%s:%d",
                        inet_ntoa(laddr_in.sin_addr), ntohs(laddr_in.sin_port),
                        inet_ntoa(raddr_in.sin_addr), ntohs(raddr_in.sin_port));

        break;
    }
    case SOCKET_ADDRESS_TYPE_FD: {
        SocketAddress *sa;
        SocketAddressType sa_type;

        fd = monitor_fd_param(monitor_cur(), local->u.fd.str, errp);
        if (fd == -1) {
            return -1;
        }
        ret = qemu_socket_try_set_nonblock(fd);
        if (ret < 0) {
            error_setg_errno(errp, -ret, "%s: Can't use file descriptor %d",
                             name, fd);
            return -1;
        }

        sa = socket_local_address(fd, errp);
        if (sa) {
            sa_type = sa->type;
            qapi_free_SocketAddress(sa);

            info_str = g_strdup_printf("fd=%d %s", fd,
                                       SocketAddressType_str(sa_type));
        } else {
            info_str = g_strdup_printf("fd=%d", fd);
        }
        break;
    }
    default:
        error_setg(errp, "only support inet or fd type for local");
        return -1;
    }

    s = net_dgram_fd_init_dgram(peer, model, name, fd, errp);
    if (!s) {
        return -1;
    }

    pstrcpy(s->nc.info_str, sizeof(s->nc.info_str), info_str);
    g_free(info_str);

    if (remote) {
        g_assert(s->dgram_dst == NULL);
        s->dgram_dst = dgram_dst;
    }
    return 0;
}

int net_init_dgram(const Netdev *netdev, const char *name,
                   NetClientState *peer, Error **errp)
{
    const NetdevDgramOptions *sock;

    assert(netdev->type == NET_CLIENT_DRIVER_DGRAM);
    sock = &netdev->u.dgram;

    return net_dgram_init(peer, "dgram", name, sock->remote, sock->local,
                          errp);
}
