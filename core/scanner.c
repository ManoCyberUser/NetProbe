#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/select.h>

#define MAX_THREADS 50
#define TIMEOUT_SEC 1

typedef struct{
        char target[64];
        int *ports;
        int total_ports;
        int index;
        pthread_mutex_t lock;
}scan_data;

int check_port(const char *target, int port, char *banner){
        int sock;
        struct sockaddr_in server;
        fd_set fdset;
        struct timeval tv;

        sock = socket(AF_INET, SOCK_STREAM, 0);

        if (sock < 0) return 0;

        fcntl(sock, F_SETFL, O_NONBLOCK);

        server.sin_family = AF_INET;
        server.sin_port = htons(port);
        inet_pton(AF_INET, target, &server.sin_addr);

        connect(sock, (struct sockaddr *)&server, sizeof(server));

        FD_ZERO(&fdset);
        FD_SET(sock, &fdset);
        tv.tv_sec = TIMEOUT_SEC;
        tv.tv_usec = 0;

        if(select(sock + 1, NULL, &fdset, NULL, &tv) == 1){
                socklen_t len = sizeof(int);
                int so_error;
                getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);

                if(so_error == 0){
                        recv(sock, banner, 1023, 0);
                        close(sock);
                        return 1;
                }
        }
        close(sock);
        return 0;
}
void *worker(void *arg){
        scan_data *data = (scan_data *)arg;

        while (1){
                pthread_mutex_lock(&data->lock);
                if(data->index >= data->total_ports){
                        pthread_mutex_unlock(&data->lock);
                        break;
                }
                int port = data->ports[data->index++];
                pthread_mutex_unlock(&data->lock);

                char banner[1024] = {0};

                if(check_port(data->target, port, banner)){
                        printf("{\"port\": %d, \"status\": \"open\", \"banner\": \"%s\"} \n",port,banner);
                }
        }
        return NULL;
}

int main(int argc, char *argv[]){
        if(argc != 4) {
                printf("Usage: %s <IP> <start_port> <end_port> \n",argv[0]);
                return 1;
        }
        char *target = argv[1];
        int start = atoi(argv[2]);
        int end = atoi(argv[3]);

        int total = end - start + 1;
        int *ports = malloc(total * sizeof(int));

        for (int i = 0; i < total; i++)
                ports[i] = start + i;

        scan_data data;
        strcpy(data.target, target);
        data.ports = ports;
        data.total_ports = total;
        data.index = 0;
        pthread_mutex_init(&data.lock, NULL);

        pthread_t threads[MAX_THREADS];

        for(int i = 0; i < MAX_THREADS; i++)
                pthread_create(&threads[i], NULL, worker, &data);

        for(int i = 0; i < MAX_THREADS; i++)
                pthread_join(threads[i], NULL);

        pthread_mutex_destroy(&data.lock);
        free(ports);

        return 0;
}
