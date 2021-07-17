/**
 *        <header comment>
 *      Student Name : 董翌飞
 *      Student ID   : 1800017771
 *      Description  : 使用读者-写者模型和LRU策略的proxy 
 */

#include <stdio.h>
#include <ctype.h>
#include "csapp.h"
#include "sbuf.h"

//报头
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *connect_hdr = "Connection: close\r\n";
static const char *proxy_hdr = "Proxy-Connection: close\r\n";

static const char *connection_key = "Connection";
static const char *user_agent_key= "User-Agent";
static const char *proxy_connection_key = "Proxy-Connection";
static const char *host_key = "Host";

//宏
#define EOF_TYPE 1
#define HOST_TYPE 2
#define OTHER_TYPE 3
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define CACHE_NUM 11
#define NTHREADS 256
#define SBUFSIZE 256

int getType(char *buf);
void doit(int connfd);
void parseUri(char uri[],char hostname[],char path[],char port[]);
void build_http
(char *server_http,char *hostname,char*path,char* port,rio_t *clientrio);
void* thread(void *vargp);
void initCache();
void preRead(int index);
void afterRead(int index);
void preWrite(int index);
void afterWrite(int index);
int findCache(char *url);
void updateLRU(int index);
int findSuitCache();
void writeCacheContent(char *url,char* buf);

//cache块
typedef struct {
    char content[MAX_OBJECT_SIZE];
    char url[MAXLINE];
    int time;
    int isEmpty;
    int readCount;
    int writeCount;
    sem_t mutex;
    sem_t w;
}cacheunit;

//具有10个slot的cache
typedef struct {
    cacheunit cacheUnit[CACHE_NUM];
}Cache;

Cache cache;
int allTime=0;  //LRU记录时间
sem_t flag;     //读者-写者模型中的信号量
sbuf_t sbuf;

//main
int main(int argc,char **argv)
{
    int listenfd,connfd;
    char hostname[MAXLINE],port[MAXLINE];
    socklen_t clientlen;
    struct  sockaddr_storage clientaddr;
    pthread_t tid;
    Signal(SIGPIPE, SIG_IGN);
    Sem_init(&flag,0,1);
    // the cmd parameters fail
    if (argc!=2){
        fprintf(stderr, "usage: %s <port>\n",argv[0]);
        exit(1);
    }
    initCache();
    listenfd= open_listenfd(argv[1]);
    sbuf_init(&sbuf, SBUFSIZE);
    for(int i=0;i<NTHREADS;i++){
        Pthread_create(&tid, NULL, thread, NULL);
    }
    while (1)
    {
        clientlen=sizeof(clientaddr);
        connfd=Accept(listenfd,(SA*)&clientaddr,&clientlen);
        Getnameinfo((SA*)&clientaddr,clientlen,hostname,MAXLINE,
                    port,MAXLINE,0);
        printf("Accepted connection from (%s , %s)\n",hostname,port);
        sbuf_insert(&sbuf, connfd);
    }
}

//建立新线程
void *thread(void *vargo)
{
    Pthread_detach(pthread_self());
    while(1){
        int connfd = sbuf_remove(&sbuf);
        doit(connfd);
        Close(connfd);
    }
    return NULL;
}

//获取，解析命令，寻找cache，输出报头，建立与服务器连接并返回内容
void doit(int connfd)
{
    int serverFd;
    rio_t clientrio,serverrio;
    char server_http[MAXLINE],buf[MAXLINE],method[MAXLINE],path[MAXLINE];
    char port[MAXLINE],uri[MAXLINE],version[MAXLINE],hostname[MAXLINE];
    
    P(&flag);
    allTime+=1;
    V(&flag);

    rio_readinitb(&clientrio,connfd);
    rio_readlineb(&clientrio,buf,MAXLINE);
    sscanf(buf,"%s %s %s",method,uri,version);

    if (strcasecmp(method,"GET")){
        printf("Proxy does not implement this method");
        return;
    }

    int cacheIndex;
    if ((cacheIndex=findCache(uri))>0)
    {
        preRead(cacheIndex);
        rio_writen(connfd,cache.cacheUnit[cacheIndex].content,
            strlen(cache.cacheUnit[cacheIndex].content));
        printf("the proxy has received %lu bytes\n",
            strlen(cache.cacheUnit[cacheIndex].content));
        afterRead(cacheIndex);
        updateLRU(cacheIndex);
    }

    parseUri(uri,hostname,path,port);
    build_http(server_http,hostname,path,port,&clientrio);
    serverFd=open_clientfd(hostname,port);
    if (serverFd<0){
        printf("connection failed\n");
        return;
    }
    rio_readinitb(&serverrio,serverFd);
    rio_writen(serverFd,server_http,strlen(server_http));

    size_t len;
    size_t allCount=0;
    char cacheBuf[MAX_OBJECT_SIZE];
    while ((len=rio_readlineb(&serverrio,buf,MAXLINE))!=0)
    {
        allCount+=len;
        if (allCount<MAX_OBJECT_SIZE)
            strcat(cacheBuf,buf);
        rio_writen(connfd,buf,len);
    }
    printf("the proxy has received %lu bytes\n",allCount);
    Close(serverFd);
    if (allCount<MAX_OBJECT_SIZE)
        writeCacheContent(uri,cacheBuf);
}

//解析uri
void parseUri(char uri[],char hostname[],char path[],char port[])
{
    //get the hostname from uri
    int i=0;
    char* hostnamePos=strstr(uri,"//");
    if (hostnamePos!=NULL)
        hostnamePos+=2;
    else
        hostnamePos=uri;
    strcpy(hostname,hostnamePos);
    int len=strlen(hostname);
    for (i=0;i<len;++i){
        if (hostname[i]=='/'||hostname[i]==':'){
            hostname[i]='\0';
            break;
        }
    }
    
    //get the port from uri
    char* portPos=strchr(hostnamePos,':');
    if (portPos!=NULL){
        strcpy(port,portPos+1);
        len=strlen(port);
        for (i=0;i<len;++i){
            if (!isdigit(port[i])){
                port[i]='\0';
                break;
            }
        }
    }
    else{
        port[0]='8';port[1]='0';port[2]='\0';
    }

    //get the path from uri
    char *pathPos=strstr(uri,"//");
    if (pathPos!=NULL)
        pathPos+=2;
    else
        pathPos=uri;
    pathPos=strchr(pathPos,'/');
    strcpy(path,pathPos);
    char *endpos = strchr(path,':');
    if (endpos!=NULL){
        (*endpos)='\0';
    }
    printf("hostname:%s\npath:%s\nport:%s\n",hostname,path,port);
    return;
}

//向服务器发送报头
void build_http
(char *server_http,char *hostname,char*path,char* port,rio_t *clientrio)
{
    char requestLine[MAXLINE],buf[MAXLINE];
    char host_hdr[MAXLINE],other_hdr[MAXLINE];
    //build the main string to get the service
    sprintf(requestLine,"GET %s HTTP/1.0\r\n",path);
    while (rio_readlineb(clientrio,buf,MAXLINE)>0)
    {
        int type = getType(buf);
        if (type==EOF_TYPE)
            break;
        else if (type==HOST_TYPE){
            strcpy(host_hdr,buf);
        }
        else{
            strcat(other_hdr,buf);
        }
    }
    if (strlen(host_hdr)==0)
        sprintf(host_hdr,"Host: %s\r\n",hostname);
    sprintf(server_http,"%s%s%s%s%s%s\r\n",
            requestLine,
            host_hdr,
            connect_hdr,
            proxy_hdr,
            user_agent_hdr,
            other_hdr);
    return;
}

//判断client发来的信息类型
int getType(char *buf)
{
    if(strcmp(buf,"\r\n")==0)
        return EOF_TYPE;
    else if (!strncasecmp(buf,host_key,strlen(host_key)))
        return HOST_TYPE;
    else if (strncasecmp(buf,connection_key,strlen(connection_key))
             &&strncasecmp(buf,proxy_connection_key,strlen(proxy_connection_key))
             &&strncasecmp(buf,user_agent_key,strlen(user_agent_key)))
        return OTHER_TYPE;
    return 0;
}

//初始化
void initCache()
{
    int i = 0;
    for (i = 0; i < CACHE_NUM; ++i)
    {
        cache.cacheUnit[i].isEmpty=1;
        cache.cacheUnit[i].time=0;
        Sem_init(&cache.cacheUnit[i].mutex,0,1);
        Sem_init(&cache.cacheUnit[i].w,0,1);
        cache.cacheUnit[i].readCount=0;
        cache.cacheUnit[i].writeCount=0;
    }
}

// 以下四个函数是包装好的读者-写者模型
void preRead(int index)
{
    P(&cache.cacheUnit[index].mutex);
    cache.cacheUnit[index].readCount++;
    if (cache.cacheUnit[index].readCount==1)
        preWrite(index);
    V(&cache.cacheUnit[index].mutex);
}

void afterRead(int index)
{
    P(&cache.cacheUnit[index].mutex);
    cache.cacheUnit[index].readCount--;
    if (cache.cacheUnit[index].readCount==0)
        afterWrite(index);
    V(&cache.cacheUnit[index].mutex);
}

void preWrite(int index)
{
    P(&cache.cacheUnit[index].w);
}

void afterWrite(int index)
{
    V(&cache.cacheUnit[index].w);
}

//通过url寻找cache
int findCache(char *url)
{
    int i,result=-1;
    for (i=0;i<CACHE_NUM;++i){
        preRead(i);
        if ((cache.cacheUnit[i].isEmpty==0)&&
            strcmp(cache.cacheUnit[i].url,url)==0)
            result=i;
        afterRead(i);
    }
    return result;
}

//更新LRU的时间值
void updateLRU(int index)
{
    preWrite(index);
    cache.cacheUnit[index].time=allTime;
    afterWrite(index);
}

//找到并驱逐某个cache
int findSuitCache()
{
    int i,result=-1,minTime=0x7fffffff;
    for (i=0;i<CACHE_NUM;++i){
        preRead(i);
        if (cache.cacheUnit[i].isEmpty){
            result=i;
        }
        afterRead(i);
        if (result==i)
            break;
    }
    if (result!=-1)
        return result;
    for (i=0;i<CACHE_NUM;++i){
        preRead(i);
        if (cache.cacheUnit[i].time<minTime){
            minTime=cache.cacheUnit[i].time;
            result=i;
        }
        afterRead(i);
    }
    return result;
}

//将信息写入cache
void writeCacheContent(char *url,char* buf)
{
    int index = findSuitCache();
    preWrite(index);
    //strcpy(cache.cacheUnit[index].content,buf);
    memcpy(cache.cacheUnit[index].content, buf, MAX_OBJECT_SIZE);
    //strcpy(cache.cacheUnit[index].url,url);
    memcpy(cache.cacheUnit[index].url, url, MAXLINE);
    cache.cacheUnit[index].isEmpty=0;
    afterWrite(index);
    updateLRU(index);
}

