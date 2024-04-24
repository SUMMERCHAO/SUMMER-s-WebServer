#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
using namespace std;

Log::Log()
{
    m_count = 0;
    m_is_async = false;
}

Log::~Log()
{
    if (m_fp != NULL)
    {
        fclose(m_fp);
    }
}
/*日志库的初始化函数。init函数实现日志创建、写入方式的判断。异步需要设置阻塞队列的长度，同步不需要设置*/
bool Log::init(const char *file_name, int log_buf_size, int split_lines, int max_queue_size)
{
    /*如果设置了max_queue_size,则设置为异步，需要创建并设置阻塞队列长度*/
    if (max_queue_size >= 1)
    {
        m_is_async = true;      //设置写入方式flag
        m_log_queue = new block_queue<string>(max_queue_size);  //创建并设置阻塞队列长度,用于存储日志信息
        pthread_t tid;      //启动一个线程来处理、写入这些异步日志信息

        pthread_create(&tid, NULL, flush_log_thread, NULL);     //flush_log_thread为回调函数,这里表示创建线程异步写日志
    }

    /*初始化用于存储日志内容的缓冲区*/
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);
    m_split_lines = split_lines;    //日志的最大行数

    /*据当前时间和给定的文件名构造日志文件的全名*/
    time_t t = time(NULL);              //获取当前的系统时间
    struct tm *sys_tm = localtime(&t);  //将 time_t 类型变量转换为本地时间的表示，并得到一个 struct tm 类型的指针。
    struct tm my_tm = *sys_tm;

    const char *p = strrchr(file_name, '/');    //从后往前找到第一个/的位置
    char log_full_name[256] = {0};

    /*相当于自定义日志名,若输入的文件名没有/，则直接将时间+文件名作为日志名*/
    if (p == NULL)
    {
        /*tm_year — 年份，从1900年开始的年数;tm_mon — 月份，从0开始到11 (0代表1月，11代表12月);tm_mday — 一个月中的日期，取值范围为1到31*/
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }
    else
    {
        /*将/的位置向后移动一个位置，然后复制到logname中
        p - file_name + 1是文件所在路径文件夹的长度
        dirname相当于./ */
        strcpy(log_name, p + 1);
        strncpy(dir_name, file_name, p - file_name + 1);
        /*后面的参数跟format有关*/
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;

    m_fp = fopen(log_full_name, "a");
    if (m_fp == NULL)
    {
        return false;
    }

    return true;
}

/*日志写入函数。write_log函数完成写入日志文件中的具体内容，主要实现日志分级、分文件、格式化输出内容。*/
void Log::write_log(int level, const char *format, ...)
{
    /*首先，通过 gettimeofday 函数获取当前的精确时间（秒数和微秒数）*/
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    /*转换得到的时间 t 到本地时间 sys_tm，然后复制到 my_tm*/    
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16] = {0};
    /*根据传入的 level 参数，设置日志等级字符串s*/
    switch (level)
    {
    case 0:
        strcpy(s, "[debug]:");
        break;
    case 1:
        strcpy(s, "[info]:");
        break;
    case 2:
        strcpy(s, "[warn]:");
        break;
    case 3:
        strcpy(s, "[erro]:");
        break;
    default:
        strcpy(s, "[info]:");
        break;
    }

    /*上锁，记录写入的日志数量 m_count，并判断是否需要创建新的日志文件*/
    m_mutex.lock();
    m_count++;      //写入一个log，对m_count++

    /*如果是新的一天或者已写入的日志行数达到了最大行数的倍数，则需要创建新的日志文件；m_split_lines为最大行数*/
    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0)
    {
        char new_log[256] = {0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};
       
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday); //格式化日志名中的时间部分
       
        /*如果是时间不是今天,则创建今天的日志，更新m_today和m_count*/
        if (m_today != my_tm.tm_mday)
        {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        }
        /*如果是因为已写入最大行数，添加一个后缀到日志文件名表示不同的日志文件。*/
        else
        {
            /*超过了最大行，在之前的日志名基础上加后缀, m_count/m_split_lines*/
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        m_fp = fopen(new_log, "a");
    }

    /*解锁，之后使用 va_list 和 va_start 来处理带有可变参数的情况。*/
    m_mutex.unlock();
    va_list valst;
    va_start(valst, format);    //将传入的format参数赋值给valst，便于格式化输出

    string log_str;

    /*再次上锁，并使用 snprintf 和 vsnprintf 格式化前缀（时间戳和日志等级）和传入的格式化内容到缓冲区 m_buf*/
    m_mutex.lock();
    /*写入内容格式：时间 + 内容; 时间格式化，snprintf成功返回写字符的总数，其中不包括结尾的null字符*/
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    
    /*内容格式化，用于向字符串中打印数据、数据格式用户自定义，返回写入到字符数组str中的字符个数(不包含终止符)*/
    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, valst);
    /*添加换行到缓冲区并结束字符串*/
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    /*若m_is_async为true表示异步，默认为同步
    若异步且日志队列没有满,则将日志信息加入阻塞队列m_log_queue。同步或队列满了，则直接写入到文件。*/
    if (m_is_async && !m_log_queue->full())
    {
        m_log_queue->push(log_str);
    }
    /*解锁，并使用 va_end 结束可变参数的处理。*/
    else
    {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst);
}

void Log::flush(void)
{
    m_mutex.lock();
    //强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}
