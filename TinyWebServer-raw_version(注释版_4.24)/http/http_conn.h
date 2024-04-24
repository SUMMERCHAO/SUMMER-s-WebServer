#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
class http_conn
{
public:
    static const int FILENAME_LEN = 200;            /*文件名m_real_file的最大长度*/
    static const int READ_BUFFER_SIZE = 2048;       /*读缓冲区m_read_buf的大小*/
    static const int WRITE_BUFFER_SIZE = 1024;      /*写缓冲区m_write_buf的大小*/
    /*HTTP请求方法，但我们仅支持GET*/
    enum METHOD
    {
        GET = 0, POST, HEAD, PUT, DELETE,
        TRACE, OPTIONS, CONNECT, PATH
    };
    /*解析客户请求时，主状态机所处的状态*/
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,    //解析请求行
        CHECK_STATE_HEADER,             //解析请求头
        CHECK_STATE_CONTENT             //解析消息体，仅用于解析POST请求
    };
    /*从状态机的三种可能状态，即行的读取状态*/
    enum LINE_STATUS
    {
        LINE_OK = 0,    //读取到一个完整的行
        LINE_BAD,       //报文语法有误
        LINE_OPEN       //行数据尚且不完整
    };
    /*服务器处理HTTP请求的可能结果*/
    enum HTTP_CODE
    {
        NO_REQUEST,         //请求不完整，需要继续读取客户数据
        GET_REQUEST,        //获得了一个完整的客户请求
        BAD_REQUEST,        //客户请求有语法错误
        NO_RESOURCE,        //请求资源不存在
        FORBIDDEN_REQUEST,  //客户对资源没有足够的访问权限
        FILE_REQUEST,       //请求资源可以正常访问
        INTERNAL_ERROR,     //服务器内部错误
        CLOSED_CONNECTION   //客户端关闭连接
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr); //初始化套接字的地址，函数内部回调用私有方法init
    void close_conn(bool real_close = true);        //关闭连接
    void process();         //处理客户请求
    bool read_once();       //非阻塞读操作，读取浏览器端发来的全部数据到buffer
    bool write();           //非阻塞写操作，响应报文写入函数
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    void initmysql_result(connection_pool *connPool);   //同步线程初始化数据库读取表

private:
    void init();        //初始化连接
    HTTP_CODE process_read();       //从m_read_buf读取，并处理请求报文
    bool process_write(HTTP_CODE ret);      //向m_read_buf写入响应报文数据

    /*下面这一组函数被process_read调用以分析HTTP请求*/
    HTTP_CODE parse_request_line(char *text);   //主状态机解析报文中的请求行数据
    HTTP_CODE parse_headers(char *text);        //主状态机解析报文中的请求头数据
    HTTP_CODE parse_content(char *text);        //主状态机解析报文中的请求内容
    HTTP_CODE do_request();     //生成响应报文

    /*m_start_line是行在buffer中的起始位置，将该后面的数据赋给text，get_line用于将指针向后偏移，指向未处理的字符*/
    char *get_line() { return m_read_buf + m_start_line; }; 

    LINE_STATUS parse_line();   //从状态机读取一行，分析是请求报文的哪一部分

    /*下面这一组函数被process_write调用以填充HTTP应答*/    
    void unmap();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;       //所有socket上的事件都被注册到同一个epoll内核事件表中，所以将epoll文件描述符设置为静态的
    static int m_user_count;    //统计用户数量
    MYSQL *mysql;

private:
    //该HTTP连接的socket和对方的socket地址
    int m_sockfd;
    sockaddr_in m_address;

    char m_read_buf[READ_BUFFER_SIZE];  //读缓冲区，存储读取的请求报文数据
    int m_read_idx;         //标识读缓冲区中已经读入的客户数据的最后一个字节的下一个位置
    int m_checked_idx;      //当前正在分析的字符在读缓冲区中的位置
    int m_start_line;       //当前正在解析的行的起始位置

    char m_write_buf[WRITE_BUFFER_SIZE];    //写缓冲区，存储发出的响应报文数据
    int m_write_idx;        //写缓冲区中待发送的字节数

    CHECK_STATE m_check_state;  //主状态机当前所处的状态
    METHOD m_method;            //请求方法

    char m_real_file[FILENAME_LEN]; //客户请求的目标文件的完整路径，其内容相当于doc_root + m_url, doc_root是网站根目录
    char *m_url;            //客户请求的目标文件的文件名
    char *m_version;        //HTTP协议版本号，我们仅支持HTTP/1.1
    char *m_host;           //主机名
    int m_content_length;   //HTTP请求的消息体长度
    bool m_linger;          //HTTP请求是否要求保持连接

    char *m_file_address;       //客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;    //目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    /*我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量*/
    struct iovec m_iv[2];
    int m_iv_count;
    
    int cgi;            //是否启用的POST
    char *m_string;     //存储请求头数据
    int bytes_to_send;  //剩余发送字节数
    int bytes_have_send;    //已发送字节数
};

#endif