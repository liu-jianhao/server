#include <iostream>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>  //for htonl() and htons()
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>     //for signal()
#include <pthread.h>
#include <semaphore.h>
#include <list>
#include <errno.h>
#include <time.h>
#include <sstream>
#include <iomanip> //for std::setw()/setfill()
#include <stdlib.h>


#define WORKER_THREAD_NUM   5

#define min(a, b) ((a <= b) ? (a) : (b))

/* 服务器端的socket */
int g_listenfd = 0;
/* 让线程可以修改它 */
int g_epollfd = 0;
/* 线程ID */
pthread_t g_accept_threadid = 0;
pthread_t g_threadid[WORKER_THREAD_NUM] = { 0 };
/* 接受客户的信号量 */
pthread_mutex_t g_accept_mutex = PTHREAD_MUTEX_INITIALIZER;
/* 有新连接的条件变量 */
pthread_cond_t g_accept_cond = PTHREAD_COND_INITIALIZER;
/* 添加、取出客户链表的信号量 */
pthread_mutex_t g_client_mutex = PTHREAD_MUTEX_INITIALIZER;
/* 通知工作线程有客户消息的条件变量 */
pthread_cond_t g_client_cond = PTHREAD_COND_INITIALIZER;


/* 存储连接客户的链表 */
std::list<int> g_clientlist;


bool create_server_listener(const char* ip, short port)
{
    g_listenfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if(g_listenfd == -1)
    {
        return false;
    }

    int on = 1;
    setsockopt(g_listenfd, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));
    setsockopt(g_listenfd, SOL_SOCKET, SO_REUSEPORT, (char *)&on, sizeof(on));

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = inet_addr(ip);
    servaddr.sin_port = htons(port);

    if(bind(g_listenfd, (sockaddr *)&servaddr, sizeof(servaddr)) == -1)
        return false;

    if(listen(g_listenfd, 50) == -1)
        return false;

    g_epollfd = epoll_create(1);
    if(g_epollfd == -1)
        return false;

    struct epoll_event e;
    memset(&e, 0, sizeof(e));
    e.events = EPOLLIN | EPOLLRDHUP;
    e.data.fd = g_listenfd;
    if(epoll_ctl(g_epollfd, EPOLL_CTL_ADD, g_listenfd, &e) == -1)
        return false;

    return true;
}


void *accept_thread_func(void *arg)
{
    while(1)
    {
        pthread_mutex_lock(&g_accept_mutex);
        pthread_cond_wait(&g_accept_cond, &g_accept_mutex);

        struct sockaddr_in clientaddr;
        socklen_t addrlen;
        int newfd = accept(g_listenfd, (struct sockaddr *)&clientaddr, &addrlen);
        pthread_mutex_unlock(&g_accept_mutex);
        if(newfd == -1)
            continue;

        std::cout << "new client connected: " << std::endl;


        /* 将新socket设置为non-blocking */
        int oldflag = fcntl(newfd, F_GETFL, 0);
        int newflag = oldflag | O_NONBLOCK;
        if(fcntl(newfd, F_SETFL, newflag) == -1)
        {
            std::cout << "fcntl error, oldflag = " << oldflag << ", newflag = " << newflag << std::endl;
            continue;
        }

        struct epoll_event e;
        memset(&e, 0, sizeof(e));
        e.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
        e.data.fd = newfd;
        if(epoll_ctl(g_epollfd, EPOLL_CTL_ADD, newfd, &e) == -1)
            perror("epoll_ctl error!\n");
    }
    return NULL;
}

void *worker_thread_func(void *arg)
{
    while(1)
    {
        int clientfd;
        pthread_mutex_lock(&g_client_mutex);
        while(g_clientlist.empty())
            pthread_cond_wait(&g_client_cond, &g_client_mutex);

        clientfd = g_clientlist.front();
        g_clientlist.pop_front();
        pthread_mutex_unlock(&g_client_mutex);

        std::cout << std::endl;


        std::string strclientmsg;
        char buff[256];
        while(1)
        {
            memset(buff, 0, sizeof(buff));
            int nRecv = recv(clientfd, buff, 256, 0);
            if(nRecv == -1)
            {
                if(errno == EWOULDBLOCK)
                    break;
                perror("recv error\n");
            }
            else if(nRecv == 0)
            {
                printf("peer close\n");
                break;
            }

            strclientmsg += buff;
        }


        while(1)
        {
            int nSend = send(clientfd, strclientmsg.c_str(), strclientmsg.length(), 0);
            if(nSend == -1)
            {
                perror("send eror\n");
            }


            std::cout << "send: " << strclientmsg << std::endl;
            strclientmsg.erase(0, nSend);

            if(strclientmsg.empty())
                break;
        }
    }


    return NULL;
}


int main(int argc, char *argv[])
{
    short port = 12345;
    char ip[] = "127.0.0.1";

    if(!create_server_listener(ip, port))
    {
        std::cout << "Unable to create listen server" << std::endl;
    }

    pthread_create(&g_accept_threadid, NULL, accept_thread_func, NULL);
    for(int i = 0; i < WORKER_THREAD_NUM; i++)
    {
        pthread_create(&g_threadid[i], NULL, worker_thread_func, NULL);
    }

    while(1)
    {
        struct epoll_event ev[1024];
        int n = epoll_wait(g_epollfd, ev, 1024, 10);
        if(n == 0)
            continue;
        else if(n < 0)
        {
            std::cout << "epoll_wait error" << std::endl;
            continue;
        }

        int m = min(n, 1024);
        for(int i = 0; i < m; i++)
        {
            /* 有新连接 */
            if(ev[i].data.fd == g_listenfd)
                pthread_cond_signal(&g_accept_cond);
            /* 有数据 */
            else
            {
                pthread_mutex_lock(&g_client_mutex);
                g_clientlist.push_back(ev[i].data.fd);
                pthread_mutex_unlock(&g_client_mutex);
                pthread_cond_signal(&g_client_cond);
            }
        }
    }
    return 0;
}
