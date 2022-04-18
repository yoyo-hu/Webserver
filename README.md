# C++实现的简易WEB服务器

## 1 项目描述：

一个基于c++实现的web服务器，可以对http请求进行解析响应，可以支持上万的QPS

## 2 项目环境：

 linux，c++14，MySql

## 3 项目要点：

1. 实现了基于epoll和线程池的多线程Reactor高并发模型；

2. 实现了基于小根堆实现的计时器，用来关闭超时的停止活动的客户端连接；

3. 实现了基于单例和阻塞队列的异步日志系统，用来记录服务器的运行状态；

4. 实现了基于RAII机制的数据库连接池，用来减少数据库连接建立和关闭的开销。

5. 使用标准库容器封装char，用来实现缓冲区的自动增长；

6. 使用正则表达式和状态机来解析HTTP请求消息，用来实现对静态资源的请求。

## 4 项目细节

### 4.1 工作逻辑

main函数中先初始化服务器WebServer对象server，调用`server.Start();`函数启动服务器。

WebServer类中启动服务器的start函数如下：

函数循环调用epoll来监听连接事件，写事件和读事件，并对这些事件进行相应的处理，对于监听到的异常事件，服务器关闭相关的客户端连接。

```c++
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
```

但监听到连接事件的时候，程序调用`DealListen_`函数对连接进行处理，函数具体如下，程序调用accept函数建立与客户端的连接，在ET模式模式下我们需要一次性处理所有的连接。

```c++
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
```

AddClient_函数将每一个客户端连接都封装成为HttpConn类，并给这个客户端注册写事件，使得epoll可以监听到该客户端的请求（HttpConn类中的文件描述符的读事件）。

```c++
void WebServer::AddClient_(int fd, sockaddr_in addr) {
    assert(fd > 0);
    users_[fd].init(fd, addr);
    if(timeoutMS_ > 0) {
        // 超时调用回调函数，关闭连接
        timer_->add(fd, timeoutMS_, std::bind(&WebServer::CloseConn_, this, &users_[fd]));
    }
    // 给这个客户端注册写事件
    epoller_->AddFd(fd, EPOLLIN | connEvent_);
    SetFdNonblock(fd);
    LOG_INFO("Client[%d] in!", users_[fd].GetFd());
}
```

但监听到读事件的时候，程序调用`DealRead_`函数对读时间进行处理，传入的参数为epoll监听到写事件的文件描述符对应的HttpConn，函数具体如下，程序将读的工作（OnRead_工作函数）添加到线程池中的工作队列中，交给线程池中的线程去处理。

```c++
/* 处理读事件 */
void WebServer::DealRead_(HttpConn* client) {
    assert(client);
    ExtentTime_(client);    // 延长超时时间
    //由线程池中的子线程来处理读操作
    threadpool_->AddTask(std::bind(&WebServer::OnRead_, this, client));
}
```

OnRead_工作函数如下，程序调用对应客户端连接的read函数，将客户端连接的套接字中读数据到改客户端连接的对应的readBuff\_中。

```c++
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
```

```c++
ssize_t HttpConn::read(int* saveErrno) {
    ssize_t len = -1;
    do {
        // 客户端的套接字中读数据到readBuff_中
        len = readBuff_.ReadFd(fd_, saveErrno);
        if (len <= 0) {
            break;
        }
    } while (isET);//ET:要一次性全部读出
    return len;
}
```

当读事件完成以后，需要调用OnProcess函数对读到的数据进行处理（对读到的请求数据进行解析和响应）。

```c++
/* 处理读（请求）数据的函数 */
void WebServer::OnProcess(HttpConn* client) {
    if(client->process()) {
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);//相应成功，修改监听事件为写
    } else {
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLIN);
    }
}
```

