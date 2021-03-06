 #include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include "time_wheel_timer.h"

#define FD_LIMIT 65535
#define MAX_EVENT_NUMBER 1024
#define TIMESLOT 1
static int pipefd[2];
/* timer */
static time_wheel timer_lst;
static int epollfd = 0;

int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}
void addfd(int epollfd, int fd)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking( fd );
}
/* signal chuli */
void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], ( char* )&msg, 1, 0);
    errno = save_errno;
}
/* set signal's chuli function */
void addsig( int sig )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ));
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset( &sa.sa_mask);
    assert( sigaction( sig, &sa, NULL ) != -1 );
}
void timer_handler()
{
    timer_lst.tick();
    alarm( TIMESLOT );
}
void cb_func( client_data* user_data )
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0 );
    assert( user_data );
    close( user_data->sockfd );
    printf( "close fd %d\n", user_data->sockfd );
}
int main(int argc, char* argv[])
{
    const char* ip = "192.168.31.59";
    int port = 54321;

    int ret = 0;
    /* create a ipv4 socket address */
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);

    /* create a TCP socket */
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret != -1);

    ret = listen(listenfd, 5);
    assert(ret != -1);

    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create( 5 );
    assert( epollfd != -1 );
    addfd(epollfd, listenfd);

    /* use socketpair create pipe*/
    ret = socketpair( PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( ret != -1 );
    setnonblocking( pipefd[1] );
    addfd( epollfd, pipefd[0] );

    addsig( SIGALRM );
    addsig( SIGTERM );
    bool stop_server = false;

    client_data* users = new client_data[FD_LIMIT];
    bool timeout = false;
    alarm( TIMESLOT );

    while(!stop_server)
    {
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        if( ( number < 0 ) && ( errno != EINTR ))
        {
            printf( "epoll failure\n" );
            break;
        }

        for( int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;
            if( sockfd == listenfd )
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                addfd(epollfd, connfd);
                users[connfd].address = client_address;
                users[connfd].sockfd = connfd;
                /* timer */
                //tw_timer* timer = new tw_timer;
                tw_timer* timer = timer_lst.add_timer(10);
                timer->user_data = &users[connfd];
                timer->cb_func = cb_func;
                //time_t cur = time(NULL);
                //timer->expire = 5 * TIMESLOT;
                users[connfd].timer = timer;
                //timer_lst.add_timer( timer );
            }
            else if( ( sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN ) )
            {
                int sig;
                char signals[1024];
                ret = recv( pipefd[0], signals, sizeof( signals ), 0 );
                if( ret == -1 )
                {
                    continue;
                }
                else if( ret == 0 )
                {
                    continue;
                }
                else
                {
                    for(int i=0; i<ret; ++i)
                    {
                        switch( signals[i])
                        {
                            case SIGALRM:
                            {
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }  
            }
            else if ( events[i].events & EPOLLIN )
            {
                memset( users[sockfd].buf, '\0', BUFFER_SIZE );
                ret = recv(sockfd, users[sockfd].buf, BUFFER_SIZE-1, 0);
                printf("get %d bytes of client data %s from %d\n", ret, users[sockfd].buf, sockfd);
                tw_timer* timer = users[sockfd].timer;
                if( ret < 0 )
                {
                    if(errno != EAGAIN )
                    {
                        cb_func( &users[sockfd]);
                        if( timer )
                        {
                            timer_lst.del_timer( timer );
                        }
                    }
                }
                else if( ret == 0)
                {
                    cb_func(&users[sockfd]);
                    if( timer )
                    {
                        timer_lst.del_timer( timer );
                    }
                }
                else 
                {
                    if(timer)
                    {
                        printf("adjust timer once\n");
                        //timer_lst.adjust_timer( timer );
                        timer_lst.del_timer(timer);
                        tw_timer* timer = timer_lst.add_timer(10);
                        timer->user_data = &users[sockfd];
                        timer->cb_func = cb_func;
                        users[sockfd].timer = timer;
                    }
                }
            }
            else
            {
                /* code */
            }
        }
        if( timeout )
        {
            timer_handler();
            timeout = false;
        }
    }

    close( listenfd );
    close( pipefd[1] );
    close( pipefd[0] );
    delete [] users;
    return 0;

}
