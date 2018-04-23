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

#include "become_daemon.h"
#include <sys/stat.h>


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


/* 决定主线程、accept线程、工作线程是否继续迭代 */
bool g_bStop = false;


void prog_exit(int signo)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGKILL, SIG_IGN);
    signal(SIGTERM, SIG_IGN);

    std::cout << "program recv signal " << signo << " to exit." << std::endl;

    g_bStop = true;

    /* 将g_listenfd从epoll兴趣列表移除 */
    epoll_ctl(g_epollfd, EPOLL_CTL_DEL, g_listenfd, NULL);

    /* 将读端和写端都关闭 */
    shutdown(g_listenfd, SHUT_RDWR);
    close(g_listenfd);
    close(g_epollfd);

    pthread_cond_destroy(&g_accept_cond);
    pthread_mutex_destroy(&g_accept_mutex);

    pthread_cond_destroy(&g_client_cond);
    pthread_mutex_destroy(&g_client_mutex);
}


void release_client(int clientfd)
{
    if(epoll_ctl(g_epollfd, EPOLL_CTL_DEL, clientfd, NULL) == -1)
    {
        std::cout << "release client socket failed as call epoll_ctl fail" << std::endl;
    }

    close(clientfd);
}


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
    while(!g_bStop)
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
        /* 添加进epoll的兴趣列表 */
        if(epoll_ctl(g_epollfd, EPOLL_CTL_ADD, newfd, &e) == -1)
        {
            std::cout << "epoll_ctl error, fd = " << newfd << std::endl;
        }
    }
    return NULL;
}

void *worker_thread_func(void *arg)
{
    while(!g_bStop)
    {
        int clientfd;
        pthread_mutex_lock(&g_client_mutex);
        /* 注意！要用while循环等待 */
        while(g_clientlist.empty())
            pthread_cond_wait(&g_client_cond, &g_client_mutex);

        /* 取出客户套接字 */
        clientfd = g_clientlist.front();
        g_clientlist.pop_front();
        pthread_mutex_unlock(&g_client_mutex);

        std::cout << std::endl;


        std::string strclientmsg;
        char buff[256];
        bool bError = false;
        while(1)
        {
            memset(buff, 0, sizeof(buff));
            int nRecv = recv(clientfd, buff, 256, 0);
            if(nRecv == -1)
            {
                if(errno == EWOULDBLOCK)
                    break;
                else
                {
                    std::cout << "recv error, client disconnected, fd = " << clientfd << std::endl;
                    release_client(clientfd);
                    bError = true;
                    break;
                }
            }
            /* 对端关闭了socket，这端也关闭 */
            else if(nRecv == 0)
            {
                std::cout << "peer clised, client disconnected, fd = " << clientfd << std::endl;
                release_client(clientfd);
                bError = true;
                break;
            }

            strclientmsg += buff;
        }

        /* 如果出错了就不必往下执行了 */
        if(bError)
        {
            continue;
        }

        std::cout << "client msg: " << strclientmsg;

        /* 将消息加上时间戳 */
        time_t now = time(NULL);
        struct tm* nowstr = localtime(&now);
        std::ostringstream ostimestr;
        ostimestr << "[" << nowstr->tm_year + 1900 << "-"
            << std::setw(2) << std::setfill('0') << nowstr->tm_mon + 1 << "-"
            << std::setw(2) << std::setfill('0') << nowstr->tm_mday << " "
            << std::setw(2) << std::setfill('0') << nowstr->tm_hour << ":"
            << std::setw(2) << std::setfill('0') << nowstr->tm_min << ":"
            << std::setw(2) << std::setfill('0') << nowstr->tm_sec << "]server reply: ";

        strclientmsg.insert(0, ostimestr.str());


        while(1)
        {
            int nSend = send(clientfd, strclientmsg.c_str(), strclientmsg.length(), 0);
            if(nSend == -1)
            {
                if(errno == EWOULDBLOCK)
                {
                    sleep(10);
                    continue;
                }
                else
                {
                    std::cout << "send error, fd = " << clientfd << std::endl;
                    release_client(clientfd);
                    break;
                }
            }


            std::cout << "send: " << strclientmsg << std::endl;
            /* 发送完把缓冲区清干净 */
            strclientmsg.erase(0, nSend);

            if(strclientmsg.empty())
                break;
        }
    }


    return NULL;
}


int daemon_run()
{
    signal(SIGCHLD, SIG_IGN);

    int maxfd, fd;

    switch (fork()) {                   /* Become background process */
        case -1: return -1;
        case 0:  break;                     /* Child falls through... */
        default: _exit(EXIT_SUCCESS);       /* while parent terminates */
    }

    if (setsid() == -1)                 /* Become leader of new session */
        return -1;

    switch (fork()) {                   /* Ensure we are not session leader */
        case -1:
            return -1;
        case 0:
            break;
        default:
            exit(EXIT_SUCCESS);
    }
    close(STDIN_FILENO);            /* Reopen standard fd's to /dev/null */

    fd = open("/dev/null", O_RDWR);

    if (fd != STDIN_FILENO)         /* 'fd' should be 0 */
        return -1;
    if (dup2(STDIN_FILENO, STDOUT_FILENO) != STDOUT_FILENO)
        return -1;
    if (dup2(STDIN_FILENO, STDERR_FILENO) != STDERR_FILENO)
        return -1;
}


int main(int argc, char *argv[])
{
    short port = 12345;
    int ch;
    bool bdaemon = false;

    while ((ch = getopt(argc, argv, "p:d")) != -1)
    {
        switch (ch)
        {
            case 'd':
                bdaemon = true;
                break;
            case 'p':
                port = atol(optarg);
                break;
        }
    }

    if (bdaemon)
        daemon_run();


    if (port == 0)
        port = 12345;

    if (!create_server_listener("0.0.0.0", port))
    {
        std::cout << "Unable to create listen server: ip=0.0.0.0, port=" << port << "." << std::endl;
        return -1;
    }


    /* 设置信号处理 */
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, prog_exit);
    signal(SIGKILL, prog_exit);
    signal(SIGTERM, prog_exit);


    pthread_create(&g_accept_threadid, NULL, accept_thread_func, NULL);
    for(int i = 0; i < WORKER_THREAD_NUM; i++)
    {
        pthread_create(&g_threadid[i], NULL, worker_thread_func, NULL);
    }

    while(!g_bStop)
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
