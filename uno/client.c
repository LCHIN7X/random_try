#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

int main() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_in server = {0};
    server.sin_family = AF_INET;
    server.sin_port = htons(9000);
    inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);

    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        perror("connect"); return 1;
    }

    char buf[2048];
    char full[4096];

    while (1) {
        full[0] = '\0';
        while (1) {
            int n = recv(sock, buf, sizeof(buf)-1, 0);
            if (n <= 0) return 0;
            buf[n] = '\0';
            strcat(full, buf);
            if (strstr(full, "> END")) break;
        }
        printf("%s", full);

        char input[64];
        fgets(input, sizeof(input), stdin);
        send(sock, input, strlen(input), 0);
    }
}