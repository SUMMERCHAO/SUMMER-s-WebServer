//课本p196页 升序定时器链表
#ifndef LST_TIMER
#define LST_TIMER

#include <time.h>
#include "../log/log.h"

class util_timer;               //前向声明

/*用户数据结构：客户端socket地址、socket文件描述符、读缓存和定时器*/
struct client_data
{
    sockaddr_in address;
    int sockfd;
    util_timer *timer;
};

/*定时器类*/
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire;                      //任务超时时间，使用绝对时间
    void (*cb_func)(client_data *);     //任务回调函数
    /*回调函数处理的客户数据，由定时器的执行者传递给回调函数*/
    client_data *user_data;
    util_timer *prev;                   //指向前一个定时器
    util_timer *next;                   //指向后一个定时器
};

/*定时器链表。它是一个升序、双向链表，且带有头结点和尾节点*/
class sort_timer_lst
{
public:
    sort_timer_lst() : head(NULL), tail(NULL) {}
    
    /*链表被销毁时，删除其中的所有定时器*/
    ~sort_timer_lst()
    {
        util_timer *tmp = head;
        while (tmp)
        {
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }
    
    /*将目标定时器timer添加到链表中*/
    void add_timer(util_timer *timer)
    {
        if (!timer)
        {
            return;
        }
        if (!head)      /*如果当前链表为空，则头结点和尾节点都是timer*/
        {
            head = tail = timer;
            return;
        }
        /*如果目标定时器的超时时间小于当前链表中所有定时器的超时时间，则把该定时器插入链表头部，作为链表新的头结点*/
        if (timer->expire < head->expire)
        {
            timer->next = head;
            head->prev = timer;
            head = timer;
            return;
        }
        /*否则接需要调用重载函数add_timer，把它插入到链表中合适的位置，以保证链表的升序特性*/
        add_timer(timer, head);
    }
    
    /*调整目标定时器timer的位置：当每个定时任务发生变化时，调整对应的定时器在链表中的位置。
    这个函数只考虑被调整的定时器的超时时间延长的情况，即该定时器需要往尾端移动*/
    void adjust_timer(util_timer *timer)
    {
        if (!timer)
        {
            return;
        }
        util_timer *tmp = timer->next;
        /*如果被调整的定时器在链表尾部，或者定时器新的超时值仍然小于其下一个定时器的超时值，则不用调整*/
        if (!tmp || (timer->expire < tmp->expire))
        {
            return;
        }
        /*如果目标定时器是链表的头节点，则将该定时器取出并重新插入链表*/
        if (timer == head)
        {
            head = head->next;
            head->prev = NULL;
            timer->next = NULL;
            add_timer(timer, head);
        }
        /*其余情况，则将该定时器取出，然后插入其原来所在位置之后的部分链表中*/
        else
        {
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            add_timer(timer, timer->next);
        }
    }
    
    /*将目标定时器timer从链表中删除*/
    void del_timer(util_timer *timer)
    {
        if (!timer)
        {
            return;
        }
        /*如果链表中只有一个定时器，即目标定时器*/
        if ((timer == head) && (timer == tail))
        {
            delete timer;
            head = NULL;
            tail = NULL;
            return;
        }
        /*如果链表中至少有两个定时器，且目标定时器是链表的头节点，则将链表的头节点重置为元原头节点的下一个节点，然后删除目标定时器*/
        if (timer == head)
        {
            head = head->next;
            head->prev = NULL;
            delete timer;
            return;
        }
        /*如果链表中至少有两个定时器，且目标定时器是链表的尾节点，则将链表的尾节点重置为元原尾节点的上一个节点，然后删除目标定时器*/
        if (timer == tail)
        {
            tail = tail->prev;
            tail->next = NULL;
            delete timer;
            return;
        }
        /*如果目标定时器位于链表的中间，则把它前后的定时器串联起来，然后删除目标定时器*/
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }
    
    /*定时任务处理函数；SIGALRM信号每次被触发就再其信号处理函数中执行一次tick函数，以处理链表上到期的任务*/
    void tick()
    {
        if (!head)
        {
            return;
        }
        //printf( "timer tick\n" );
        LOG_INFO("%s", "timer tick");
        Log::get_instance()->flush();
        
        time_t cur = time(NULL);     /*获得系统当前的绝对时间*/
        util_timer *tmp = head;
        /*从头节点依次处理每个定时器，直到遇到一个尚未过期的定时器，这就是定时器的核心逻辑*/
        while (tmp)
        {
            if (cur < tmp->expire)
            {
                break;
            }
            /*调用定时器的回调函数，以执行定时任务（回调函数：删除非活动连接在socket上的注册事件，并关闭）*/
            tmp->cb_func(tmp->user_data);
            /*执行完定时器中的定时任务之后，就将他从链表中删除，并重置链表头节点*/
            head = tmp->next;
            if (head)
            {
                head->prev = NULL;
            }
            delete tmp;
            tmp = head;
        }
    }

private:
    /*一个重载的辅助函数，私有成员，它被公有的add_timer函数和adjust_timer函数调用。该函数表示将目标定时器timer添加到节点lst_head之后的部分链表中*/
    void add_timer(util_timer *timer, util_timer *lst_head)
    {
        util_timer *prev = lst_head;
        util_timer *tmp = prev->next;
        /*遍历lst_head节点后的部分链表，直到找到一个超时时间大于目标定时器的超时时间的节点，并把目标定时器插入到该节点之前*/
        while (tmp)
        {
            if (timer->expire < tmp->expire)
            {
                prev->next = timer;
                timer->next = tmp;
                tmp->prev = timer;
                timer->prev = prev;
                break;
            }
            prev = tmp;
            tmp = tmp->next;
        }
        /*遍历完成之后仍未找到超时时间大于目标定时器timer超时时间的节点，则将timer插入到链表尾部，并将它设置为链表新的尾节点*/
        if (!tmp)
        {
            prev->next = timer;
            timer->prev = prev;
            timer->next = NULL;
            tail = timer;
        }
    }

private:
    util_timer *head;
    util_timer *tail;
};

#endif

