#include "webserver.h"

using namespace std;

WebServer::WebServer(
            int port, int trigMode, int timeoutMS, bool OptLinger,
            int sqlPort, const char* sqlUser, const  char* sqlPwd,
            const char* dbName, int connPoolNum, int threadNum,
            bool openLog, int logLevel, int logQueSize):
            port_(port), openLinger_(OptLinger), timeoutMS_(timeoutMS), isClose_(false),
            timer_(new HeapTimer()), threadpool_(new ThreadPool(threadNum)), epoller_(new Epoller())
    {
    srcDir_ = getcwd(nullptr, 256);         // 获取当前的工作路径
    assert(srcDir_);
    strncat(srcDir_, "/resources/", 16);    // 拼接路径，这个路径就是资源的根路径
    
    HttpConn::userCount = 0;                // 设置当前客户端用户数为0
    HttpConn::srcDir = srcDir_;             // 设置资源目录

    // 初始化数据库连接池
    SqlConnPool::Instance()->Init("localhost", sqlPort, sqlUser, sqlPwd, dbName, connPoolNum);

    // 初始化事件的模式
    InitEventMode_(trigMode);
    // 初始化失败，关闭服务器
    if(!InitSocket_()) { isClose_ = true;}

    // 日志相关
    if(openLog) {
        // logQueSize<=0使用同步
        Log::Instance()->init(logLevel, "./log", ".log", logQueSize);
        if(isClose_) { LOG_ERROR("========== Server init error!=========="); }
        else {
            LOG_INFO("========== Server init ==========");
            LOG_INFO("Port:%d, OpenLinger: %s", port_, OptLinger? "true":"false");
            LOG_INFO("Listen Mode: %s, OpenConn Mode: %s",
                            (listenEvent_ & EPOLLET ? "ET": "LT"),
                            (connEvent_ & EPOLLET ? "ET": "LT"));
            LOG_INFO("LogSys level: %d", logLevel);
            LOG_INFO("srcDir: %s", HttpConn::srcDir);
            LOG_INFO("SqlConnPool num: %d, ThreadPool num: %d", connPoolNum, threadNum);
        }
    }
}

WebServer::~WebServer() {
    close(listenFd_);
    isClose_ = true;
    free(srcDir_);
    SqlConnPool::Instance()->ClosePool();
}

/* 设置监听的文件描述符和通信的文件描述符的模式 */
void WebServer::InitEventMode_(int trigMode) {
    listenEvent_ = EPOLLRDHUP;                  // 监听文件描述符设置的事件，EPOLLRDHUP用来监测对方是否正常关闭
    connEvent_ = EPOLLONESHOT | EPOLLRDHUP;     // 通信文件描述符设置的事件，EPOLLONESHOT表示一次注册的事件只能监听执行一次,EPOLLRDHUP用来监测对方是否正常关闭
    //不设置则为是LT模式
    switch (trigMode)
    {
    case 0:
        break;                       // LT模式
    case 1:
        connEvent_ |= EPOLLET;      // connEvent设置为ET模式
        break;
    case 2:
        listenEvent_ |= EPOLLET;    // listenEvent_设置为ET模式
        break;
    default:
        listenEvent_ |= EPOLLET;    // 都设置为ET模式
        connEvent_ |= EPOLLET;
        break;
    }
    HttpConn::isET = (connEvent_ & EPOLLET);    // 静态变量isET：通信模式是否为ET模式
}

/* 启动服务器 */
void WebServer::Start() {
    int timeMS = -1;  // epoll wait timeout == -1 无事件将阻塞 
    if(!isClose_) { LOG_INFO("========== Server start =========="); }
    // 循环调用epoll
    while(!isClose_) {
        // 解决超时连接
        if(timeoutMS_ > 0) {
            timeMS = timer_->GetNextTick();      // 清除超时的客户端连接，并得到下一次超时的时间
        }
        int eventCnt = epoller_->Wait(timeMS);   // 使得epoller_wait阻塞超过timeMS时间返回
        for(int i = 0; i < eventCnt; i++) {
            /* 处理事件 */
            int fd = epoller_->GetEventFd(i);
            uint32_t events = epoller_->GetEvents(i);
            if(fd == listenFd_) {                                   //处理监听事件
                DealListen_();
            }
            else if(events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {  //异常
                assert(users_.count(fd) > 0);
                CloseConn_(&users_[fd]);
            }
            else if(events & EPOLLIN) {                             //处理读操作
                assert(users_.count(fd) > 0);
                DealRead_(&users_[fd]);
            }
            else if(events & EPOLLOUT) {                            //处理写操作
                assert(users_.count(fd) > 0);
                DealWrite_(&users_[fd]);
            } else {
                LOG_ERROR("Unexpected event");
            }
        }
    } 
}

void WebServer::SendError_(int fd, const char*info) {
    assert(fd > 0);
    int ret = send(fd, info, strlen(info), 0);
    if(ret < 0) {
        LOG_WARN("send error to client[%d] error!", fd);
    }
    close(fd);
}

/* 关闭连接 */
void WebServer::CloseConn_(HttpConn* client) {
    assert(client);
    LOG_INFO("Client[%d] quit!", client->GetFd());
    epoller_->DelFd(client->GetFd());   // 把客户端的文件描述符从epoll中移除
    client->Close();                    // 关闭客户端
}

void WebServer::AddClient_(int fd, sockaddr_in addr) {
    assert(fd > 0);
    users_[fd].init(fd, addr);
    if(timeoutMS_ > 0) {
        //超时调用回调函数，关闭连接
        timer_->add(fd, timeoutMS_, std::bind(&WebServer::CloseConn_, this, &users_[fd]));
    }
    //给这个客户端注册写事件
    epoller_->AddFd(fd, EPOLLIN | connEvent_);
    SetFdNonblock(fd);
    LOG_INFO("Client[%d] in!", users_[fd].GetFd());
}

/* 处理连接事件 */
void WebServer::DealListen_() {
    struct sockaddr_in addr;            //保存连接的客户端的地址
    socklen_t len = sizeof(addr);
    do {
        //非阻塞的accept
        int fd = accept(listenFd_, (struct sockaddr *)&addr, &len);
        
        if(fd <= 0) { return;}
        else if(HttpConn::userCount >= MAX_FD) {
            SendError_(fd, "Server busy!");
            LOG_WARN("Clients is full!");
            return;
        }

        AddClient_(fd, addr);            //添加客户端
    } while(listenEvent_ & EPOLLET);     //ET模式：需要一次性处理全部的连接
}

/* 处理读事件 */
void WebServer::DealRead_(HttpConn* client) {
    assert(client);
    ExtentTime_(client);    // 延长超时时间
    //由线程池中的子线程来处理读操作
    threadpool_->AddTask(std::bind(&WebServer::OnRead_, this, client));
}

/* 处理写事件 */
void WebServer::DealWrite_(HttpConn* client) {
    assert(client);
    ExtentTime_(client);    // 延长超时时间
    //由线程池中的子线程来处理写操作
    threadpool_->AddTask(std::bind(&WebServer::OnWrite_, this, client));
}

void WebServer::ExtentTime_(HttpConn* client) {
    assert(client);
    if(timeoutMS_ > 0) { timer_->adjust(client->GetFd(), timeoutMS_); }
}

/* 线程池中的子线程的读工作函数 */
void WebServer::OnRead_(HttpConn* client) {
    assert(client);
    int ret = -1;
    int readErrno = 0;
    ret = client->read(&readErrno);         // 读取客户端套接字的数据，读到httpconn的读缓存区
    if(ret <= 0 && readErrno != EAGAIN) {   // 读异常就关闭客户端
        CloseConn_(client);
        return;
    }
    // 业务逻辑的处理（先读后处理）
    OnProcess(client);
}

/* 处理读（请求）数据的函数 */
void WebServer::OnProcess(HttpConn* client) {
    if(client->process()) {
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);//相应成功，修改监听事件为写
    } else {
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLIN);
    }
}

/* 线程池中的子线程的写工作函数 */
void WebServer::OnWrite_(HttpConn* client) {
    assert(client);
    int ret = -1;
    int writeErrno = 0;
    ret = client->write(&writeErrno);
    if(client->ToWriteBytes() == 0) {
        /* 传输完成 */
        if(client->IsKeepAlive()) {
            OnProcess(client);
            return;
        }
    }
    else if(ret < 0) {
        if(writeErrno == EAGAIN) {
            /* 继续传输 */
            epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
            return;
        }
    }
    CloseConn_(client);
}

/* Create listenFd */
bool WebServer::InitSocket_() {
    int ret;
    struct sockaddr_in addr;    //套接字地址
    if(port_ > 65535 || port_ < 1024) { //判断端口号
        LOG_ERROR("Port:%d error!",  port_);
        return false;
    }
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   //主机字节序转换为网络字节序
    addr.sin_port = htons(port_);
    struct linger optLinger = { 0 };
    if(openLinger_) {
        /* 优雅关闭: 直到所剩数据发送完毕或超时 */
        optLinger.l_onoff = 1;
        optLinger.l_linger = 1;
    }

    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if(listenFd_ < 0) {
        LOG_ERROR("Create socket error!", port_);
        return false;
    }

    ret = setsockopt(listenFd_, SOL_SOCKET, SO_LINGER, &optLinger, sizeof(optLinger));
    if(ret < 0) {
        close(listenFd_);
        LOG_ERROR("Init linger error!", port_);
        return false;
    }

    int optval = 1;
    /* 端口复用 */
    /* 只有最后一个套接字会正常接收数据。 */
    ret = setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int));
    if(ret == -1) {
        LOG_ERROR("set socket setsockopt error !");
        close(listenFd_);
        return false;
    }

    ret = bind(listenFd_, (struct sockaddr *)&addr, sizeof(addr));
    if(ret < 0) {
        LOG_ERROR("Bind Port:%d error!", port_);
        close(listenFd_);
        return false;
    }

    ret = listen(listenFd_, 6);
    if(ret < 0) {
        LOG_ERROR("Listen port:%d error!", port_);
        close(listenFd_);
        return false;
    }
    ret = epoller_->AddFd(listenFd_,  listenEvent_ | EPOLLIN);
    if(ret == 0) {
        LOG_ERROR("Add listen error!");
        close(listenFd_);
        return false;
    }
    SetFdNonblock(listenFd_);
    LOG_INFO("Server port:%d", port_);
    return true;
}

int WebServer::SetFdNonblock(int fd) {
    assert(fd > 0);
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFD, 0) | O_NONBLOCK);
}


