#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>

#define MAXEVENTS 4096

static int
make_socket_non_blocking (int sfd)
{
    int flags, s;

    flags = fcntl (sfd, F_GETFL, 0);
    if (flags == -1)
    {
        perror ("fcntl");
        return -1;
    }

    flags |= O_NONBLOCK;
    s = fcntl (sfd, F_SETFL, flags);
    if (s == -1)
    {
        perror ("fcntl");
        return -1;
    }

    return 0;
}

static int
create_and_bind (char *port)
{
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int s, sfd;

    memset (&hints, 0, sizeof (struct addrinfo));
    hints.ai_family = AF_UNSPEC;     /* Return IPv4 and IPv6 choices */
    hints.ai_socktype = SOCK_STREAM; /* We want a TCP socket */
    hints.ai_flags = AI_PASSIVE;     /* All interfaces */

    s = getaddrinfo (NULL, port, &hints, &result);
    if (s != 0)
    {
        fprintf (stderr, "getaddrinfo: %s\n", gai_strerror (s));
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next)
    {
        sfd = socket (rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        int opt = 1;
        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (sfd == -1)
            continue;

        s = bind (sfd, rp->ai_addr, rp->ai_addrlen);
        if (s == 0)
        {
            /* We managed to bind successfully! */
            break;
        }

        close (sfd);
    }

    if (rp == NULL)
    {
        fprintf (stderr, "Could not bind\n");
        return -1;
    }

    freeaddrinfo (result);

    return sfd;
}

int write_all(int fd, char* buf, int n) {
    int done_write = 0;
    int total_bytes_write = 0;
    while (!done_write && total_bytes_write != n) {
        int bytes_write = write(fd, buf + total_bytes_write, n - total_bytes_write);
// 	printf("bytes_write: %d\n", bytes_write);
        if (bytes_write <= 0) {
            done_write = 1;
            return -1;
        } else {
            total_bytes_write += bytes_write;
        }
    }
    return 0;
}

#define header_404 "HTTP 404 Not found\nServer: myserver/1.0\nContent-Type: text/html\n\n<h1>not found</h1>"
#define header_200 "HTTP 200 OK\nServer: myserver/1.0\nContent-Type: text/html\n\n"

int sendfile(char *filename, int sock) {
    char fullname[256];
    char *prefix = "/var/www/";
    strcpy(fullname, prefix);
    strcpy(fullname + strlen(prefix), filename);
    int filefd = open(fullname, O_RDONLY);
    char *header = header_200;

    if (filefd < 0) {
        header = header_404;
        write_all(sock, header, strlen(header));
        return -1;
    }

    write_all(sock, header, strlen(header));
    while (1) {
        int done = 0;
        char buf[4096];
        int bytes_read = read(filefd, buf, sizeof(buf));
        if (bytes_read == -1)
        {
            /* If errno == EAGAIN, that means we have read all
               data. So go back to the main loop. */
            if (errno != EAGAIN)
            {
                perror ("read");
                done = 1;
            }
            break;
        }
        else if (bytes_read == 0)
        {
            /* End of file. The remote has closed the
               connection. */
            done = 1;
            break;
        } else if (bytes_read > 0) {
            int done_write = 0;
            int result = write_all(sock, buf, bytes_read);
	    if (result < 0) {
	      done = 1;
	    }
        }
    }
    close(filefd);
}

int
main (int argc, char *argv[])
{
    int sfd, s;
    int efd;
    struct epoll_event event;
    struct epoll_event *events;

//     if (argc != 2)
//     {
//         fprintf (stderr, "Usage: %s [port]\n", argv[0]);
//         exit (EXIT_FAILURE);
//     }

    sfd = create_and_bind (argv[1]);
    if (sfd == -1)
        abort ();

    s = make_socket_non_blocking (sfd);
    if (s == -1)
        abort ();

    s = listen (sfd, SOMAXCONN);
    if (s == -1)
    {
        perror ("listen");
        abort ();
    }

    efd = epoll_create1 (0);
    if (efd == -1)
    {
        perror ("epoll_create");
        abort ();
    }

    event.data.fd = sfd;
    event.events = EPOLLIN | EPOLLET;
    s = epoll_ctl (efd, EPOLL_CTL_ADD, sfd, &event);
    if (s == -1)
    {
        perror ("epoll_ctl");
        abort ();
    }

    /* Buffer where events are returned */
    events = calloc (MAXEVENTS, sizeof event);

    /* The event loop */
    while (1)
    {
        int n, i;

        n = epoll_wait (efd, events, MAXEVENTS, -1);
        for (i = 0; i < n; i++)
        {
            if ((events[i].events & EPOLLERR) ||
                    (events[i].events & EPOLLHUP) ||
                    (!(events[i].events & EPOLLIN)))
            {
                /* An error has occured on this fd, or the socket is not
                   ready for reading (why were we notified then?) */
                fprintf (stderr, "epoll error\n");
                close (events[i].data.fd);
                continue;
            }

            else if (sfd == events[i].data.fd)
            {
                /* We have a notification on the listening socket, which
                   means one or more incoming connections. */
                while (1)
                {
                    struct sockaddr in_addr;
                    socklen_t in_len;
                    int infd;
                    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

                    in_len = sizeof in_addr;
                    infd = accept (sfd, &in_addr, &in_len);
                    if (infd == -1)
                    {
                        if ((errno == EAGAIN) ||
                                (errno == EWOULDBLOCK))
                        {
                            /* We have processed all incoming
                               connections. */
                            break;
                        }
                        else
                        {
                            perror ("accept");
                            break;
                        }
                    }

                    s = getnameinfo (&in_addr, in_len,
                                     hbuf, sizeof hbuf,
                                     sbuf, sizeof sbuf,
                                     NI_NUMERICHOST | NI_NUMERICSERV);
                    if (s == 0)
                    {
//                         printf("Accepted connection on descriptor %d "
//                                "(host=%s, port=%s)\n", infd, hbuf, sbuf);
                    }

                    /* Make the incoming socket non-blocking and add it to the
                       list of fds to monitor. */
                    s = make_socket_non_blocking (infd);
                    if (s == -1)
                        abort ();

                    event.data.fd = infd;
                    event.events = EPOLLIN | EPOLLET;
                    s = epoll_ctl (efd, EPOLL_CTL_ADD, infd, &event);
                    if (s == -1)
                    {
                        perror ("epoll_ctl");
                        abort ();
                    }
                }
                continue;
            }
            else
            {
                /* We have data on the fd waiting to be read. Read and
                   display it. We must read whatever data is available
                   completely, as we are running in edge-triggered mode
                   and won't get a notification again for the same
                   data. */
                int done = 0;

                char buf[4096];
                ssize_t totalcount = 0;
                while (1)
                {
                    ssize_t count;

                    count = read (events[i].data.fd, buf + totalcount, sizeof buf - totalcount);
                    if (count == -1)
                    {
                        /* If errno == EAGAIN, that means we have read all
                           data. So go back to the main loop. */
                        if (errno != EAGAIN)
                        {
                            perror ("read");
                            done = 1;
                        }
                        break;
                    }
                    else if (count == 0)
                    {
                        /* End of file. The remote has closed the
                           connection. */
                        done = 1;
                        break;
                    } else if (count > 0) {

                        totalcount += count;
                    }

                    /* Write the buffer to standard output */
                    s = write (1, buf + totalcount - count, count);
                    if (s == -1)
                    {
                        perror ("write");
                        abort ();
                    }
                }

                int error = 0;
                if (totalcount > 10) {
                    // get GET info
                    if (strncmp(buf, "GET", 3) == 0) {
                        // get first line
                        int n_loc = (int)strchr(buf, '\n');
                        int space_loc = (int)strchr(buf + 4, ' ');
                        if (n_loc > space_loc) {
                            char path[255];
                            int len = space_loc - (int)buf - 4;
                            strncpy(path, buf+4, len);
                            path[len] = 0;
//                             printf("path: %s\n", path);
//                             printf("\n");
                            int result = sendfile(path, events[i].data.fd);
                            done = 1;
                        } else {
                            error = 400;
                        }

                    } else {
                        error = 401;
                    }
                }


                if (done)
                {
//                     printf ("Closed connection on descriptor %d\n",
//                             events[i].data.fd);

                    /* Closing the descriptor will make epoll remove it
                       from the set of descriptors which are monitored. */
                    close (events[i].data.fd);
                }
            }
        }
    }

    free (events);

    close (sfd);

    return EXIT_SUCCESS;
}