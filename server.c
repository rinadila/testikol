#include <libwebsockets.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define POOL_HOST "eu.luckpool.net"
#define POOL_PORT 3956
#define MAX_CLIENTS 50
#define BUFFER_SIZE 4096

typedef struct {
    struct lws *wsi;
    int pool_sock;
} client_t;

client_t clients[MAX_CLIENTS];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// ===== Forward data dari pool ke WebSocket =====
void forward_pool_to_ws(client_t *c) {
    char buffer[BUFFER_SIZE];
    int n = read(c->pool_sock, buffer, sizeof(buffer));
    if (n > 0) {
        unsigned char *out = malloc(LWS_PRE + n);
        memcpy(out + LWS_PRE, buffer, n);
        lws_write(c->wsi, out + LWS_PRE, n, LWS_WRITE_BINARY);
        free(out);
        lws_callback_on_writable(c->wsi);
    }
}

// ===== Callback WebSocket =====
static int callback_stratum_proxy(struct lws *wsi,
                                  enum lws_callback_reasons reason,
                                  void *user, void *in, size_t len) {
    client_t *c = (client_t *)user;

    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED: {
            pthread_mutex_lock(&clients_mutex);
            int found = 0;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].wsi == NULL) {
                    clients[i].wsi = wsi;
                    clients[i].pool_sock = socket(AF_INET, SOCK_STREAM, 0);
                    struct sockaddr_in addr;
                    addr.sin_family = AF_INET;
                    addr.sin_port = htons(POOL_PORT);
                    inet_pton(AF_INET, POOL_HOST, &addr.sin_addr);
                    if (connect(clients[i].pool_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                        close(clients[i].pool_sock);
                        clients[i].pool_sock = -1;
                        clients[i].wsi = NULL;
                    }
                    c = &clients[i];
                    found = 1;
                    break;
                }
            }
            pthread_mutex_unlock(&clients_mutex);
            if (!found) return -1;
            break;
        }

        case LWS_CALLBACK_RECEIVE:
            if (c->pool_sock > 0) write(c->pool_sock, in, len);
            break;

        case LWS_CALLBACK_SERVER_WRITEABLE:
            if (c->pool_sock > 0) forward_pool_to_ws(c);
            break;

        case LWS_CALLBACK_CLOSED:
            pthread_mutex_lock(&clients_mutex);
            if (c->pool_sock > 0) close(c->pool_sock);
            c->pool_sock = -1;
            c->wsi = NULL;
            pthread_mutex_unlock(&clients_mutex);
            break;

        default:
            break;
    }
    return 0;
}

static struct lws_protocols protocols[] = {
    { "stratum-proxy", callback_stratum_proxy, sizeof(client_t), BUFFER_SIZE },
    { NULL, NULL, 0, 0 }
};

int main(void) {
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = 8080;
    info.protocols = protocols;

    struct lws_context *context = lws_create_context(&info);
    if (!context) {
        fprintf(stderr, "Gagal membuat WebSocket server\n");
        return 1;
    }

    printf("VPS WebSocket proxy listening on port 8080\n");

    while (1) {
        lws_service(context, 1000);
        pthread_mutex_lock(&clients_mutex);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].wsi != NULL) lws_callback_on_writable(clients[i].wsi);
        }
        pthread_mutex_unlock(&clients_mutex);
    }

    lws_context_destroy(context);
    return 0;
}
