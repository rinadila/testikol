#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <libwebsockets.h>

#define LOCAL_PORT 3333
#define VPS_IP "52.173.163.138"
#define VPS_WS_PORT 8080
#define MAX_CLIENTS 10
#define BUFFER_SIZE 4096

typedef struct {
    int miner_fd;
    struct lws *ws_wsi;
} client_t;

client_t clients[MAX_CLIENTS];
int client_count = 0;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// ===== WebSocket callback =====
static int callback_ws_client(struct lws *wsi,
                              enum lws_callback_reasons reason,
                              void *user, void *in, size_t len) {
    client_t *c = (client_t *)user;

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            printf("[*] WebSocket connected to VPS\n");
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            if (c->miner_fd > 0) write(c->miner_fd, in, len);
            break;

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        case LWS_CALLBACK_CLOSED:
            c->ws_wsi = NULL;
            break;

        default:
            break;
    }
    return 0;
}

static struct lws_protocols protocols[] = {
    { "ws-client-proxy", callback_ws_client, sizeof(client_t), BUFFER_SIZE },
    { NULL, NULL, 0, 0 }
};

// ===== Thread miner → WS =====
void *miner_to_ws_thread(void *arg) {
    client_t *c = (client_t *)arg;
    char buffer[BUFFER_SIZE];
    while (1) {
        int n = read(c->miner_fd, buffer, sizeof(buffer));
        if (n <= 0) break;
        if (c->ws_wsi) {
            unsigned char *out = malloc(LWS_PRE + n);
            memcpy(out + LWS_PRE, buffer, n);
            lws_write(c->ws_wsi, out + LWS_PRE, n, LWS_WRITE_BINARY);
            free(out);
        }
    }
    close(c->miner_fd);
    c->miner_fd = -1;
    return NULL;
}

// ===== Accept miner lokal =====
void *accept_thread(void *arg) {
    int local_sock = *(int *)arg;
    while (1) {
        int fd = accept(local_sock, NULL, NULL);
        if (fd < 0) continue;

        pthread_mutex_lock(&clients_mutex);
        if (client_count >= MAX_CLIENTS) { close(fd); pthread_mutex_unlock(&clients_mutex); continue; }

        client_t *c = &clients[client_count++];
        c->miner_fd = fd;
        c->ws_wsi = NULL;
        pthread_mutex_unlock(&clients_mutex);

        pthread_t t;
        pthread_create(&t, NULL, miner_to_ws_thread, c);
        pthread_detach(t);
        printf("[*] Miner connected\n");
    }
    return NULL;
}

int main() {
    int local_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(LOCAL_PORT);
    bind(local_sock, (struct sockaddr*)&addr, sizeof(addr));
    listen(local_sock, 5);

    printf("[*] Listening on 127.0.0.1:%d for miner\n", LOCAL_PORT);

    pthread_t accept_t;
    pthread_create(&accept_t, NULL, accept_thread, &local_sock);
    pthread_detach(accept_t);

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;

    struct lws_context *context = lws_create_context(&info);
    if (!context) { fprintf(stderr, "Failed to create WS context\n"); return 1; }

    while (1) {
        pthread_mutex_lock(&clients_mutex);
        for (int i = 0; i < client_count; i++) {
            client_t *c = &clients[i];
            if (!c->ws_wsi && c->miner_fd > 0) {
                struct lws_client_connect_info ccinfo = {0};
                ccinfo.context = context;
                ccinfo.address = VPS_IP;
                ccinfo.port = VPS_WS_PORT;
                ccinfo.path = "/";
                ccinfo.host = VPS_IP;
                ccinfo.origin = VPS_IP;
                ccinfo.protocol = protocols[0].name;
                ccinfo.userdata = c;
                c->ws_wsi = lws_client_connect_via_info(&ccinfo);
            }
        }
        pthread_mutex_unlock(&clients_mutex);
        lws_service(context, 100);
        usleep(1000);
    }

    close(local_sock);
    lws_context_destroy(context);
    return 0;
}
