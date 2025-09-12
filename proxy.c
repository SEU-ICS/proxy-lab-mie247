#include <stdio.h>
#include <stdarg.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define MAX_CACHE_NUM 10
//
/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

struct url_t{
    char host[MAXLINE], port[MAXLINE], path[MAXLINE];
};

struct cache_t{
    char url[MAXLINE];
    char content[MAX_OBJECT_SIZE];
    int content_size;
    int timestamp;
};

static struct cache_t cache[MAX_CACHE_NUM];
static int cache_used;
static sem_t mutex, w;
static int readcnt,timestamp;


void output_to_file(char* filename,const char *format, ...) {
    FILE *file = fopen(filename, "a");
    if (file == NULL) {
        perror("Error opening output.txt");
        return;
    }
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fclose(file);
}


void init_cache() {
    timestamp = 0;
    readcnt = 0;
    cache_used = 0;
    sem_init(&mutex, 0, 1);
    sem_init(&w, 0, 1);
}

int query_cache(char *url, rio_t* rio_p) {
    P(&mutex);
    readcnt++;
    if (readcnt == 1) {//读者优先：第一个读者出现时，占用读写锁
        P(&w);
    }
    V(&mutex);

    output_to_file("output.txt", "\n[-1]\n%s\n",url);
    int index = -1;
    for (int i = 0; i < cache_used;i++) {
        output_to_file("output.txt", "\n[%d]\n%s\n", i ,cache[i].url);
        if (strcmp(cache[i].url, url)==0) {
            //若命中，更新时间戳
            P(&mutex);
            cache[i].timestamp = timestamp++;
            V(&mutex);
            
            Rio_writen(rio_p->rio_fd, cache[i].content, cache[i].content_size);//直接在此处向客户端发送内容
            char *p=cache[i].content+ cache[i].content_size;
            p ='\0';
            output_to_file("output1.txt" ,"%s\n", cache[i].content);
            index = i;
            break;
        }
    }

    P(&mutex);
    readcnt--;
    if (readcnt == 0) {//若没有读者，释放读写锁
        V(&w);
    }
    V(&mutex);

    if (index>=0) {
        return index;
    }
    return -1;
}

int add_cache(char *url, char *content, int content_size) {
    P(&w);
    if (cache_used == MAX_CACHE_NUM - 1) {
        // 若缓存已满，替换掉最久未使用的时间戳
        int oldest_index;
        int oldest_timestamp = timestamp;
        for (int i = 0;i < cache_used;i++) {
            if (cache[i].timestamp < oldest_timestamp) {
                oldest_timestamp = cache[i].timestamp;
                oldest_index = i;
            }
        }
        strcpy(cache[oldest_index].url, url);
        memcpy(cache[oldest_index].content, content, content_size);
        cache[oldest_index].content_size = content_size;
        //更新时间戳
        P(&mutex);
        cache[oldest_index].timestamp = timestamp++;
        V(&mutex);
    }else{
        // 添加缓存
        output_to_file("output.txt", "[ori_url]%s\n", url);
        strcpy(cache[cache_used].url, url);
        output_to_file("output.txt", "[cac_url]%s\n", cache[cache_used].url);
        memcpy(cache[cache_used].content, content, content_size);
        cache[cache_used].content_size = content_size;
        // 更新时间戳
        P(&mutex);
        cache[cache_used].timestamp = timestamp++;
        cache_used++;
        V(&mutex);
    }
    V(&w);
    return 0;
}

// url:[http://][(host)][:XX(port)][/.../...(path)] 

int parse_url(char *url, struct url_t *url_info){
    if(strncasecmp(url, "http://", strlen("http://")) != 0){//没有http协议头，无效
        return -1;
    }
    char *hostp=url+strlen("http://");
    char *portp=strchr(hostp,':');
    char *pathp=strchr(hostp,'/');

    if(pathp==NULL){//没有路径，无效
        return -1;
    }

    if(portp==NULL){
        strcpy(url_info->port,"80");
        *pathp='\0';
        strcpy(url_info->host,hostp);
        *pathp='/';
    }else{
        *pathp='\0';
        strcpy(url_info->port,portp+1);
        *pathp='/';
        *portp='\0';
        strcpy(url_info->host,hostp);
        *portp=':';
    }
    strcpy(url_info->path,pathp);
    return 0;
}

int parse_request(rio_t* client_riop, struct url_t *url_info, char* request_info){
    char buffer[MAXLINE];
    int has_host=0;

    sprintf(request_info,"GET %s HTTP/1.0\r\n",url_info->path);//请求头

    while (1) {
        rio_readlineb(client_riop, buffer, MAXLINE);//每次读取一行
        if (strcmp(buffer, "\r\n") == 0) {//结束行
            break;
        }
        if(!strncasecmp(buffer, "Host:", strlen("Host:"))){
            has_host=1;
        }
        if(!strncasecmp(buffer, "User-Agent:", strlen("User-Agent:"))){
            continue;
        }
        if(!strncasecmp(buffer, "Connection:", strlen("Connection:"))){
            continue;
        }
        if(!strncasecmp(buffer, "Proxy-Connection:", strlen("Proxy-Connection:"))){
            continue;
        }
        strcat(request_info, buffer);
    }

    if (has_host==0) {
        sprintf(buffer, "Host: %s\r\n", url_info->host);
        strcat(request_info, buffer);
    }
    strcat(request_info, user_agent_hdr);
    strcat(request_info, "Connection: close\r\n");
    strcat(request_info, "Proxy-Connection: close\r\n");
    strcat(request_info, "\r\n");
    return 0;
}

void work(int clientfd){
    int serverfd;
    rio_t client_rio, server_rio;
    
    char buffer[MAXLINE],method[MAXLINE],url[MAXLINE],http_version[MAXLINE],request_info[MAXLINE];
    struct url_t url_info;

    Rio_readinitb(&client_rio,clientfd);
    Rio_readlineb(&client_rio,buffer,MAXLINE);

    sscanf(buffer,"%s %s %s",method,url,http_version);
    if(strcasecmp(method,"GET")!=0){// 不是GET请求，不处理
        return;
    }
    output_to_file("output.txt", "[que_url]%s\n", url);
    if (query_cache(url, &client_rio)>=0) {//若命中缓存
        return;
    }
    if(parse_url(url, &url_info) < 0){// url解析失败
        return;
    }
    parse_request(&client_rio,&url_info,request_info);

    serverfd = Open_clientfd(url_info.host,url_info.port);
    if (serverfd < 0) {// 连接到服务器失败
        return;
    }
    //发送请求
    Rio_writen(serverfd, request_info, strlen(request_info));
    //接收响应
    int resp_total=0,resp_current=0;
    char resp_cache[MAX_OBJECT_SIZE];
    Rio_readinitb(&server_rio,serverfd);
    while((resp_current = Rio_readlineb(&server_rio,buffer,MAXLINE))){
        Rio_writen(clientfd,buffer,resp_current);
        
        if (resp_total+resp_current < MAX_OBJECT_SIZE) {
            memcpy(resp_cache + resp_total, buffer, resp_current);
        }
        resp_total+=resp_current;

        char* p=buffer+resp_current;
        p='\0';
        // output_to_file("output.txt", "%s", buffer);
    }
    // output_to_file("output.txt","\n");
    //写入缓存
    if (resp_total < MAX_OBJECT_SIZE) {
        add_cache(url, resp_cache, resp_total);
    }
}

void *thread(void *vargp) {
    int connfd = *((int *)vargp);
    Pthread_detach(pthread_self());
    Free(vargp);
    work(connfd);
    Close(connfd);
    return NULL;
}

int main(int argc,char **argv)
{
    init_cache();

    int listenfd, *connfdp;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(0);
    }
    listenfd = Open_listenfd(argv[1]);

    while (1) {
        clientlen = sizeof(clientaddr);
        connfdp = Malloc(sizeof(int));
        *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Pthread_create(&tid, NULL, thread, connfdp);
    }
    return 0;
}
