/*
 * proxy.c - ICS Web proxy
 *
 *
 */

#include "csapp.h"
#include <stdarg.h>
#include <unistd.h>
// Longer than cookies length limit of general browsers.
#define HTTP_MAXLINE 5120
#define BLOCK_MAXLINE 10240
#define URI_MAXLINE 1024*1024

typedef struct header {
    char *name;
    char *value;
    char *raw_header;
    size_t raw_length;
    struct header *next;
} HttpHeader;

typedef enum {
    GET,
    POST
} HttpMethod;

typedef struct {
    HttpMethod method;
    char *uri;
    char *hostname;
    char *port;
    char *path;
    size_t start_line_length;
    size_t content_length;
} HttpMeta;

typedef struct {
    char *start_line;
    HttpHeader *headers;
    char *body;
    HttpMeta meta;
} HttpMessage;

typedef enum {
    REQUEST,
    RESPONSE
} HttpMessageType;

/* $begin sbuft */
typedef struct {
    int *buf;          /* Buffer array */
    int n;             /* Maximum number of slots */
    int front;         /* buf[(front+1)%n] is first item */
    int rear;          /* buf[rear%n] is last item */
    sem_t mutex;       /* Protects accesses to buf */
    sem_t slots;       /* Counts available slots */
    sem_t items;       /* Counts available items */
} sbuf_t;

/* $end sbuft */

void sbuf_init(sbuf_t *sp, int n);

void sbuf_deinit(sbuf_t *sp);

void sbuf_insert(sbuf_t *sp, int item);

int sbuf_remove(sbuf_t *sp);

sbuf_t proxy_tasks;

/*
 * Function prototypes
 */
int parse_uri(char *uri, char *target_addr, char *path, char *port);

void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, char *uri, size_t size);

size_t get_cpu_cores();

size_t divide_by_delim(char *source, char delim, char **lhs, char **rhs);

int parse_request_line(char *line, HttpMethod *method, char **uri);

HttpHeader *init_headers();

HttpHeader *parse_header(char *raw_header);

void parse_host(char *host, char **hostname, char **port);

size_t parse_content_length(char *content_length);

void free_headers(HttpHeader *headers);

int write_headers(int fd, HttpHeader *headers);

HttpMessage *build_message_from_rio(rio_t *istream, HttpMessageType type, size_t *total_bytes);

int send_http_message(int clientfd, HttpMessage *message);

void free_http_message(HttpMessage *message);

_Noreturn void *proxy_worker(void *vargs);

/*
 * Rio wrappers
 */
ssize_t Rio_readlineb_w(rio_t *rp, void *usrbuf, size_t maxlen);

int Rio_writen_w(int fd, void *usrbuf, size_t n);

ssize_t Rio_readn_w(rio_t *rp, void *usrbuf, size_t n);

ssize_t rio_read(rio_t *rp, char *usrbuf, size_t n);

/*
 * main - Main routine for the proxy program
 */
int main(int argc, char **argv) {
    /* Check arguments */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port number>\n", argv[0]);
        exit(0);
    }
    Signal(SIGPIPE, SIG_IGN);
    int listenfd, connfd;
    pthread_t tid;
    listenfd = Open_listenfd(argv[1]);
    size_t threads = get_cpu_cores();
    sbuf_init(&proxy_tasks, threads * 1000);
    for (int i = 0; i < threads; i++) {
        Pthread_create(&tid, NULL, proxy_worker, NULL);
    }
    while (1) {
        connfd = Accept(listenfd, NULL, NULL);
        sbuf_insert(&proxy_tasks, connfd);
    }
    exit(0);
}

/* Create an empty, bounded, shared FIFO buffer with n slots */
/* $begin sbuf_init */
void sbuf_init(sbuf_t *sp, int n) {
    sp->buf = Calloc(n, sizeof(int));
    sp->n = n;                       /* Buffer holds max of n items */
    sp->front = sp->rear = 0;        /* Empty buffer iff front == rear */
    Sem_init(&sp->mutex, 0, 1);      /* Binary semaphore for locking */
    Sem_init(&sp->slots, 0, n);      /* Initially, buf has n empty slots */
    Sem_init(&sp->items, 0, 0);      /* Initially, buf has zero data items */
}
/* $end sbuf_init */

/* Clean up buffer sp */
/* $begin sbuf_deinit */
void sbuf_deinit(sbuf_t *sp) {
    Free(sp->buf);
}
/* $end sbuf_deinit */

/* Insert item onto the rear of shared buffer sp */
/* $begin sbuf_insert */
void sbuf_insert(sbuf_t *sp, int item) {
    P(&sp->slots);                          /* Wait for available slot */
    P(&sp->mutex);                          /* Lock the buffer */
    sp->buf[(++sp->rear) % (sp->n)] = item;   /* Insert the item */
    V(&sp->mutex);                          /* Unlock the buffer */
    V(&sp->items);                          /* Announce available item */
}
/* $end sbuf_insert */

/* Remove and return the first item from buffer sp */
/* $begin sbuf_remove */
int sbuf_remove(sbuf_t *sp) {
    int item;
    P(&sp->items);                          /* Wait for available item */
    P(&sp->mutex);                          /* Lock the buffer */
    item = sp->buf[(++sp->front) % (sp->n)];  /* Remove the item */
    V(&sp->mutex);                          /* Unlock the buffer */
    V(&sp->slots);                          /* Announce available slot */
    return item;
}
/* $end sbuf_remove */
/* $end sbufc */

/*
 * parse_uri - URI parser
 *
 * Given a URI from an HTTP proxy GET request (i.e., a URL), extract
 * the host name, path name, and port.  The memory for hostname and
 * pathname must already be allocated and should be at least MAXLINE
 * bytes. Return -1 if there are any problems.
 */
int parse_uri(char *uri, char *hostname, char *pathname, char *port) {
    char *hostbegin;
    char *hostend;
    char *pathbegin;
    int len;

    if (strncasecmp(uri, "http://", 7) != 0) {
        hostname[0] = '\0';
        return -1;
    }

    /* Extract the host name */
    hostbegin = uri + 7;
    hostend = strpbrk(hostbegin, " :/\r\n\0");
    if (hostend == NULL)
        return -1;
    len = hostend - hostbegin;
    strncpy(hostname, hostbegin, len);
    hostname[len] = '\0';

    /* Extract the port number */
    if (*hostend == ':') {
        char *p = hostend + 1;
        while (isdigit(*p))
            *port++ = *p++;
        *port = '\0';
    } else {
        strcpy(port, "80");
    }

    /* Extract the path */
    pathbegin = strchr(hostbegin, '/');
    if (pathbegin == NULL) {
        pathname[0] = '\0';
    } else {
        pathbegin++;
        strcpy(pathname, pathbegin);
    }

    return 0;
}

/*
 * format_log_entry - Create a formatted log entry in logstring.
 *
 * The inputs are the socket address of the requesting client
 * (sockaddr), the URI from the request (uri), the number of bytes
 * from the server (size).
 */
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr,
                      char *uri, size_t size) {
    time_t now;
    char time_str[MAXLINE];
    char host[INET_ADDRSTRLEN];

    /* Get a formatted time string */
    now = time(NULL);
    strftime(time_str, MAXLINE, "%a %d %b %Y %H:%M:%S %Z", localtime(&now));

    if (inet_ntop(AF_INET, &sockaddr->sin_addr, host, sizeof(host)) == NULL)
        unix_error("Convert sockaddr_in to string representation failed\n");

    /* Return the formatted log entry string */
    sprintf(logstring, "%s: %s %s %zu", time_str, host, uri, size);
}

ssize_t Rio_readlineb_w(rio_t *rp, void *usrbuf, size_t maxlen) {
    ssize_t rc;

    if ((rc = rio_readlineb(rp, usrbuf, maxlen)) < 0)
        return 0;
    return rc;
}

int Rio_writen_w(int fd, void *usrbuf, size_t n) {
    if (rio_writen(fd, usrbuf, n) != n) {
        return -1;
    }
    return n;
}

ssize_t Rio_readn_w(rio_t *rp, void *usrbuf, size_t n) {
    ssize_t rc;

    if ((rc = rio_readnb(rp, usrbuf, n)) < 0)
        return 0;
    return rc;
}

ssize_t rio_read(rio_t *rp, char *usrbuf, size_t n) {
    int cnt;

    while (rp->rio_cnt <= 0) {  /* Refill if buf is empty */
        rp->rio_cnt = read(rp->rio_fd, rp->rio_buf,
                           sizeof(rp->rio_buf));
        if (rp->rio_cnt < 0) {
            if (errno != EINTR) /* Interrupted by sig handler return */
                return -1;
        } else if (rp->rio_cnt == 0)  /* EOF */
            return 0;
        else
            rp->rio_bufptr = rp->rio_buf; /* Reset buffer ptr */
    }

    /* Copy min(n, rp->rio_cnt) bytes from internal buf to user buf */
    cnt = n;
    if (rp->rio_cnt < n)
        cnt = rp->rio_cnt;
    memcpy(usrbuf, rp->rio_bufptr, cnt);
    rp->rio_bufptr += cnt;
    rp->rio_cnt -= cnt;
    return cnt;
}

size_t get_cpu_cores() {
    return sysconf(_SC_NPROCESSORS_ONLN);
}

HttpHeader *init_headers() {
    HttpHeader *head = (HttpHeader *) calloc(1, sizeof(HttpHeader));
    head->raw_header = NULL;
    head->next = NULL;
    return head;
}

size_t divide_by_delim(char *source, char delim, char **lhs, char **rhs) {
    size_t source_len = strlen(source);
    size_t delim_index = 0;
    for (size_t i = 0; i < source_len; i++) {
        if (source[i] == delim) {
            delim_index = i;
            break;
        }
    }
    size_t lhs_len = delim_index;
    *lhs = (char *) malloc(lhs_len + sizeof(char));
    strncpy(*lhs, source, lhs_len);
    (*lhs)[lhs_len] = '\0';
    size_t rhs_index = delim_index + 1;
    while (source[rhs_index] == ' ') {
        rhs_index++;
    }
    size_t rhs_len = source_len - rhs_index;
    *rhs = (char *) malloc(rhs_len + sizeof(char));
    strncpy(*rhs, &source[rhs_index], rhs_len);
    (*rhs)[rhs_len] = '\0';
    return source_len;
}

int parse_request_line(char *line, HttpMethod *method, char **uri) {
    size_t line_length = strlen(line);
    size_t path_index = 0, path_length = 0;
    if (strncmp(line, "GET", 3) == 0) {
        *method = GET;
        path_index = 4;
    } else if (strncmp(line, "POST", 4) == 0) {
        *method = POST;
        path_index = 5;
    } else {
        return 0;
    }
    for (int i = path_index; i < line_length; ++i) {
        if (line[i] == ' ') {
            break;
        }
        path_length++;
    }
    *uri = (char *) malloc(path_length + sizeof(char));
    strncpy(*uri, &line[path_index], path_length);
    (*uri)[path_length] = '\0';
    return line_length;
}

HttpHeader *parse_header(char *raw_header) {
    HttpHeader *header = calloc(1, sizeof(HttpHeader));
    header->next = NULL;
    header->raw_length = divide_by_delim(raw_header, ':', &header->name, &header->value);
    header->raw_header = (char *) malloc(header->raw_length + sizeof(char));
    strcpy(header->raw_header, raw_header);
    return header;
}

void parse_host(char *host, char **hostname, char **port) {
    divide_by_delim(host, ':', hostname, port);
}

size_t parse_content_length(char *content_length) {
    return strtoull(content_length, NULL, 10);
}

void free_headers(HttpHeader *headers) {
    HttpHeader *cur = headers;
    HttpHeader *next = headers;
    while (next != NULL) {
        next = cur->next;
        free(cur->name);
        free(cur->value);
        free(cur->raw_header);
        free(cur);
        cur = next;
    }
}

int write_headers(int fd, HttpHeader *headers) {
    HttpHeader *cur = headers->next;
    while (cur != NULL && cur->raw_header != NULL) {
        if (Rio_writen_w(fd, cur->raw_header, cur->raw_length) != cur->raw_length) {
            return -1;
        }
        cur = cur->next;
    }
    return 0;
}

HttpMessage *build_message_from_rio(rio_t *istream, HttpMessageType type, size_t *total_bytes) {
    HttpMessage *message = (HttpMessage *) calloc(1, sizeof(HttpMessage));
    HttpMeta *meta = &message->meta;
    size_t recv_bytes = 0;
    /*
     * Parse start line.
     */
    char *buf = (char *) malloc(URI_MAXLINE);
    if (Rio_readlineb_w(istream, buf, URI_MAXLINE) <= 0) {
        goto error;
    }
    size_t start_line_length = 0;
    if (type == REQUEST) {
        start_line_length = parse_request_line(buf, &meta->method, &meta->uri);
        meta->hostname = (char *) malloc(URI_MAXLINE);
        meta->path = (char *) malloc(URI_MAXLINE);
        meta->port = (char *) malloc(URI_MAXLINE);
        parse_uri(meta->uri, meta->hostname, meta->path, meta->port);
    } else {
        start_line_length = strlen(buf);
    }
    recv_bytes += start_line_length;
    message->start_line = (char *) malloc(start_line_length + sizeof(char));
    if (type == REQUEST) {
        sprintf(message->start_line, "%s /%s HTTP/1.1\r\n", meta->method == GET ? "GET" : "POST", meta->path);
        meta->start_line_length = strlen(message->start_line);
    } else {
        strcpy(message->start_line, buf);
        meta->start_line_length = start_line_length;
    }

    /*
     * load headers.
     */
    message->headers = (HttpHeader *) calloc(1, sizeof(HttpHeader));
    size_t rc;
    HttpHeader *tail = message->headers;
    while (1) {
        if ((rc = Rio_readlineb_w(istream, buf, URI_MAXLINE)) <= 0) {
            goto error;
        }
        recv_bytes += rc;
        if (strcmp(buf, "\r\n") == 0) {
            break;
        }
        HttpHeader *new_header = parse_header(buf);
        tail->next = new_header;
        tail = new_header;

        if (strcmp(new_header->name, "Content-Length") == 0) {
            meta->content_length = parse_content_length(new_header->value);
        }
    }

    /*
     * For requests, load all body
     * For response, body should be read chunkedly, do not load all here.
     */
    if (type == REQUEST && meta->method == POST) {
        size_t content_length = meta->content_length;
        char *body_buf = (char *) malloc(content_length);
        if (Rio_readn_w(istream, body_buf, content_length) != content_length) {
            goto error;
        }
        message->body = body_buf;
        recv_bytes += content_length;
    }

    if (total_bytes != NULL) {
        *total_bytes = recv_bytes;
    }
    return message;

    error:

    free_http_message(message);
    return NULL;
}

int send_http_message(int clientfd, HttpMessage *message) {
    HttpMeta *meta = &message->meta;
    if (Rio_writen_w(clientfd, message->start_line, meta->start_line_length) < 0) {
        return -1;
    }

    if (write_headers(clientfd, message->headers) < 0) {
        return -1;
    }

    if (Rio_writen_w(clientfd, "\r\n", 2) < 0) {
        return -1;
    }

    if (meta->method == POST && message->body != NULL) {
        if (Rio_writen_w(clientfd, message->body, meta->content_length) < 0) {
            return -1;
        }
    }
    return 0;
}

void free_http_message(HttpMessage *message) {
    if (message == NULL) {
        return;
    }
    HttpMeta *meta = &message->meta;
    free(meta->uri);
    free(meta->hostname);
    free(meta->port);
    free(meta->path);
    free(message->start_line);
    free(message->body);
    free_headers(message->headers);
    free(message);
}

_Noreturn void *proxy_worker(void *vargs) {
    Signal(SIGPIPE, SIG_IGN);
    Pthread_detach(Pthread_self());

    char buf[BLOCK_MAXLINE + 10];
    while (1) {
        size_t relay_bytes = 0;
        int connfd = sbuf_remove(&proxy_tasks);

        rio_t istream;
        Rio_readinitb(&istream, connfd);
        HttpMessage *request = build_message_from_rio(&istream, REQUEST, NULL);
        HttpMessage *response = NULL;
        if (request == NULL) {
            goto loop_clean;
        }
        HttpMeta *request_meta = &request->meta;
        int clientfd = open_clientfd(request_meta->hostname, request_meta->port);
        if (clientfd < 0) {
            goto loop_clean;
        }

        if (send_http_message(clientfd, request) < 0) {
            goto loop_clean;
        }

        rio_t response_stream;
        Rio_readinitb(&response_stream, clientfd);
        response = build_message_from_rio(&response_stream, RESPONSE, &relay_bytes);
        if (response == NULL) {
            goto loop_clean;
        }

        if (send_http_message(connfd, response) < 0) {
            goto loop_clean;
        }

        /*
         * Run out of buffer.
         */
        size_t buffer_remain = rio_read(&response_stream, buf, RIO_BUFSIZE);
        if (Rio_writen_w(connfd, buf, buffer_remain) < 0) {
            goto loop_clean;
        }
        relay_bytes += buffer_remain;

        size_t recv_bytes = 0, left_bytes = response->meta.content_length - buffer_remain;

        while (left_bytes != 0) {
            if (left_bytes < BLOCK_MAXLINE) {
                if ((recv_bytes = recv(clientfd, buf, left_bytes, 0)) <= 0) {
                    goto loop_clean;
                }
            } else {
                if ((recv_bytes = recv(clientfd, buf, BLOCK_MAXLINE, 0)) <= 0) {
                    goto loop_clean;
                }
            }
            /*
            * Remember: recv usually can't read as much as BLOCK_MAXLINE.
            */
            left_bytes -= recv_bytes;
            relay_bytes += recv_bytes;
            if (Rio_writen_w(connfd, buf, recv_bytes) < 0) {
                goto loop_clean;
            }
        }

        struct sockaddr_in clientaddr;
        socklen_t clientlen = sizeof(struct sockaddr_storage);
        getpeername(connfd, (SA *) &clientaddr, &clientlen);
        char *log = (char *) malloc(URI_MAXLINE);
        format_log_entry(log, &clientaddr, request_meta->uri, relay_bytes);
        printf("%s\n", log);

        loop_clean:
        free_http_message(request);
        free_http_message(response);
    }
    return NULL;
}