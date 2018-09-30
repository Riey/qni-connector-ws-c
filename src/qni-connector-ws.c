#include "qni-connector-ws.h"
#include "impls/handshake-util.h"

#include <sys/socket.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <wslay/wslay.h>

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct Session
{
    int fd;
    QniConnectorContext *ctx;
    wslay_event_context_ptr event_ctx;
    char clientAddr[INET_ADDRSTRLEN];
};

struct HandlingArgs
{
    struct Session session;
    int epoll_size;
};

const int CONST_TRUE = 1;

int create_listen_socket(const char *host, in_port_t port)
{
    int sfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sfd == -1)
    {
        perror("socket");
        goto END;
    }

    if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &CONST_TRUE, sizeof(CONST_TRUE)) < 0)
    {
        perror("setsockopt");
        goto END_CLEAR_SOCK;
    }

    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(host);

    addr.sin_port = htons(port);

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        perror("bind");
        goto END_CLEAR_SOCK;
    }

    if (listen(sfd, 20) == -1)
    {
        perror("listen");
        goto END_CLEAR_SOCK;
    }

    printf("Now listen %s:%u...\n", host, port);

    goto END;

END_CLEAR_SOCK:
    close(sfd);
    sfd = -1;
END:
    return sfd;
}

ssize_t recv_callback(wslay_event_context_ptr ctx,
                      uint8_t *buf, size_t len,
                      int flags, void *user_data)
{
    struct Session *session = user_data;

    ssize_t ret = recv(session->fd, buf, len, 0);

    if (ret == -1)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        }
        else
        {
            wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        }

        return -1;
    }
    else if (ret == 0)
    {
        wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        return -1;
    }

    return ret;
}

ssize_t send_callback(wslay_event_context_ptr ctx,
                      const uint8_t *data, size_t len, int flags,
                      void *user_data)
{
    struct Session *session = user_data;
    ssize_t ret;
    int sflags;

    if (flags & WSLAY_MSG_MORE)
    {
        sflags |= MSG_MORE;
    }

    ret = send(session->fd, data, len, sflags);

    if (ret == -1)
    {
        if (errno == EAGAIN)
        {
            wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        }
        else
        {
            wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        }
    }

    return ret;
}

void on_msg_recv_callback(wslay_event_context_ptr ctx,
                          const struct wslay_event_on_msg_recv_arg *arg,
                          void *user_data)
{
    if (!wslay_is_ctrl_frame(arg->opcode))
    {
        struct Session *session = user_data;

        struct wslay_event_msg msgarg;

        Qni__Api__ConsoleMessage *msg = qni__api__console_message__unpack(
            NULL,
            arg->msg_length,
            arg->msg);

        if (!msg)
        {
            wslay_event_set_error(ctx, WSLAY_ERR_INVALID_ARGUMENT);
            return;
        }

        switch (msg->data_case)
        {
        case QNI__API__CONSOLE_MESSAGE__DATA_REQ:
        {
            msgarg.msg_length = qni_connector_context_process_request(session->ctx, msg->req, (void **)&msgarg.msg);

            if (msgarg.msg_length == 0)
                break;

            msgarg.opcode = WSLAY_BINARY_FRAME;
            wslay_event_queue_msg(ctx, &msgarg);

            qni_free((void *)msgarg.msg);
            break;
        }
        case QNI__API__CONSOLE_MESSAGE__DATA_RES:
        {
            msgarg.msg_length = qni_connector_context_process_response(session->ctx, msg->res, (void **)&msgarg.msg);

            if (msgarg.msg_length == 0)
                break;

            msgarg.opcode = WSLAY_BINARY_FRAME;
            wslay_event_queue_msg(ctx, &msgarg);

            qni_free((void *)msgarg.msg);
            break;
        }
        }

        qni__api__console_message__free_unpacked(msg, NULL);
    }
}

int qni_connector_ws_start(QniConnectorContext *ctx, const char *host, uint16_t port, int epoll_size)
{
    int val = 1;
    int ret = 0;
    int sfd = create_listen_socket(host, (in_port_t)port);

    if (sfd == -1)
    {
        return -1;
    }

    struct epoll_event ev;
    struct epoll_event *events = malloc(sizeof(struct epoll_event) * epoll_size);

    struct wslay_event_callbacks event_callbacks = {
        recv_callback,
        send_callback,
        NULL,
        NULL,
        NULL,
        NULL,
        on_msg_recv_callback};

    int epfd = epoll_create(epoll_size);

    if (epfd <= 0)
    {
        perror("epoll_create");
        ret = -1;
        goto END;
    }

    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
    ev.data.fd = sfd;

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &ev) < 0)
    {
        perror("epoll_ctl");
        ret = -1;
        goto END_CLEAN_EPFD;
    }

    while (1)
    {
        if (qni_console_context_need_exit(ctx->ctx))
        {
            break;
        }

        int res;

        do
        {
            res = epoll_wait(epfd, events, epoll_size, 2000);
        } while (res < 0 && errno == EINTR);

        if (res == -1)
        {
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < res; i++)
        {
            if (events[i].data.fd == sfd)
            {
                struct sockaddr_in cliaddr;
                socklen_t cliaddr_len = sizeof(cliaddr);
                int clifd = accept(sfd, (struct sockaddr *)&cliaddr, &cliaddr_len);

                if (clifd > 0)
                {
                    if (http_handshake(clifd) < 0 ||
                        make_non_block(clifd) < 0 ||
                        setsockopt(clifd, IPPROTO_TCP, TCP_NODELAY, &CONST_TRUE, sizeof(CONST_TRUE)))
                    {
                        perror("handshake");
                        close(clifd);
                    }

                    else
                    {
                        struct Session *session = malloc(sizeof(struct Session));
                        session->ctx = ctx;
                        session->fd = clifd;

                        inet_ntop(AF_INET, &cliaddr, session->clientAddr, cliaddr_len);

                        ev.events = (EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET);
                        ev.data.ptr = session;

                        if (epoll_ctl(epfd, EPOLL_CTL_ADD, clifd, &ev) < 0)
                        {
                            perror("epoll_ctl");
                            close(clifd);
                            free(session);
                        }

                        assert(wslay_event_context_server_init(&session->event_ctx, &event_callbacks, session) == 0);
                    }
                }

                else
                {
                    perror("accept");
                }
            }

            else
            {
                struct Session *session = events[i].data.ptr;

                uint32_t ep_event = events[i].events;

                if (ep_event & EPOLLIN)
                {
                    if (wslay_event_recv(session->event_ctx) != 0)
                    {
                        goto END_SESSION;
                    }
                }

                if (ep_event & EPOLLOUT)
                {
                    if (wslay_event_send(session->event_ctx) != 0)
                    {
                        goto END_SESSION;
                    }
                }

                if (ep_event & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
                {
                    goto END_SESSION;
                }

                ev.events = (EPOLLRDHUP | EPOLLET);

                if (wslay_event_want_read(session->event_ctx))
                {
                    ev.events |= EPOLLIN;
                }

                if (wslay_event_want_write(session->event_ctx))
                {
                    ev.events |= EPOLLOUT;
                }

                if (ev.events == (EPOLLRDHUP | EPOLLET))
                {
                    goto END_SESSION;
                }

                ev.data.ptr = session;

                if (epoll_ctl(epfd, EPOLL_CTL_ADD, session->fd, &ev) != -1)
                {
                    perror("epoll_ctl_add");
                    goto END_SESSION;
                }

                continue;

            END_SESSION:
                shutdown(session->fd, SHUT_RDWR);
                close(session->fd);
                wslay_event_context_free(session->event_ctx);
                free(session);
            }
        }
    }

END_CLEAN_EPFD:
    close(epfd);

END:
    free(events);

    return ret;
}
