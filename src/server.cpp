#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <time.h>
#include <string.h>
#include <cerrno>
#include <fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>

#define MAX_EVENTS 1024
#define BUFFLEN 128
#define SERV_PORT 7890

/*事件结构体*/
struct myevent{
    int fd;  //关联的文件描述符
    int events; //EPOLLIN EPOLLOUT 监听的事件
    void *arg; //回调函数参数
    void (*call_back)(int fd, int events, void *arg); // call back (concrete event handler)
    int status;  // 1->listening  0->not
    char buf[BUFFLEN]; 
    int len;
    long last_active; 
};

int epollfd; //epoll文件描述符
struct myevent s_events[MAX_EVENTS + 1];
/*
@brief 设置myevent的参数 
@param ev 需要设置的myevent指针
@param fd handle 需要被处理的事件源
@param call_back 回调函数指针
@param arg 指向事件结构体的指针
*/
void eventset(struct myevent *ev, int fd, void (*call_back)(int,int,void *), void *arg){
    ev->fd = fd;
    ev->call_back = call_back;
    ev->events = 0;
    ev->arg = arg;
    ev->status = 0;
    // memset(ev->buf,0,sizeof(ev->buf));
    // ev->len = 0;
    ev->last_active = time(NULL);
}

void recvdata(int fd,int events, void *arg); //接收数据
void senddata(int fd,int events,void *arg); //发送数据
/*
@brief 向epoll中注册事件
@param efd epoll文件描述符
@param events 监听的事件 EPOLLIN/EPOLLOUT
@param ev 具体的事件指针
*/
void eventadd(int efd, int events,struct myevent *ev){
    /*struct epoll_event
        uint32_t events;	//Epoll events 
        epoll_data_t data;	// User data variable
    */
    struct epoll_event epv = {0,{0}};
    int op;
    epv.data.ptr = ev;
    epv.events = ev->events = events;

    if(ev->status == 1){
        op = EPOLL_CTL_MOD;
    }else{
        op = EPOLL_CTL_ADD;
        ev->status = 1;
    }
    if(epoll_ctl(efd,op,ev->fd,&epv) < 0){
        printf("event add failed [fd=%d],events[%d]\n",ev->fd,events);
    }else{
        printf("event add OK [fd=%d],events[%d]\n",ev->fd,events);
    }
    return;
}

/*
@brief 删除epoll中的指定事件
@param efd epoll文件描述符
@param ev 具体的事件指针
*/
void eventdel(int efd, struct myevent *ev){
    /*union epoll_data {
            void *ptr;        // 指针，用户自定义的数据
            int fd;          // 文件描述符
            uint32_t u32;    // 32 位无符号整型
            uint64_t u64;    // 64 位无符号整型
        };
    */
    struct epoll_event epv = {0,{0}};
    if(ev->status != 1)//必须是已经注册的事件才能删除
        return;
    epv.data.ptr = ev; //将epoll_data中的用户自定义数据设置为自定义的事件结构体
    ev->status = 0;
    epoll_ctl(efd,EPOLL_CTL_DEL,ev->fd,&epv); //删除事件
    return;
}
/*
@brief 接受客户端连接
@param lfd 监听套接字
@param events 没有实际用途 仅仅是实现回调函数的形式统一
@param arg 没有实际用途 仅仅是实现回调函数的形式统一
*/
void acceptconn(int lfd, int events, void *arg){
    sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    int cfd,i;
    cfd = accept(lfd, (sockaddr *)&client_addr,&len);
    if(cfd == -1){
        if(errno != EAGAIN && errno != EINTR){};
        printf("%s: accept, %s \n", __func__, strerror(errno));
        return;
    }

    do{
        for(i=0;i<MAX_EVENTS;i++){ //查找事件数组中未监听的并退出
            if(s_events[i].status == 0){
                break;
            }
        }
        if(i==MAX_EVENTS){  //如果所有事件都监听，则没有空间容纳新的连接
            printf("%s:max connect limit[%d]", __func__, MAX_EVENTS);
            break;
        }
        int flag = 0;
        if((flag = fcntl(cfd, F_SETFL, O_NONBLOCK)) < 0){ //将客户端fd设置为非阻塞形式
            printf("%s, fcntl nonblocking failed, %s\n",__func__, strerror(errno));
            break;
        }
        eventset(&s_events[i], cfd, recvdata, &s_events[i]); //设置事件 新连接达到先设置为接受数据事件
        eventadd(epollfd, EPOLLIN, &s_events[i]);//向epoll中注册事件
    }while(0);
    printf("new connect [%s:%d][time:%ld], pos[%d]\n",inet_ntoa(client_addr.sin_addr),ntohs(client_addr.sin_port),s_events[i].last_active,i);
    return;
}

/*
@brief 从指定套接字接受数据
@fd 从fd中获取数据
@events 事件
@arg 指向事件结构体的指针
*/
void recvdata(int fd, int events,void *arg){
    myevent *ev = (myevent *)arg;
    int len;
    len = recv(fd, ev->buf, sizeof(ev->buf), 0);
    eventdel(epollfd, ev);
    if(len > 0){
        ev->len = len;
        ev->buf[len] = '\0';
        printf("C[%d]:%s\n",fd,ev->buf);
        eventset(ev,fd,senddata,ev); //接受完数据 对事件进行修改 回调函数换为发送数据
        eventadd(epollfd,EPOLLOUT,ev);
    }else if(len==0){
        close(ev->fd);
        printf("[fd=%d] pos=[%d] closed\n", fd, (int)(ev - s_events));
    }else{
        close(ev->fd);
        printf("recv[fd=%d] error[%d]:%s\n",fd,errno,strerror(errno));
    }
    return;
}

/*
@brief 发送数据
@param fd 套接字
@param events 事件
@param arg 具体的事件结构体指针
*/
void senddata(int fd, int events, void *arg){
    myevent * ev = (myevent*)arg;
    int len;

    len = send(fd, ev->buf, ev->len, 0);
    eventdel(epollfd, ev);
    if(len > 0){
        printf("send[fd=%d],[%d]%s\n",fd,len,ev->buf);
        eventset(ev,fd,recvdata,ev);
        eventadd(epollfd, EPOLLIN, ev);
    }else{
        close(ev->fd);
        printf("send[fd=%d] error %s\n",fd,strerror(errno));
    }
    return;
}

/*
@brief 初始化监听套接字
@param efd epoll描述符
@param port 监听端口 
*/
void initlistensocket(int efd, short port){
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(lfd,F_SETFL,O_NONBLOCK);
    eventset(&s_events[MAX_EVENTS], lfd,acceptconn,&s_events[MAX_EVENTS]);
    eventadd(epollfd, EPOLLIN,&s_events[MAX_EVENTS]); //对于监听套接字只监听EPOLLIN套接字 回调为接受连接请求

    sockaddr_in server_addr;
    memset(&server_addr,0,sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    bind(lfd,(sockaddr *)&server_addr,sizeof(server_addr));
    listen(lfd,20);
    return;
}

int main(){
    short port = SERV_PORT;
    epollfd = epoll_create(MAX_EVENTS + 1);

    if(epollfd<=0){
        printf("create epollfd in %s error %s\n",__func__, strerror(errno));
    }
    initlistensocket(epollfd, port);
    epoll_event events[MAX_EVENTS + 1];
    printf("server running:port[%d]\n",port);
    int checkpos = 0,i;
    while(1){
        //超时验证  每次验证100个 防止验证时间过长
        long now = time(NULL);
        for(i=0;i<100;i++,checkpos++){
            if(checkpos == MAX_EVENTS)
                checkpos = 0;
            if(s_events[checkpos].status != 1) continue;
            long duration = now - s_events[checkpos].last_active;
            if(duration >= 60){
                close(s_events[checkpos].fd);
                printf("[fd=%d] timeout\n",s_events[checkpos].fd);
                eventdel(epollfd, &s_events[checkpos]);
            }
        }
        //等待事件发生
        int nfd = epoll_wait(epollfd, events,MAX_EVENTS + 1, 1000);
        if(nfd < 0){
            printf("epoll_wait error, exit\n");
            break;
        }
        for(i=0;i<nfd;i++){
            myevent *ev = (myevent *)events[i].data.ptr;
            if((events[i].events & EPOLLIN) && (ev->events & EPOLLIN)){
                ev->call_back(ev->fd,events[i].events,ev->arg);
            }
            if((events[i].events & EPOLLOUT) && (ev->events & EPOLLOUT)){
                ev->call_back(ev->fd,events[i].events,ev->arg);
            }
        }
    }
    close(epollfd);
    return 0;
}