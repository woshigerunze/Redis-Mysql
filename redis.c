#include <stdio.h>
#include <string.h>
#include <hiredis/hiredis.h>
#include <unp.h>
#include <pthread.h>
#include <mysql.h>




//---------------------------------------------------全局变量
int listenfd;//监听套接字
int nowconnect=0;//当前连接人数
int socketfd[100];//连接队列
const int maxsize=100;//最大连接数
redisContext* context;//redis连接句柄
MYSQL* conn;
MYSQL_RES* res;
MYSQL_ROW row;
struct sockaddr_in servaddr;//服务器信息
//---------------------------------------------------全局变量

void init()
{
    listenfd=Socket(AF_INET,SOCK_STREAM,0);
    bzero(&servaddr,sizeof(servaddr));
    servaddr.sin_family=AF_INET;
    servaddr.sin_port=htons(6666);
    servaddr.sin_addr.s_addr=htonl(INADDR_ANY);
    Bind(listenfd,(SA*)&servaddr,sizeof(servaddr));
    Listen(listenfd,100);
    printf("Server argument initialized successful!\n");
}
void redisconnect()
{
    context = redisConnect("127.0.0.1", 6379);//默认端口，本机redis-server服务开启
    if(context->err)
    {
        redisFree(context);
        printf("connect redisServer err:%s\n", context->errstr);
        return ;
    }
    
    printf("Connect redisServer success!\n");
}
void mysqlconnect()
{
    conn=mysql_init(NULL);
    if(mysql_real_connect(conn,"localhost","root","geliang8888","book",0,NULL,0))
    printf("Connect mysqlServer success!\n");
}
int borrow(int fd)
 {
     char query[20];
     char temp[100];
     bzero(&temp,sizeof(temp));
     bzero(&query,sizeof(query));
     recv(fd,query,sizeof(query),0);
     redisReply *reply = (redisReply *)redisCommand(context, "get %s",query);
     //printf("get value %s\n",reply->str);
     if(NULL == reply)
        {
            printf("redis command err:%s\n",context->errstr);
            freeReplyObject(reply);
            return 0;
        }
     if(reply->type==REDIS_REPLY_NIL)
     {
         sprintf(temp,"select* from bookinfo where bookname='%s'",query);
         mysql_query(conn,temp);
         res=mysql_store_result(conn);
         if(res==NULL)
         {
             printf("res is NULL\n");
             freeReplyObject(reply);
             return 2;
         }
         else
         {
             printf("query from mysql!\n");
             row=mysql_fetch_row(res);
             if(row==NULL)
             {
                 freeReplyObject(reply);
                 return 2;
             }
             char* bookname=row[0];
             char* status=row[1];
             if(strcmp(status,"0")==0)
             {
                 redisCommand(context, "set %s %s",bookname,"0");
                 redisCommand(context, "expire %s %s",bookname,"10");
                 freeReplyObject(reply);
                 return 3;
             }
             bzero(&temp,sizeof(temp));
             sprintf(temp,"update bookinfo set status='%s' where bookname='%s'","0",bookname);
             mysql_query(conn, temp);
             redisCommand(context, "set %s %s",bookname,"0");
             redisCommand(context, "expire %s %s",bookname,"10");
             freeReplyObject(reply);
             return 1;
         }
     }
     if(strcmp(reply->str,"0")==0)
     {
         printf("query from redis!\n");
         freeReplyObject(reply);
         return 3;
     }
     printf("query from redis!\n");
     redisCommand(context, "set %s %s",query,"0");
     redisCommand(context, "expire %s 10",query);
     bzero(&temp,sizeof(temp));
     sprintf(temp,"update bookinfo set status='%s' where bookname='%s","0",query);
     mysql_query(conn, temp);
     freeReplyObject(reply);
     return 1;
 }
int giveback(int fd)
{
    char query[20];
    bzero(&query,sizeof(query));
    recv(fd,query,sizeof(query),0);
    redisReply *reply = (redisReply *)redisCommand(context, "get %s",query);
    //printf("get value %s\n",reply->str);
    if(NULL == reply)
    {
        printf("command execute failure1\n");
        redisFree(context);
        return 0;
    }
    if(reply->type==REDIS_REPLY_NIL)
    {
        freeReplyObject(reply);
        return 1;
    }
    if(strcmp(reply->str,"1")==0)
    {
        freeReplyObject(reply);
        return 2;
    }
    redisCommand(context, "set %s %s",query,"1");
    redisCommand(context, "expire %s 10",query);
    freeReplyObject(reply);
    return 3;
 }
void* userfunc(void* arg)
{
    int fd=(int)arg;
    while(1)
    {
        char query[10]={0};
        bzero(&query,sizeof(query));
        if(recv(fd,query,sizeof(query),0)<=0)
        {
            for(int i=0;i<maxsize;i++)
                if(fd==socketfd[i])
                {
                    socketfd[i]=0;
                    break;
                }
            printf("someone disconnected from server!\n");
            nowconnect--;
            Close(fd);
            pthread_exit((void*)0);
        }
        if(strcmp(query,"borrow")==0)
        {
            int res=borrow(fd);
            if(res==0)
            {
                char buf[]="操作失败!";
                send(fd,buf,strlen(buf),0);
            }
            else if(res==1)
            {
                char buf[]="成功借书,请阅读完后尽快归还！";
                send(fd,buf,strlen(buf),0);
            }
            else if(res==2)
            {
                char buf[]="书库暂时没有这本书!";
                send(fd,buf,strlen(buf),0);
            }
            else if(res==3)
            {
                char buf[]="此书已被借走!";
                send(fd,buf,strlen(buf),0);
            }
        }
        else if(strcmp(query,"giveback")==0)
        {
            int res=giveback(fd);
            if(res==0)
            {
                char buf[]="操作失败!";
                send(fd,buf,strlen(buf),0);
            }
            else if(res==1)
            {
                char buf[]="书库里没有这本书,操作失败!";
                send(fd,buf,strlen(buf),0);
            }
            else if(res==2)
            {
                char buf[]="此书没有被借,操作失败!";
                send(fd,buf,strlen(buf),0);
            }
            else if(res==3)
            {
                char buf[]="还书成功,欢迎下次借书!";
                send(fd,buf,strlen(buf),0);
            }
        }
    }
    return arg;
}
void service()
{
    printf("The server has started!\n");
    while(1)
    {
        struct sockaddr_in cliaddr;
        socklen_t len=sizeof(cliaddr);
        int fd=accept(listenfd,(SA*)&cliaddr,&len);
        if(fd==-1)
        {
            printf("客户端连接出错！\n");
            continue;
        }
        nowconnect++;
        if(nowconnect==maxsize)
        {
            char* str="当前连接数满了，请过会再连接!";
            send(fd,str,strlen(str),0);
            close(fd);
        }
        for(int i=0;i<maxsize;i++)
            if(socketfd[i]==0)
            {
                socketfd[i]=fd;
                printf("当前连接数:%d\n",nowconnect);
                pthread_t tid;
                pthread_create(&tid,NULL,userfunc,(void*)fd);
                break;
            }
    }
}
int main(void)
{
    init();
    mysqlconnect();
    redisconnect();
    service();
}

