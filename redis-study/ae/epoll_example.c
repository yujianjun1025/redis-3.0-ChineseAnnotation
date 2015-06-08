#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <stdio.h>
#include <strings.h>

int main(int argc, char* argv[])
{
    /// -------- socket部分 --------
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(38365);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int server_sock_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_sock_fd < 0)
    {
        printf("socket() failed\n");
        return -1;
    }

    if (bind(server_sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0)
    {
        printf("bind() failed\n");
        return -1;
    }

    if (listen(server_sock_fd, 5) < 0)
    {
        printf("listen() failed\n");
        return -1;
    }


    /// -------- epoll部分 --------
    int epoll_fd = epoll_create(1);
    if (epoll_fd < 0)
    {
        printf("epoll_create() failed\n");
        return -1;
    }

    struct epoll_event ee;
    ee.events = 0;
    ee.events |= EPOLLIN;
    ee.data.fd = server_sock_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_sock_fd, &ee) < 0)
    {
        printf("epoll_ctl() failed\n");
        return -1;
    }

    struct epoll_event event_container[10];
    while (1)
    {
        char buf[1024];
        int num_events = epoll_wait(epoll_fd, event_container, 10, 10 * 1000);
        int n = 0;
        for (n = 0; n < num_events; ++n)
        {
            if (event_container[n].data.fd == server_sock_fd)
            {
                printf("new client connected\n");
                struct sockaddr_in client_addr;
                socklen_t len;
                len = sizeof(client_addr);
                int client_fd;
                if ((client_fd = accept(server_sock_fd, (struct sockaddr*)&client_addr, &len)) < 0)
                {
                    printf("accept() failed\n");
                    return -1;
                }
                else
                {
                    printf("--------------------\n");
                    printf("client \nip: %s\nport: %d\n",
                            inet_ntop(AF_INET, &(client_addr.sin_addr), buf, sizeof(buf)),
                            ntohs(client_addr.sin_port));

                    struct epoll_event ee;
                    ee.events = 0;
                    ee.events |= EPOLLIN;
                    ee.data.fd = client_fd;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ee) < 0)
                    {
                        printf("epoll_ctl() failed, add client's fd %d\n", client_fd);
                        return -1;
                    }
                }
            }
            else
            {
                char read_buf[1024];
                bzero(read_buf, sizeof(read_buf));
                int fd = event_container[n].data.fd;
                if (event_container[n].events & EPOLLIN)
                {
                    int read_bytes = read(fd, read_buf, sizeof(read_buf));
                    if (read_bytes > 0)
                    {
                        printf("read:\n");
                        printf("%s", read_buf);
                    }
                    else if (read_bytes == 0)
                    {
                        printf("read EOF\n");
                        struct epoll_event ee;
                        ee.events = 0;
                        ee.events |= EPOLLIN;
                        ee.data.fd = fd;
                        if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &ee) < 0)
                        {
                            printf("epoll_ctl() failed, del client's fd %d\n", fd);
                            return -1;
                        }
                    }
                    else
                    {
                        printf("read fd %d error\n", fd);
                    }
                }

                if (event_container[n].events & EPOLLOUT)
                {

                }

                if (event_container[n].events & EPOLLERR)
                {

                }

                if (event_container[n].events & EPOLLHUP)
                {

                }
            }
        }
    }

    return 0;
}
