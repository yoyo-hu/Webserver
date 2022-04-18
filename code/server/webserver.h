#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <unordered_map>
#include <fcntl.h>       // fcntl()
#include <unistd.h>      // close()
#include <assert.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "epoller.h"
#include "../log/log.h"
#include "../timer/heaptimer.h"
#include "../pool/sqlconnpool.h"
#include "../pool/threadpool.h"
#include "../pool/sqlconnRAII.h"
#include "../http/httpconn.h"

class WebServer {
public:
    WebServer(
        int port, int trigMode, int timeoutMS, bool OptLinger,                       /* 端口 ET模式 timeoutMs 优雅退出  */
        int sqlPort, const char* sqlUser, const  char* sqlPwd, const char* dbName,   /* Mysql配置 */
        int connPoolNum, int threadNum,bool openLog, int logLevel, int logQueSize);  /* 连接池数量 线程池数量 日志开关 日志等级 日志异步队列容量 */

    ~WebServer();
    void Start();   // 启动服务器

private:
    bool InitSocket_();                         // 初始化Socket
    void InitEventMode_(int trigMode);          // 初始化模式
    void AddClient_(int fd, sockaddr_in addr);  // 添加客户端
  
    void DealListen_();                         // 处理连接事件
    void DealWrite_(HttpConn* client);          // 处理写事件
    void DealRead_(HttpConn* client);           // 处理读事件

    void SendError_(int fd, const char*info);
    void ExtentTime_(HttpConn* client);         // 扩展超时时间
    void CloseConn_(HttpConn* client);          // 关闭客户端连接，并注销相关的epoll注册

    void OnRead_(HttpConn* client);             // 子线程工作函数：读
    void OnWrite_(HttpConn* client);            // 子线程工作函数：写
    void OnProcess(HttpConn* client);           // 子线程工作函数：处理读到的内容

    static const int MAX_FD = 65536;            // 最大的文件描述符的数目

    static int SetFdNonblock(int fd);            // 设置文件描述符非阻塞

    int port_;              // 端口
    bool openLinger_;       // 是否打开优雅关闭
    int timeoutMS_;         // 毫秒MS,用来设置客户端连接的超时时间，如果timeoutMS_时间内客户端没有读写，服务端自动断开与它的连接
    bool isClose_;          // 是否关闭
    int listenFd_;          // 监听的文件描述符
    char* srcDir_;          // 资源的目录
    
    uint32_t listenEvent_;  // 监听的文件描述符的事件
    uint32_t connEvent_;    // 连接的文件描述符的事件
   
    std::unique_ptr<HeapTimer> timer_;          // 定时器
    std::unique_ptr<ThreadPool> threadpool_;    // 线程池
    std::unique_ptr<Epoller> epoller_;          // epoll对象
    std::unordered_map<int, HttpConn> users_;   // 保存的是客户端连接的信息
};


#endif //WEBSERVER_H