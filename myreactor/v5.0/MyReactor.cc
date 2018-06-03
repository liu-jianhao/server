#include "MyReactor.h"


#define min(a, b) ((a <= b) ? (a) : (b))

MyReactor::MyReactor()
{
}

MyReactor::~MyReactor()
{
}

struct ARG
{
    MyReactor* pThis;
};

bool MyReactor::init(const char* ip, short nport)
{
    if(!create_server_listener(ip, nport))
    {
        LOG_DEBUG("Unable to bind: %s:%d.\n", ip, nport);
        return false;
    }

    ARG *arg = new ARG();
    arg->pThis = this;

    pthread_create(&m_accept_threadid, NULL, accept_thread_proc, (void*)arg);

    LOG_DEBUG("accept thread \n");

    for(int i = 0; i < WORKER_THREAD_NUM; i++)
    {
        pthread_create(&m_threadid[i], NULL, worker_thread_proc, (void*)arg);
    }

    return true;
}


bool MyReactor::uninit()
{
    m_bStop = true;


    /* 将读端和写端都关闭 */
    shutdown(m_listenfd, SHUT_RDWR);
    close(m_listenfd);
    close(m_epollfd);

    return true;
}


void* MyReactor::main_loop(void *p)
{
    LOG_DEBUG("main thread id = %ld\n", pthread_self());

    MyReactor* pReactor = static_cast<MyReactor*>(p);


    while(!pReactor->m_bStop)
    {
        /* std::cout << "main loop" << std::endl; */
        struct epoll_event ev[1024];
        int n = epoll_wait(pReactor->m_epollfd, ev, 1024, 10);
        if(n == 0)
            continue;
        else if(n < 0)
        {
            LOG_ERROR("epoll_wait error\n");
            continue;
        }

        int m = min(n, 1024);
        for(int i = 0; i < m; i++)
        {
            /* 有新连接 */
            if(ev[i].data.fd == pReactor->m_listenfd)
                pthread_cond_signal(&pReactor->m_accept_cond);
            /* 有数据 */
            else
            {
                pthread_mutex_lock(&pReactor->m_client_mutex);
                pReactor->m_clientlist.push_back(ev[i].data.fd);
                pthread_mutex_unlock(&pReactor->m_client_mutex);
                pthread_cond_signal(&pReactor->m_client_cond);
            }
        }
    }

    LOG_DEBUG("main loop exit ...\n");
    return NULL;
}


bool MyReactor::close_client(int clientfd)
{
    if(epoll_ctl(m_epollfd, EPOLL_CTL_DEL, clientfd, NULL) == -1)
    {
        LOG_DEBUG("release client socket failed as call epoll_ctl fail\n");
    }

    close(clientfd);
    return true;
}


bool MyReactor::create_server_listener(const char* ip, short port)
{
    m_listenfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if(m_listenfd == -1)
    {
        return false;
    }

    int on = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEPORT, (char *)&on, sizeof(on));

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = inet_addr(ip);
    servaddr.sin_port = htons(port);

    if(bind(m_listenfd, (sockaddr *)&servaddr, sizeof(servaddr)) == -1)
        return false;

    if(listen(m_listenfd, 50) == -1)
        return false;

    m_epollfd = epoll_create(1);
    if(m_epollfd == -1)
        return false;

    struct epoll_event e;
    memset(&e, 0, sizeof(e));
    e.events = EPOLLIN | EPOLLRDHUP;
    e.data.fd = m_listenfd;
    if(epoll_ctl(m_epollfd, EPOLL_CTL_ADD, m_listenfd, &e) == -1)
        return false;

    return true;
}


void* MyReactor::accept_thread_proc(void* args)
{
    ARG *arg = (ARG*)args;
    MyReactor* pReactor = arg->pThis;

    while(!pReactor->m_bStop)
    {
        pthread_mutex_lock(&pReactor->m_accept_mutex);
        pthread_cond_wait(&pReactor->m_accept_cond, &pReactor->m_accept_mutex);

        struct sockaddr_in clientaddr;
        socklen_t addrlen;
        int newfd = accept(pReactor->m_listenfd, (struct sockaddr *)&clientaddr, &addrlen);
        pthread_mutex_unlock(&pReactor->m_accept_mutex);
        if(newfd == -1)
            continue;

        LOG_DEBUG("new client connected: ");


        /* 将新socket设置为non-blocking */
        int oldflag = fcntl(newfd, F_GETFL, 0);
        int newflag = oldflag | O_NONBLOCK;
        if(fcntl(newfd, F_SETFL, newflag) == -1)
        {
            LOG_DEBUG("fcntl error, oldflag = %d , newflag = %d\n", oldflag, newflag);
            continue;
        }

        struct epoll_event e;
        memset(&e, 0, sizeof(e));
        e.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
        e.data.fd = newfd;
        /* 添加进epoll的兴趣列表 */
        if(epoll_ctl(pReactor->m_epollfd, EPOLL_CTL_ADD, newfd, &e) == -1)
        {
            LOG_ERROR("epoll_ctl error, fd = %d\n", newfd);
        }
    }

    return NULL;
}

void* MyReactor::worker_thread_proc(void* args)
{
    ARG *arg = (ARG*)args;
    MyReactor* pReactor = arg->pThis;

    while(!pReactor->m_bStop)
    {
        int clientfd;
        pthread_mutex_lock(&pReactor->m_client_mutex);
        /* 注意！要用while循环等待 */
        while(pReactor->m_clientlist.empty())
            pthread_cond_wait(&pReactor->m_client_cond, &pReactor->m_client_mutex);

        /* 取出客户套接字 */
        clientfd = pReactor->m_clientlist.front();
        pReactor->m_clientlist.pop_front();
        pthread_mutex_unlock(&pReactor->m_client_mutex);

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
                    LOG_ERROR("recv error, client disconnected, fd = %d\n", clientfd);
                    pReactor->close_client(clientfd);
                    bError = true;
                    break;
                }
            }
            /* 对端关闭了socket，这端也关闭 */
            else if(nRecv == 0)
            {
                LOG_ERROR("peer closed, client disconnected, fd = %d\n", clientfd);
                pReactor->close_client(clientfd);
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

        LOG_DEBUG("client msg: %s", strclientmsg.c_str());

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
                    LOG_ERROR("send error, fd = %d\n", clientfd);
                    pReactor->close_client(clientfd);
                    break;
                }
            }


            LOG_DEBUG("send: %s\n", strclientmsg.c_str());
            /* 发送完把缓冲区清干净 */
            strclientmsg.erase(0, nSend);

            if(strclientmsg.empty())
                break;
        }
    }
    return NULL;
}
