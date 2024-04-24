/*参考：第四章、8.6节、15.6节*/
#include "http_conn.h"
#include "../log/log.h"
#include <map>
#include <mysql/mysql.h>
#include <fstream>

//#define connfdET //边缘触发非阻塞
#define connfdLT //水平触发阻塞

//#define listenfdET //边缘触发非阻塞
#define listenfdLT //水平触发阻塞

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

//当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
const char *doc_root = "/home/summer/WebServer/TinyWebServer0220/root";

/*载入数据库表；将数据库中的用户名和密码载入到服务器的map中来，map中的key为用户名，value为密码*/
map<string, string> users;
locker m_lock;
void http_conn::initmysql_result(connection_pool *connPool)
{
    /*先从连接池中取一个连接*/
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    /*在user表中检索username，passwd数据，浏览器端输入*/
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集，这个结果集包含所有匹配查询的行
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

/*对文件描述符设置非阻塞模式*/
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);        //获取文件描述符旧的状态标志
    int new_option = old_option | O_NONBLOCK;   //新状态添加非阻塞标志
    fcntl(fd, F_SETFL, new_option);             //设置fd为新状态
    return old_option;                          //返回fd旧的状态标志，以便日后回复该状态标志
}

/*将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT*/
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;

#ifdef connfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef connfdLT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif

#ifdef listenfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef listenfdLT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif
    /*针对cfd，开启EPOLLONESHOT，因为我们希望每个socket在任意时刻都只被一个线程处理*/
    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);  //将lfd及对应的结构体设置到树上
    setnonblocking(fd);
}

/*从内核事件表删除描述符*/
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

/*将事件重置为EPOLLONESHOT*/
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;

#ifdef connfdET
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
#endif

#ifdef connfdLT
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
#endif

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;        //m_user_count统计用户数量
int http_conn::m_epollfd = -1;

/*关闭连接，关闭一个连接，客户总量减一*/
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

/*初始化连接,外部调用初始化套接字地址*/
void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    //int reuse=1;
    //setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    addfd(m_epollfd, sockfd, true);
    m_user_count++;
    init();
}

/*初始化新接受的连接*/
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;    //主状态机的初始状态，默认为分析请求行状态
    m_linger = false;       //HTTP请求是否要求保持连接，默认为不保持连接
    m_method = GET;         //请求方法，默认为GET
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

/*主线程循环读取客户数据，直到无数据可读或对方关闭连接*/
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

#ifdef connfdLT
    bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0); //网络通信接收数据
    if (bytes_read <= 0)
    {
        return false;
    }
    m_read_idx += bytes_read;   //修改m_read_idx的读取字节数
    return true;
#endif

#ifdef connfdET
    /*非阻塞ET工作模式下，需要一次性将数据读完*/
    while (true)
    {
        /*从套接字中读取数据，存储在m_read_buf缓冲区*/
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return false;
        }
        else if (bytes_read == 0)
        {
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
#endif
}

/*处理客户请求；由线程池中的工作线程调用，这是处理HTTP请求的入口函数*/
void http_conn::process()
{
    //调用process_read函数完成报文解析
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)     //只有当HTTP_CODE是NO_REQUEST状态时不需要发送响应报文，其余状态都需要发送响应报文至浏览器端
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);    //注册并监听读事件，将浏览器发送的请求报文读入buffer
        return;
    }

    //调用process_write函数完成报文响应
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);    //注册并监听写事件，将响应报文发送至浏览器端
}

/*###############################################################第一部分：服务器解析请求报文###############################################################
############ 解析m_read_buf中主线程读入的请求报文。从状态机使用parse_line()函数读取报文的一行，主状态机调用process_read()函数对该行数据进行解析 ############*/

/*从状态机，用于分析出一行内容。返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN*/
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    /*m_checked_idx指向m_read_buf（读缓冲区）中当前正在分析的字节，m_read_idx指向m_read_buf中客户数据尾部的下一个字节。
    m_read_buf中第0~m_checked_idx字节都已分析完毕，第m_checked_idx~（m_read_idx - 1）字节由下面的循环挨个分析*/
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];   //获得当前要分析的字节

        /*如果当前字节是"/r"，即回车符，则说明可能读到一个完整的行*/
        if (temp == '\r')
        {
            /*如果下一个字符达到了m_read_buf的末尾，那么这次分析没有读取到一个完整的行，返回LINE_OPEN以表示还需要继续读取客户数据才能进一步分析*/
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;

            /*如果下一个字符是"/n"，则说明我们成功读取到一个完整的行，将\r\n改为\0\0*/
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }

            /*否则，说明客户发送的HTTP请求存在语法问题*/
            return LINE_BAD;
        }

        /*如果当前字节是"/n"，即换行符，则说明可能读到一个完整的行*/
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }

    /*如果分析完所有内容也没有遇到/r字符，则返回LINE_OPEN，表示还需要继续读取客户数据才能进一步分析*/
    return LINE_OPEN;
}

/*主状态机；对该connfd读缓冲区的请求报文进行解析*/
http_conn::HTTP_CODE http_conn::process_read()
{
    /*初始化从状态机状态，HTTP请求解析结果*/
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;
    /*主状态机，用于从m_read_buf中取出所有完整的行*/
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))   //如果获取到完整的行
    {
        /*将m_start_line后面的数据赋给text，此时从状态机已提前将一行的末尾字符\r\n变为\0\0，所以text可以直接取出完整的行进行解析*/
        text = get_line();
        /*记录下一行的起始位置；m_start_line是每一个数据行在m_read_buf中的起始位置，m_checked_idx表示从状态机在m_read_buf中读取的位置*/
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        Log::get_instance()->flush();

        /*主状态机的三种状态转移逻辑，m_check_state记录主状态机当前的状态*/
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:       //第一个状态，分析请求行
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:            //第二个状态，分析头部字段
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();    //完整解析GET请求后，跳转到报文响应函数
            }
            break;
        }
        case CHECK_STATE_CONTENT:           //第三个状态，解析消息体。
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();    //完整解析POST请求后，跳转到报文响应函数
            line_status = LINE_OPEN;    //解析完消息体即完成报文解析，避免再次进入循环，更新line_status = LINE_OPEN
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

/*解析请求行。获得请求方式，目标url(访问资源)及http版本号*/
/*在HTTP请求报文中，请求行用来说明请求类型，要访问的资源以及所使用的HTTP版本，其中各个部分之间通过\t或空格分隔。*/
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    /*确定请求方式*/
    m_url = strpbrk(text, " \t"); //查找text字符集集合中\t字符的位置，找到返回第一个匹配的字符的指针，没找到返回NULL
    /*如果请求行中没有空白字符或\t字符，则HTTP请求必有问题*/
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';    //将该位置改为\0，用于将前面的数据取出
    /*取出数据，并通过与GET和POST比较，以确定请求方式*/
    char *method = text;
    if (strcasecmp(method, "GET") == 0)     //strcasecmp()用于比较两个字符串是否相等，相等返回0
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    

    /*确定HTTP版本号*/
    /*m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有，
    将m_url向后偏移，通过查找，继续跳过空格和\t字符，指向请求资源的第一个字符*/
    m_url += strspn(m_url, " \t");          //strspn计算字符串中从开头连续包含字符集合的字符数目，返回找到的连续字符数目
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");

    /*仅支持HTTP/1.1*/
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;

    /*确定访问资源*/
    /*对请求资源前7个字符进行判断，有些报文的请求资源中会带有"http://"或"https://"，这里需要对这种情况进行单独处理*/
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    /*一般情况的不会带有上述两种符号，直接是单独的/或/后面带访问资源*/
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    /*当url为/时，显示判断界面*/
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    /*HTTP请求行处理完毕，状态转移到头部字段的分析*/
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

/*解析请求头*/
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    /*遇到空行，表示头部字段解析完毕*/
    if (text[0] == '\0')
    {
        /*如果HTTP请求有消息体，则说明是POST请求，还需要读取m_content_length字节的消息体，状态机制转移到CHECK_STATE_CONTENT*/
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        /*否则说明我们已经得到了一个完整的HTTP请求*/
        return GET_REQUEST;
    }

    /*处理Connection头部字段*/
    else if (strncasecmp(text, "Connection:", 11) == 0)  //两个字符相等返回0
    {
        text += 11;
        text += strspn(text, " \t");    //跳过空格和/t字符
        /*如果是长连接，则将linger标志设置为true*/
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    /*处理Content-Length头部字段*/
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);      //atol()字符串转换为长整型数
    }
    /*处理Host头部字段*/
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        //printf("oop!unknow header: %s\n",text);
        LOG_INFO("oop!unknow header: %s", text);
        Log::get_instance()->flush();
    }
    return NO_REQUEST;
}

/*解析消息体。仅用于解析POST请求，判断它是否被完整地读入了*/
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        /*POST请求中最后为输入的用户名和密码*/
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

/*###################################################第二部分：服务器完成响应报文，并将响应报文发送给浏览器端###################################################
############ do_request()解析请求资源，process_write()向m_write_buf中写入响应报文，write()发送响应报文给浏览器端 ############*/

/*当得到一个完整、正确的HTTP请求时，我们就将网站根目录和url文件拼接获得m_real_file，然后通过stat判断该文件属性。
如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其映射到内存地址m_file_address处，返回FILE_REQUEST*/
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);      //将网站根目录doc_root复制到m_real_file
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');    //找到m_url中 / 的位置

    /*实现登录和注册校验；cgi == 1 表示POST请求; p代表m_url中/的位置，其后面的第一个字符是2则为登录校验，3为注册校验*/
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        /*根据标志判断是登录检测还是注册检测*/
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        /*将用户名和密码提取出来;user=123&passwd=123*/
        char name[100], password[100];
        int i;

        /*以&为分隔符，前面的为用户名*/
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        /*以&为分隔符，后面的为密码*/
        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        /*同步线程登录注册*/
        /*注册校验*/
        if (*(p + 1) == '3')
        {
            /*如果是注册，先检测数据库中是否有重名的; 没有重名的，进行增加数据*/
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {

                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    /*如果请求资源为/0，表示跳转注册界面*/
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real)); //将网站目录和/register.html进行拼接，更新到m_real_file中

        free(m_url_real);
    }
    /*如果请求资源为/1，表示跳转登录界面*/
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real)); //将网站目录和/log.html进行拼接，更新到m_real_file中

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    /*如果以上情况都不符合，直接将url与网站目录拼接*/
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    /*通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体。失败返回NO_RESOURCE状态，表示资源不存在*/
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    /*判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态*/
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    /*判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误*/
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    /*以只读方式获取文件描述符，通过mmap将该文件映射到内存中m_file_address*/
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);              //避免文件描述符的浪费和占用
    return FILE_REQUEST;    //表示请求文件存在，且可以访问
}

/*对映射内存区执行munmap操作*/
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);    //解除对内存映射区的映射
        m_file_address = 0;
    }
}

/*往写缓冲m_write_buf中写入待发送的数据*/
bool http_conn::add_response(const char *format, ...)
{
    /*如果写入内容超出m_write_buf大小则报错*/
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;

    va_list arg_list;           //定义可变参数列表。用于访问不定数量的参数。
    va_start(arg_list, format); //将变量arg_list初始化为传入参数。
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list); //将数据format从可变参数列表写入写缓冲区，返回写入数据的长度
    /*如果写入的数据长度超过缓冲区剩余空间，则报错*/
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);       //结束对参数列表arg_list的访问，释放由va_start宏初始化的资源。
        return false;
    }
    m_write_idx += len;     //更新m_write_idx位置
    va_end(arg_list);       //清空可变参列表
    LOG_INFO("request:%s", m_write_buf);
    Log::get_instance()->flush();
    return true;
}

/*添加状态行*/
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title); //将数据写入写缓冲区
}
/*添加消息报头，具体的添加文本长度、连接状态和空行*/
bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}
/*添加Content-Length，表示响应报文的长度*/
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
/*添加文本类型，这里是html*/
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
/*添加连接状态，通知浏览器端是保持连接还是关闭*/
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
/*添加空行*/
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
/*添加文本content*/
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}


/*根据服务器处理HTTP请求(process_read()，即do_request())的结果，调用以上函数，向m_write_buf中填充响应报文，决定返回给客户端的内容*/
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    /*服务器内部错误，500*/
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);  //状态行
        add_headers(strlen(error_500_form));    //消息报头
        if (!add_content(error_500_form))
            return false;
        break;
    }
    /*客户请求有语法错误，404*/
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    /*客户对资源没有足够的访问权限，403*/
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    /*文件存在，200*/
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        /*如果请求的资源存在*/
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            /*第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx*/
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            /*第二个iovec指针指向mmap返回的目标文件（请求资源）指针，长度指向文件大小*/
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;  //发送的全部数据为响应报文头部信息和目标文件资源大小
            return true;
        }
        /**/
        else
        {
            /*如果请求的资源大小为0，则返回空白html文件*/
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    /*除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区*/
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

/*写HTTP响应*/
bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            /*如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件。虽然在此期间，服务器无法
            立即接收到同一客户的下一个请求，但这可以保证连接的完整性*/
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        /*第一个iovec头部信息的数据已发送完，发送第二个iovec数据*/
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            /*不再继续发送头部信息*/
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        /*继续发送第一个iovec头部信息的数据*/
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        /*判断条件，数据已全部发送完*/
        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);  //在epoll树上重置EPOLLONESHOT事件

            /*浏览器的请求为长连接*/
            if (m_linger)
            {
                init();     //重新初始化HTTP对象
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
