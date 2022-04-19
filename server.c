/* 
 * tcpechosrv.c - A concurrent TCP echo server using threads
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>      /* for fgets */
#include <strings.h>     /* for bzero, bcopy */
#include <unistd.h>      /* for read, write */
#include <sys/socket.h>  /* for socket use */
#include <netdb.h>
#include <time.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define MAXLINE  8192  /* max text line length */
#define MAXBUF   8192  /* max I/O buffer size */
#define LISTENQ  1024  /* second argument to listen() */

int     open_listenfd(int port);
void    echo(int connfd);
void    *thread(void *vargp);
void    parse(char *sendbuf, int connfd);
FILE    *locate_file(char *fileString, char *mode);
void    get(char *sendbuf, int connfd, char *uri, int version, bool keepalive, bool headersonly);
void    post(char *sendbuf, int connfd, char *uri, char *postdata, int version, bool keepalive);
int     insert_header(char *buf, int httpversion, int status, char *type, int length, bool keepalive, int connfd);
char    *type_from_filename(char *filename);
int     len_from_fp(FILE *fp);
void    server_err(int connfd);

/*
Read the input port
Forever loop:
    - Accept connections on that port
    - Create a thread to service the connection, call the thread function on it
*/
int main(int argc, char **argv) 
{
    int listenfd, *connfdp, port, clientlen=sizeof(struct sockaddr_in);
    struct sockaddr_in clientaddr;
    pthread_t tid; 

    if (argc != 2) {
	    fprintf(stderr, "usage: %s <port>\n", argv[0]);
	    exit(0);
    }
    port = atoi(argv[1]);
    chdir("www");

    listenfd = open_listenfd(port);
    if (listenfd < 0){
        printf("Error getting listen FD\n");
        return 0;
    }
    while (1) {
        connfdp = malloc(sizeof(int));
        // Why is this a pointer?
        *connfdp = accept(listenfd, (struct sockaddr*)&clientaddr, (socklen_t *)&clientlen);
        pthread_create(&tid, NULL, thread, connfdp);
    }
}

/*
Thread routine
Acts on the connection pointer
Calls to echo
*/
void *thread(void *vargp) 
{  
    char sendbuf[MAXBUF];
    bzero(sendbuf, MAXBUF);
    int connfd = *((int *)vargp);
    pthread_detach(pthread_self());
    fcntl(connfd, F_SETFL, O_NONBLOCK);
    parse(&sendbuf[0], connfd);
    free(vargp); // shouldn't this be later?
    //echo(connfd);
    close(connfd);
    return NULL;
}

/*
Read request from the connection
Split into <Method> <URI> <version>
Call corresponding action
*/
void parse(char *sendbuf, int connfd){
    //printf("PARSE\n");
    ssize_t n; 
    char recvbuf[MAXBUF];
    char postdata[MAXBUF];
    char *option;
    char *remainder;
    char *method;
    char *uri;
    char *version;
    bool keepalive;
    int httpversion;
    int sockalive;
    clock_t lastmsg;
    clock_t checktime;
    int lastelapsed = 0;
    int elapsedtime;
    bool firstreceived = false;

    while(1) {
        bzero(recvbuf, MAXBUF);
        n = read(connfd, recvbuf, MAXBUF);
        if (n <= 0){
            
            // Keep looping if waiting for first message or waiting for keepalive timeout
            checktime = clock();
            elapsedtime = (checktime-lastmsg)/CLOCKS_PER_SEC;
            if (elapsedtime > lastelapsed){
                printf("Elapsed time: %i\n", elapsedtime);
                lastelapsed = elapsedtime;
            }
            if (!firstreceived || (keepalive && (elapsedtime < 10)))
                continue;
            else
                return;
        }
        firstreceived = true;
        printf("Socket %i received the following request:\n%s\n", connfd, recvbuf);
        lastmsg = clock();

        // Parse main request
        //strncpy(recvbuf, recvbuf, MAXBUF);
        method = strtok_r(&recvbuf[0], " ", &remainder);
        uri = strtok_r(NULL, " ", &remainder);
        version = strtok_r(NULL, " \n\r", &remainder);

        // Parse options for keep-alive
        option = strsep(&remainder, "\n");
        //option = strtok_r(NULL, "\n\r", &remainder);
        keepalive = false;
        while(option){
            if(strncmp(option, "Connection: Keep-alive", 22) == 0){
                keepalive = true;
                // sockalive = 1;
                // if(setsockopt(connfd, SOL_SOCKET, SO_KEEPALIVE, &sockalive, sizeof(sockalive)) < 0){
                //     perror("Error turning keepalive on");
                //     server_err(connfd);
                // }
            } else if (strncmp(option, "Connection: Close", 17) == 0){
                sockalive = 0;
                // if(setsockopt(connfd, SOL_SOCKET, SO_KEEPALIVE, &sockalive, sizeof(sockalive)) < 0){
                //     perror("Error turning keepalive off");
                //     server_err(connfd);
                // }
                // keepalive = false;
            } else if (option[0] == '\0' || (strncmp(option, "\r", 1) == 0)){
                strncpy(&postdata[0], remainder, strlen(remainder));
                break;
            }
            option = strsep(&remainder, "\n");
            //option = strtok_r(NULL, "\n\r", &remainder);
        }

        // Parse version
        if (strncmp(version, "HTTP/1.0", 8) == 0){
            httpversion = 10;
        } else if (strncmp(version, "HTTP/1.1", 8) == 0){
            httpversion = 11;
        } else {
            server_err(connfd);
        }

        // Replace default endpoints and clear leading slash
        if (((strlen(uri) == 1) && (strncmp(uri, "/", 1) == 0)) || (strncmp(uri, "/inside/", 8) == 0)){
            uri = "index.html"; // default page
        } else {
            uri++; // get rid of the leading slash
        }

        printf("Server recognized request [%s] [%s] [%s], keepalive=%i, httpversion=%i\n", 
            method, uri, version, keepalive, httpversion);

        if(strncmp(method, "GET", 3) == 0){
            get(sendbuf, connfd, uri, httpversion, keepalive, false);
        } else if (strncmp(method, "POST", 4) == 0){
            post(recvbuf, connfd, uri, postdata, httpversion, keepalive);
        } else if (strncmp(method, "HEAD", 4) == 0){
            get(sendbuf, connfd, uri, httpversion, keepalive, true);
        } else {
            printf("Unsupported HTTP method\n");
            server_err(connfd);
        }
        if (!keepalive)
            return;
    }
}

/*
 Place appropriate header in the send buffer
 */
int    insert_header(char *buf, int httpversion, int status, char *type, int length, bool keepalive, int connfd){
    //printf("HEADER\n");
    char *http;
    char *msg;
    char *connection;
    int written;

    if (httpversion == 10){
        http = "HTTP/1.0";
        connection = "";
    } else if (httpversion == 11){
        http = "HTTP/1.1";
        if (keepalive)
            connection = "\r\nConnection: Keep-alive";
        else
            connection = "\r\nConnection: Close";
    }

    if (status == 200)
        msg = "Document Follows";
    else if (status == 404)
        msg = "File Not Found";
    else 
        server_err(connfd);

    written = sprintf(buf, "%s %i %s\r\nContent-Type: %s\r\nContent-Length: %i%s\r\n\r\n", 
        http, status, msg, type, length, connection);
    return written;
}

/*
 Generic error
 */
void server_err(int connfd){
    char *msg = "HTTP/1.1 500 Internal Server Error\r\n";
    write(connfd, msg, sizeof(msg));
}

/*
 Get request
 */
void    get(char *sendbuf, int connfd, char *uri, int version, bool keepalive, bool headersonly){
    //printf("GET\n");

    ssize_t bytesread;
    ssize_t byteswritten;
    ssize_t filelen;
    ssize_t headerlen;
    ssize_t pos;
    FILE *fp = NULL;

    char *filetype;

    // See if file exists
    fp = locate_file(uri, "rb");
    if (!fp){
        printf("Sending 404\n");
        headerlen = insert_header(sendbuf, version, 404, "", 0, keepalive, connfd);
        write(connfd, sendbuf, headerlen);
        return;
    }

    // Get file metadata
    filetype = type_from_filename(uri);
    filelen = len_from_fp(fp);

    // Insert the header, return if this is HEAD request
    headerlen = insert_header(sendbuf, version, 200, filetype, filelen, keepalive, connfd);
    if (headersonly){
        byteswritten = write(connfd, &sendbuf[0], headerlen);
        return;
    }

    // Insert the file contents
    bytesread = read(fileno(fp), &sendbuf[headerlen], MAXBUF-headerlen);
    byteswritten = write(connfd, &sendbuf[0], bytesread+headerlen);
    if(byteswritten < bytesread+headerlen)
        perror("Error on first write");
    // Repeat if multiple packets needed
    pos = bytesread;
    while(pos < filelen){
        bzero(&sendbuf[0], MAXBUF);
        bytesread = fread(&sendbuf[0], 1, MAXBUF, fp);
        pos += bytesread;
        byteswritten = write(connfd, &sendbuf[0], bytesread);
        if(byteswritten < bytesread)
            perror("Error on subsequent write");
    }
    fclose(fp);
}

/*
 Post request
 */
void    post(char *sendbuf, int connfd, char *uri, char *postdata, int version, bool keepalive){
    char *filetype;
    char *htmlheader;
    FILE *fp;

    // Only support HTML post requests
    filetype = type_from_filename(uri);
    if (strncmp(filetype, "text/html", 9) != 0){
        printf("Unsupported POST request\n");
        server_err(connfd);
        return;
    }

    // Open the file in append mode
    fp = locate_file(uri, "a");
    if (!fp)
        server_err(connfd);
    
    // Append to file
    fprintf(fp, "<h1>POST DATA</h1>\r\n<pre>%s</pre>", postdata);
    fclose(fp); // Will be reopened in read-binary mode

    // Forward to GET request
    get(sendbuf, connfd, uri, version, keepalive, false);
}

/*
 Given a filename, infer the filetype
 */
char    *type_from_filename(char *uri){
    //printf("FILETYPE\n");
    char *ext;

    ext = strrchr(uri, '.');

    if (strncmp(ext, ".html", 3) == 0){
        return "text/html";
    } else if (strncmp(ext, ".txt", 3) == 0){
        return "text/plain";
    } else if (strncmp(ext, ".png", 3) == 0){
        return "image/png";
    } else if (strncmp(ext, ".gif", 3) == 0){
        return "image/gif";
    } else if (strncmp(ext, ".jpg", 3) == 0){
        return "image/jpg";
    } else if (strncmp(ext, ".css", 3) == 0){
        return "text/css";
    } else if (strncmp(ext, ".js", 2) == 0){
        return "application/javascript";
    } else {
        return ext;
    }
}

/* 
 Given a file pointer, return the size
 */
int     len_from_fp(FILE *fp){
    //printf("FILELEN\n");
    int len;

    fseek(fp, 0L, SEEK_END);
    len = ftell(fp);
    rewind(fp);
    return len;
}

/*
 Given a filename as a string, find it in the directory and return a file pointer
 Return null if no file
 */
FILE    *locate_file(char *filename, char *mode){
    //printf("LOCATE\n");
    char buf[1024];
    char *cwd;
    FILE *fp = NULL;

    cwd = getcwd(&buf[0], 1024);
    //printf("Current directory is %s\n", cwd);

    fp = fopen(filename, mode);
    if (!fp)
        perror("Error opening file");

    return fp;
}

/* 
 * open_listenfd - open and return a listening socket on port
 * Returns -1 in case of failure 
 */
int open_listenfd(int port) 
{
    int listenfd, optval=1;
    struct sockaddr_in serveraddr;
  
    /* Create a socket descriptor */
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;

    /* Eliminates "Address already in use" error from bind. */
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, 
                   (const void *)&optval , sizeof(int)) < 0)
        return -1;

    /* listenfd will be an endpoint for all requests to port
       on any IP address for this host */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET; 
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY); 
    serveraddr.sin_port = htons((unsigned short)port); 
    if (bind(listenfd, (struct sockaddr*)&serveraddr, sizeof(serveraddr)) < 0)
        return -1;

    /* Make it a listening socket ready to accept connection requests */
    if (listen(listenfd, LISTENQ) < 0)
        return -1;
    return listenfd;
}
