//课本200页
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./timer/lst_timer.h"
#include "./http/http_conn.h"
#include "./log/log.h"
#include "./CGImysql/sql_connection_pool.h"

#define MAX_FD 65536           //最大文件描述符
#define MAX_EVENT_NUMBER 10000 //最大事件数
#define TIMESLOT 5             //最小超时单位

#define SYNLOG     //同步写日志
//#define ASYNLOG  //异步写日志

//#define listenfdET //边缘触发非阻塞
#define listenfdLT   //水平触发阻塞

/*这三个函数在http_conn.cpp中定义，改变链接属性*/
extern int addfd(int epollfd, int fd, bool one_shot); //extern:声明外部变量或函数
extern int remove(int epollfd, int fd);
extern int setnonblocking(int fd);

/*设置定时器相关参数*/
static int pipefd[2];               //定义管道文件描述符
static sort_timer_lst timer_lst;    //定义升序定时器容器链表
static int epollfd = 0;

/*信号处理函数*/
void sig_handler(int sig)
{
    int save_errno = errno;     //为保证函数的可重入性，保留原来的errno，可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);    //将信号值写入管道，以通知主循环。传输字符类型，而非整型
    errno = save_errno;
}

/*设置信号函数*/
void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;    //创建sigaction结构体变量，定义sa_handler,sa_flags,sa_mask
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);    //将所有信号添加到信号集中
    assert(sigaction(sig, &sa, NULL) != -1);    //执行sigaction函数
}

/*定时处理任务，重新定时以不断触发SIGALRM信号*/
void timer_handler()
{
    timer_lst.tick();       //处理定时任务，实际上就是调用tick()函数
    alarm(TIMESLOT);        //因为一次alarm调用只会引起一次SIGALRM信号，所以我们要重新定时，以不断触发SIGALRM信号
}

/*定时器回调函数，删除非活动连接在socket上的注册事件，并关闭*/
void cb_func(client_data *user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);    //删除非活动连接在socket上的注册事件
    assert(user_data);
    close(user_data->sockfd);           //关闭文件描述符
    http_conn::m_user_count--;          //减少连接数
    LOG_INFO("close fd %d", user_data->sockfd);
    Log::get_instance()->flush();
}

void show_error(int connfd, const char *info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[])
{
#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8); //异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0); //同步日志模型
#endif

    if (argc <= 1)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]);   //接收端口号

    addsig(SIGPIPE, SIG_IGN);

    //创建数据库连接池
    connection_pool *connPool = connection_pool::GetInstance();
    connPool->init("localhost", "root", "Abcdefg@123", "summerdb", 3306, 8);

    //创建线程池
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>(connPool);
    }
    catch (...)
    {
        return 1;
    }

    /*创建MAX_FD个http类对象*/
    http_conn *users = new http_conn[MAX_FD];
    assert(users);

    /*初始化数据库读取表*/
    users->initmysql_result(connPool);

    /*server的实现*/
    int listenfd = socket(PF_INET, SOCK_STREAM, 0); //创建一个TCP监听socket文件描述符   socket(指定IP地址协议, 数据传输协议, 代表协议(默认为0，由type决定))
    assert(listenfd >= 0);                          //判断函数返回值，若listenfd == -1，返回错误；void assert(int expression); 计算表达式 expression, 如果其值为假(即为0), 打印出错信息，终止程序运行。
    //struct linger tmp={1,0};
    //SO_LINGER若有数据待发送，延迟关闭
    //setsockopt(listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));

    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)); //端口复用,允许创建端口号相同但IP地址不同的多个socket文件描述符，在bind之前

    /*初始化sockaddr_in结构 配置服务器地址*/
    struct sockaddr_in address;                   //创建监听socket的TCP/IP的IPV4 socket地址,初始化如下1 2 3
    bzero(&address, sizeof(address));             //将address的前sizeof(address)个字节数清零
    address.sin_family = AF_INET;                 //1.所用的地址结构类型, AF_INET:IPV4
    address.sin_addr.s_addr = htonl(INADDR_ANY);  //2.取出系统中有效的IP地址, htonl:
    address.sin_port = htons(port);               //3.初始化端口号port, htons:本地字节序转换为网络字节序
    
    int ret = 0;
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address)); //给socket绑定一个地址结构(IP+Port)
    assert(ret >= 0);
    ret = listen(listenfd, 5);                    //设置监听队列以存放待处理的客户连接；设置同时与服务器建立连接的上限数; 同时进行3次握手的客户端数量，最大值为128
    assert(ret >= 0);

    /*创建内核事件表*/
    epoll_event events[MAX_EVENT_NUMBER];         //用于存储epoll事件表中就绪事件的event数组
    epollfd = epoll_create(5);                    //创建一个额外的文件描述符来唯一标识内核中的epoll事件表; 创建epoll模型，epollfd指向红黑树根节点
    assert(epollfd != -1);
    addfd(epollfd, listenfd, false);              //主线程往epoll内核事件表中注册监听socket事件，当listen到新的客户连接时，listenfd变为就绪事件
    
    http_conn::m_epollfd = epollfd;     //将epollfd赋值给http类对象的m_epollfd属性

    /*使用socketpair创建管道，注册pipefd[0]上的可读事件 （课本p186）*/
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);  //创建管道
    assert(ret != -1);
    setnonblocking(pipefd[1]);                          //将管道的写端设置为非阻塞
    addfd(epollfd, pipefd[0], false);                   //将管道的读端添加到内核事件表,并设置为非堵塞

    /*设置信号处理函数，传递给主循环的信号值，这里只关注SIGALRM和SIGTERM*/
    addsig(SIGALRM, sig_handler, false);    //时间到了触发
    addsig(SIGTERM, sig_handler, false);    //kill会触发

    /*循环条件*/
    bool stop_server = false;

    client_data *users_timer = new client_data[MAX_FD]; //创建连接资源数组
    bool timeout = false;   //超时标志，默认为false
    alarm(TIMESLOT);        //定时，每隔TIMESLOT时间触发SIGALRM信号

    while (!stop_server)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1); //主线程调用epoll_wait等待一组文件描述符上的事件，并将当前所有就绪的epoll_event复制到events数组中
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        /*遍历events数组以处理这些已经就绪的事件*/
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;       //事件表中就绪的socket文件描述符

            /*处理新到的客户连接*/
            if (sockfd == listenfd)               //如果就绪的文件描述符是listenfd，则处理新的连接
            {
                /*初始化客户端连接地址*/
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
#ifdef listenfdLT  //LT模式
                /*该连接分配的文件描述符*/
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);  //accept返回一个新的socket文件描述符用于send()和recv()，accept中第二个参数为传出参数，返回成功与服务器建立连接的那个客户端的地址结构
                if (connfd < 0)                                       
                {
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "Internal server busy");
                    LOG_ERROR("%s", "Internal server busy");
                    continue;
                }
                users[connfd].init(connfd,client_address);     //将connfd注册到内核事件表中（加入红黑树）
                
                /*初始化该连接对应的连接资源*/
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                
                /*创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表timer_lst中*/
                util_timer *timer = new util_timer;         //创建定时器临时变量
                timer->user_data = &users_timer[connfd];    //设置定时器对应的连接资源
                timer->cb_func = cb_func;                   //设置回调函数
                time_t cur = time(NULL);                //当前绝对时间
                timer->expire = cur + 3 * TIMESLOT;     //设置绝对超时时间  
                users_timer[connfd].timer = timer;      //创建该连接对应的定时器，初始化为前述临时变量
                timer_lst.add_timer(timer);             //将该定时器添加到链表中
#endif

#ifdef listenfdET    //ET模式
                while (1)
                {
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (connfd < 0)
                    {
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        break;
                    }
                    if (http_conn::m_user_count >= MAX_FD)
                    {
                        show_error(connfd, "Internal server busy");
                        LOG_ERROR("%s", "Internal server busy");
                        break;
                    }
                    users[connfd].init(connfd, client_address);

                    //初始化client_data数据
                    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    util_timer *timer = new util_timer;
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);
                }
                continue;
#endif
            }

            /*处理异常事件：如有异常，则直接关闭客户连接，并删除该用户的timer*/
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                /*服务器端关闭连接，移除对应的定时器*/
                util_timer *timer = users_timer[sockfd].timer;  //创建定时器临时变量，将该连接对应的定时器取出来
                timer->cb_func(&users_timer[sockfd]);

                if (timer)
                {
                    timer_lst.del_timer(timer);
                }
            }

            /*处理定时器信号：如果管道读端对应文件描述符发生读事件，则处理信号*/
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                /*从管道读端读出信号值，成功返回字节数，失败返回-1；正常情况下，这里的ret返回值总是1，只有14和15两个ASCII码对应的字符*/
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    /*处理信号值对应的逻辑；因为每个信号值占1字节，所以按字节逐个接收信号*/
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                        case SIGALRM:
                        {
                            /*用timeout变量标记有定时任务需要处理，但不立即处理定时任务。
                            因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。*/
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

            /*处理客户连接上接收到的数据*/
            else if (events[i].events & EPOLLIN)
            {
                util_timer *timer = users_timer[sockfd].timer;  //创建定时器临时变量，将该连接对应的定时器取出来
                /*当这一sockfd上有可读事件时，epoll_wait通知主线程*/
                if (users[sockfd].read_once())               //‘主线程’ 从这一socket循环读取数据，直到没有更多数据可读
                {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    pool->append(users + sockfd);            //然后将读取到的数据封装成一个请求对象并插入请求队列线程池，选择一个‘工作线程’处理

                    /*若有数据传输，则将定时器往后延迟3个单位。并对新的定时器在链表上的位置进行调整*/
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                /*如果发生错误，或对方关闭连接，则我们也关闭连接，并移除其对应的定时器*/
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }

            else if (events[i].events & EPOLLOUT)             //如果当前文件描述符为写事件
            {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write())       //当这一sockfd上有可写事件时，epoll_wait通知主线程。主线程往socket上写入服务器处理客户请求的结果
                {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    /*若有数据传输，则将定时器往后延迟3个单位。并对新的定时器在链表上的位置进行调整*/
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}