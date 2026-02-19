#include <Python.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/select.h>
#include <errno.h>

#define MAX_THREADS 50
#define TIMEOUT_SEC 1
#define BANNER_SIZE 1024

typedef struct{
        char target[64];
        int *ports;
        int total_ports;
        int index;
        pthread_mutex_t lock;
}scan_data;

static PyThreadState *mainThreadState = NULL;

const char* get_service_from_python(int port, const char* banner){
        static char result[256];
        PyObject *pName = NULL, *pModule = NULL, *pFunc = NULL;
        PyObject *pArgs = NULL, *pValue = NULL;
        PyObject *pPort = PyLong_FromLong(port);
        PyGILState_STATE gstate;

        strcpy(result, "UNKNOWN");

        const char* safe_banner = (banner != NULL) ? banner : "";

        gstate = PyGILState_Ensure();

        pName = PyUnicode_FromString("service_detector");
        if(!pName){
                PyErr_Print();
                goto cleanup;
        }

        pModule = PyImport_Import(pName);
        Py_DECREF(pName);

        if(!pModule){
                PyErr_Print();
                strcpy(result, "IMPORT ERROR");
                goto cleanup;
        }

        pFunc = PyObject_GetAttrString(pModule, "detect_service");
        if(!pFunc || !PyCallable_Check(pFunc)){
                if(PyErr_Occurred()) PyErr_Print();
                strcpy(result, "FUNCTION ERROR");
                goto cleanup;
        }

        pArgs = PyTuple_New(2);
        if(!pArgs){
                PyErr_Print();
                goto cleanup;
        }

        PyTuple_SetItem(pArgs, 0, pPort);

        PyObject *pBanner = PyUnicode_FromString(safe_banner);
        if(!pBanner){
                PyErr_Print();
                goto cleanup;
        }

        PyTuple_SetItem(pArgs, 1, pBanner);

        pValue = PyObject_CallObject(pFunc, pArgs);
        if(!pValue){
                if(PyErr_Occurred()) PyErr_Print();
                strcpy(result, "CALL ERROR");
                goto cleanup;
        }

        PyObject *pStr = PyObject_Str(pValue);
        if(pStr){
                const char *tmp = PyUnicode_AsUTF8(pStr);
                if(tmp){
                        strncpy(result, tmp, sizeof(result) - 1);
                        result[sizeof(result) -1] = '\0';
                }
        Py_DECREF(pStr);
        }
        cleanup:
        Py_XDECREF(pFunc);
        Py_XDECREF(pModule);
        Py_XDECREF(pArgs);
        Py_XDECREF(pValue);

        PyGILState_Release(gstate);

        return result;
}

int check_port(const char *target, int port, char *banner){
        int sock;
        struct sockaddr_in server;
        fd_set fdset;
        struct timeval tv;
        int flags;

        sock = socket(AF_INET, SOCK_STREAM, 0);
        if(sock < 0) return 0;

        flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        server.sin_family = AF_INET;
        server.sin_port = htons(port);
        if(inet_pton(AF_INET, target, &server.sin_addr) <= 0){
                close(sock);
                return 0;
        }

        connect(sock, (struct sockaddr *)&server, sizeof(server));

        FD_ZERO(&fdset);
        FD_SET(sock, &fdset);
        tv.tv_sec = TIMEOUT_SEC;
        tv.tv_usec = 0;

        if(select(sock + 1, NULL, &fdset, NULL, &tv) == 1){
                int so_error;
                socklen_t len = sizeof(so_error);

                getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);

                if(so_error == 0){
                        ssize_t bytes = recv(sock, banner, BANNER_SIZE -1, 0);
                        if(bytes > 0){
                                banner[bytes] = '\0';
                        }else{
                                banner[0] = '\0';
                        }
                        close(sock);
                        return 1;
                }
        }

        close(sock);
        return 0;
}
void *worker(void *arg){
        scan_data *data = (scan_data *)arg;

        while(1){
                pthread_mutex_lock(&data->lock);
                if(data->index >= data->total_ports){
                        pthread_mutex_unlock(&data->lock);
                        break;
                }
                int port = data->ports[data->index++];
                pthread_mutex_unlock(&data->lock);

                char banner[BANNER_SIZE] = {0};

                if(check_port(data->target, port, banner)){
                        const char* service = get_service_from_python(port, banner);

                        pthread_mutex_lock(&data->lock);
                        printf("PORT: %d OPEN\n", port);
                        printf("SERVICE: %s\n", service);
                        if(strlen(banner) > 0){
                                printf("BANNER: %s\n", banner);
                        }else{
                                printf("BANNER: No Banner Recieved\n");
                        }
                        printf("------------------------------\n");
                        fflush(stdout);
                        pthread_mutex_unlock(&data->lock);
                }
        }
        return NULL;
}

int main(int argc, char *argv[]){
        if (argc != 4){
                printf("USAGE: %s <target_IP> <start_port> <end_port>\n",argv[0]);
                return 1;
        }

        char *target = argv[1];
        int start = atoi(argv[2]);
        int end = atoi(argv[3]);

        if(start > end || start < 1 || end > 65535){
                printf("Invalid Port Range\n");
                return 1;
        }

        Py_Initialize();

        PyRun_SimpleString("import sys");
        PyRun_SimpleString("sys.path.append('.')");

        mainThreadState = PyEval_SaveThread();

        int total = end - start + 1;
        int *ports = malloc(total * sizeof(int));
        if(!ports){
                printf("Memory allocation Failed\n");
                PyEval_RestoreThread(mainThreadState);
                Py_Finalize();
                return 1;
        }

        for(int i = 0; i < total; i++){
                ports[i] = start + i;
        }

        scan_data data;
        strncpy(data.target, target, sizeof(data.target) - 1);
        data.target[sizeof(data.target) - 1] = '\0';
        data.ports = ports;
        data.total_ports = total;
        data.index = 0;

        if (pthread_mutex_init(&data.lock, NULL) != 0) {
                 printf("Mutex initialization failed\n");
                 free(ports);
                 PyEval_RestoreThread(mainThreadState);
                 Py_Finalize();
                 return 1;
         }


        pthread_t threads[MAX_THREADS];
        int active_threads = 0;

        for(int i = 0; i < MAX_THREADS; i++){
                if(pthread_create(&threads[i], NULL, worker, &data) == 0){
                        active_threads++;
                }else{
                        printf("WARNING: Failed to create Thread\n");
                }
        }

        for(int i = 0; i < active_threads; i++){
                pthread_join(threads[i], NULL);
        }

        pthread_mutex_destroy(&data.lock);

        PyEval_RestoreThread(mainThreadState);
        Py_Finalize();

        printf("SCAN COMPLETED: %d ports checked\n",total);
        return 0;
}
