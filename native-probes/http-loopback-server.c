#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    static const char response[] =
        "HTTP/1.0 200 OK\r\nContent-Length: 15\r\nConnection: close\r\n\r\n"
        "phase5_http_ok\n";
    struct sockaddr_storage storage;
    struct sockaddr *address = (struct sockaddr *)&storage;
    socklen_t address_length;
    int family = argc > 1 && strcmp(argv[1], "6") == 0 ? AF_INET6 : AF_INET;
    int enabled = 1;
    int listener;
    int client;
    char request[1024];
    ssize_t used;
    size_t sent = 0;

    listener = socket(family, SOCK_STREAM, 0);
    if (listener < 0) { perror("socket"); return 1; }
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled,
            sizeof(enabled)) != 0) { perror("setsockopt"); return 2; }
    memset(&storage, 0, sizeof(storage));
    if (family == AF_INET) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)&storage;
        ipv4->sin_len = sizeof(*ipv4);
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = htons(18080);
        ipv4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address_length = sizeof(*ipv4);
    } else {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)&storage;
        ipv6->sin6_len = sizeof(*ipv6);
        ipv6->sin6_family = AF_INET6;
        ipv6->sin6_port = htons(18080);
        ipv6->sin6_addr = in6addr_loopback;
        address_length = sizeof(*ipv6);
    }
    if (bind(listener, address, address_length) != 0) {
        perror("bind"); return 3;
    }
    if (listen(listener, 1) != 0) { perror("listen"); return 4; }
    puts("native_http_ready");
    fflush(stdout);
    client = accept(listener, 0, 0);
    if (client < 0) { perror("accept"); return 5; }
    used = recv(client, request, sizeof(request), 0);
    if (used <= 0) { perror("recv"); return 6; }
    while (sent != sizeof(response) - 1) {
        ssize_t result = send(client, response + sent,
            sizeof(response) - 1 - sent, 0);
        if (result <= 0) { perror("send"); return 7; }
        sent += (size_t)result;
    }
    shutdown(client, SHUT_RDWR);
    close(client);
    close(listener);
    return 0;
}
