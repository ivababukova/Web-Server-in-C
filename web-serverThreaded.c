#define _GNU_SOURCE // for strcasestr function

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <netinet/in.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>

#define MY_PORT 5000 // the port I am using

#define SMALL 1000
#define BIG 100000
#define HUGE 1000000

typedef struct queue Queue;
typedef struct node Node;

typedef struct queue {
	pthread_mutex_t q_mutex;
	pthread_cond_t nonempty_q;
    Node *front;
    Node *rear;
    long size;
} Queue;

typedef struct node {
    Node *next;
    int connfd;
} Node;

int isEmpty(Queue *q) {
    return q->size == 0 ? 1 : 0;
}

Queue *queue_create() {
	Queue *q;
    q = (Queue *)malloc(sizeof(Queue));
    if (q == NULL) {
        printf ("no memory!.\n");
        exit (1);
    }
    q->size = 0;
    q->front = NULL;
    q->rear = NULL;
    pthread_mutex_t qm = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP; 
    pthread_cond_t nonempty_signal = PTHREAD_COND_INITIALIZER;
    q->q_mutex = qm;
    q->nonempty_q = nonempty_signal;
    return q;
}

Node *node_create (int connfd) {
	Node *n;
    n = (Node *)malloc(sizeof(Node));
    if (n == NULL) {
        printf ("no memory!.\n");
        exit (1);
    }
    n->next = NULL;
    n->connfd = connfd;
    return n;
}

void enqueue (Queue *q, int connfd) {

    pthread_mutex_lock(&q->q_mutex);

	Node *n = node_create(connfd);
    if (q->size == 0){
        q->front = n;
        q->rear = n;
    }
    else {
        q->rear->next = n;
    }	
	q->size++;

    pthread_cond_signal (&q->nonempty_q);
    pthread_mutex_unlock (&q->q_mutex);
}

int dequeue (Queue *q) {

    int connfd;
    pthread_mutex_lock(&q->q_mutex);		
    
    while (isEmpty(q)){
        pthread_cond_wait(&q->nonempty_q, &q->q_mutex);
    }

	Node *front = q->front;

    q->front = front->next;

    connfd = front->connfd;
   
	q->size--;
    
    free(front->next);
    free(front);
    pthread_mutex_unlock(&q->q_mutex);   
	return connfd;
}

static void iterate_nodes_destroy(Node *n)
{
    if (n == NULL)
        return;
    iterate_nodes_destroy(n->next);
    free(n);
}

void queue_destroy(Queue *q)
{
    iterate_nodes_destroy(q->front);
    free(q);
}

// allocate @length memory
char* newstr(int length){
   char* new_memory = (char*) calloc(length, sizeof(char));
   if (new_memory == NULL){
     perror("Error in calloc.");
     exit(1);
   }
   return new_memory;
}


void respond_400 (int conn_fd) {
    char buff [300];
    strcpy (buff, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/html\r\nConnection: keep-alive\r\n\r\n") ;
    strcat (buff, "<!DOCTYPE html><html><head><title>400 Bad Request </title></head>");
    strcat (buff, "<body> <h1>400 Bad Request</h1><p>Something wrong with your request</p></body></html>");
    write (conn_fd, buff, strlen(buff));
    close (conn_fd);
}

void respond_404 (int conn_fd) {
    char buff [300];
    strcpy (buff, "HTTP/1.1 404 File Not Found\r\nContent-Type: text/html\r\nConnection: keep-alive\r\n\r\n") ;
    strcat (buff, "<!DOCTYPE html><html><head><title>404 File Not Found</title></head>");
    strcat (buff, "<body> <h1>404 File Not Found</h1><p>The file requested doesn't exist.</p></body></html>");
    write (conn_fd, buff, strlen(buff));
    close (conn_fd);
}

void respond_500 (int conn_fd) {
    char buff [300];
    strcpy (buff, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/html\r\nConnection: keep-alive\r\n\r\n") ;
    strcat (buff, "<!DOCTYPE html><html><head><title>500 Internal Server Error</title></head>");
    strcat (buff, "<body> <h1>500 Internal Server Error</h1></body></html>");
    write (conn_fd, buff, strlen(buff));
    close (conn_fd);
}

bool write_message (const char *message, int message_length, int file_descriptor, int repeat_if_error) {
    if(repeat_if_error <= 0){
        return false;
    }
    unsigned int total_written = 0;
    while(total_written < message_length) {
        usleep(1000);
        ssize_t written_this_round = write (file_descriptor,
                                            (const char *) &message[total_written],
                                            sizeof(char) * (message_length - total_written)
                                            //sizeof(char) * (1)
                                            );
        if (written_this_round != -1) {
            total_written += written_this_round;        
        }
        if (written_this_round == -1) {
            perror ("write error, tusia");
            return write_message (message + total_written,
                                  message_length - total_written,
                                  file_descriptor,
                                  repeat_if_error - 1);
        }
    }
    return true;
}

int response_generator (int conn_fd, char *filename) {

    /* vars needed for finding the length of the file */
    struct stat filestat;
    FILE *fp;
    int fd;
    char* header_buff = newstr(BIG);
    char* file_buff = newstr(HUGE);
    char filesize[7];//, name[30]; 

    if ( ( (fd = open (filename, O_RDONLY)) < -1) || (fstat(fd, &filestat) < 0) ) {
        printf ("Error in measuring the size of the file.\n");
        respond_404 (conn_fd);
    }

    if (filename == NULL || strcmp (filename, "400") == 0 ) {
        respond_400 (conn_fd);
        return 1;
    }

    fp = fopen (filename, "r");
    if (fp == NULL) {
	    respond_404 (conn_fd);
        return 2;
    }

    else if (fp != NULL) {
        sprintf (filesize, "%zd", filestat.st_size); // put the file size to buffer
        strcpy (header_buff, "HTTP/1.1 200 OK\r\nContent-Length: ");

        /* write the content-length: */
        strcat (header_buff, filesize);
        strcat (header_buff, "\r\n");

        /* find the content-type: */
        if (strstr (filename, ".html") != NULL || strstr(filename, ".hml") != NULL ) {
            strcat (header_buff, "Content-Type: text/html \r\n");
        }
        else if (strstr (filename, ".txt") != NULL ) {
            strcat (header_buff, "Content-Type: text/plain \r\n");
        }
        else if (strstr (filename, ".jpg") != NULL  || strstr(filename, ".jpeg") != NULL ) {
            strcat (header_buff, "Content-Type: image/jpeg \r\n");
        }
        else if (strstr (filename, ".gif") != NULL ) {
            strcat (header_buff, "Content-Type: image/gif \r\n");
        } 
        else {
            strcat (header_buff, "Content-Type: application/octet-stream \r\n");
        }
        
        strcat (header_buff, "Connection: keep-alive\r\n\r\n");
        write_message (header_buff, strlen(header_buff), conn_fd, 5);

        fread (file_buff, sizeof(char), filestat.st_size, fp);
        write_message (file_buff, filestat.st_size, conn_fd, 10);
    }

    else {
        // I have measured the length of my 500.html file
        respond_500 (conn_fd);
        return -1;
    }        

    fclose (fp);
    close (conn_fd);
//    free (file_buff);
    free (header_buff);
    return 0;
}

int checkHostName (char *hostname, char *hostname_req, char *buff) {
    char *host = strcasestr (buff, "host: ");
    if (host == NULL) {
        return -1; // TODO check what error to return if there is missing host header
    }

    else {
        host += 6;
        char *start_host = host;
        char *stripped_host = strstr (start_host, "\r\n");
        memcpy(hostname_req, start_host, stripped_host - start_host);  
        if (strcmp (hostname_req, hostname) != 0) {
            strcat (hostname, ":5000"); // check whether this hostname is used instead
            if (strcmp (hostname_req, hostname) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

Queue *q;

/* returns the name of the file requested by the browser: */
void *request_parser () {

    printf ("hello!\n");

    char *filename = newstr (SMALL);
    char *token = NULL;
    char *get = newstr (SMALL);
    char *http = newstr (BIG);
    char *hostname = newstr (SMALL);
    char *hostname_req = newstr (SMALL);
    char *name = newstr (SMALL);
    char *buff = newstr (BIG);
    int r, clientsock;
    int isHostNameRight = 0;

    
    clientsock = dequeue (q);
    gethostname(hostname, 100);

    if ( (r = read(clientsock, buff, BIG) ) == -1) {
            perror ("Error on RECV\n");
    }
    if (r == 0) {
         printf ("stopped requesting\n");
         return;
    }
 
    if (checkHostName (hostname, hostname_req, buff) == -1) {
         isHostNameRight = -1;
         printf ("hostname is wrong, have to return 400");
    }

    sscanf (buff, "%s %s %s", get, filename, http);
    
    if ( strcmp (get, "GET") == 0 ) {
        strcpy (name, &filename[1]);        
        free(filename);
        filename = name;
        response_generator (clientsock, filename);
    }

    else if (isHostNameRight == -1 || strcmp (get, "GET") != 0){
        free(name);
        response_generator (clientsock, "400");
    }

    free (filename);
    free (get);
    free (http);
    free (hostname);
    free (hostname_req);
    free (buff);
    printf ("hello2!\n");
    return;

}

int main() {

    int connNumb = 20;
    q = queue_create ();
    int sock, clientsock, listens;
    struct sockaddr_in addr;
    struct sockaddr_in cliaddr;
    socklen_t cliaddrlen;
    int connfd_int = -3; // if there is no request, this is -3, if there is a request, it is the conn_fd

    // create a TCP socket
    sock = socket(AF_INET, SOCK_STREAM, 0); 
    if (sock == -1) {
        return -1; 
    }

    /* inititalize address, port structure */
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MY_PORT);
    addr.sin_addr.s_addr = htonl (INADDR_ANY);
    
    int optval = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
    
    // bind the socket to port 8080
    if (bind (sock, (struct sockaddr *) &addr, sizeof(addr) ) == -1 ) {
        return 7;
    }
    // listen for and accept connections from browsers, max 20 connections can wait
    if ( (listens = listen (sock, connNumb)) == -1) {
        return -1;
    }
 
    cliaddrlen = sizeof(cliaddr);

    pthread_t *thread = malloc (sizeof (pthread_t)*connNumb); // pointer to the thread pool

    // create threads in the thread pool:
    int i;

    while (1) {
        // accepts the connection, returns new file descriptor for the connection and cliaddr
        if ( (clientsock = accept(sock, (struct sockaddr*)&cliaddr, &cliaddrlen)) == -1) {
            perror ("Error on ACCEPT\n");
        }   

        for (i = 0; i < connNumb; i++) {
            int ret = -1;
            ret = pthread_create (&thread[i], NULL, request_parser, NULL);
            // pthread_join (thread [i], NULL);
            if (ret != 0) {
                perror("error occured while creating thread:");
                exit (1);
            }
        }

        // start a new thread to process the network connection accepted:
        enqueue (q, clientsock);
        printf ("Sending ...\n");
/*
        for (i = 0; i < connNumb; i++) {
            pthread_exit (thread[i]);
            printf ("the thread exited.\n");
        }
*/
    }
    close (sock);
    free (thread);
    queue_destroy (q);
    return 0;
}



