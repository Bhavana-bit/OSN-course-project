// client.c (UPDATED FULL VERSION)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define NM_PORT 5000
#define SS_PORT 6000   // <<---- SET YOUR STORAGE SERVER PORT HERE
#define BUFFER 8192

// connect helper
int connect_to(int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in srv;
    memset(&srv,0,sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &srv.sin_addr);

    if (connect(sock,(struct sockaddr *)&srv,sizeof(srv)) < 0){
        close(sock);
        return -1;
    }

    return sock;
}

int main() {
    char cmd[BUFFER];

    printf("Client started.\n");

    while (1) {
        printf("> ");
        fflush(stdout);
        if (!fgets(cmd, sizeof(cmd), stdin)) break;
        cmd[strcspn(cmd, "\n")] = '\0';

        if (strcmp(cmd, "exit") == 0)
            break;

        // Detect STREAM (must connect directly to SS)
        if (strncmp(cmd, "STREAM ", 7) == 0)
        {
            char filename[512];
            strcpy(filename, cmd + 7);

            int ss = connect_to(SS_PORT);
            if (ss < 0) {
                printf("❌ Cannot connect to Storage Server\n");
                continue;
            }

            char sendline[BUFFER];
            snprintf(sendline, sizeof(sendline), "%s\n", cmd);
            send(ss, sendline, strlen(sendline), 0);

            // Receive words until SS closes
            char rbuf[BUFFER];
            ssize_t rn;

            while ((rn = read(ss, rbuf, sizeof(rbuf)-1)) > 0)
            {
                rbuf[rn] = '\0';
                printf("%s", rbuf);
                fflush(stdout);
            }

            if (rn <= 0)
                printf("\n⚠ SS disconnected during stream.\n");

            close(ss);
            continue;
        }

        // All other commands go to NM
        int nm = connect_to(NM_PORT);
        if (nm < 0) {
            printf("❌ Cannot connect to Name Manager\n");
            continue;
        }

        char sendline[BUFFER];
        snprintf(sendline, sizeof(sendline), "%s\n", cmd);
        send(nm, sendline, strlen(sendline), 0);

        // WRITE interactive mode
        if (strncmp(cmd, "WRITE ", 6) == 0) {
            while (1) {
                char op[BUFFER];
                if (!fgets(op, sizeof(op), stdin)) break;

                if (send(nm, op, strlen(op), 0) < 0)
                    break;

                char tmp[BUFFER];
                strcpy(tmp, op);
                tmp[strcspn(tmp, "\r\n")] = '\0';

                if (strcmp(tmp, "ETIRW") == 0)
                    break;
            }

            char rbuf[BUFFER];
            ssize_t rn;
            while ((rn = read(nm, rbuf, sizeof(rbuf)-1)) > 0) {
                rbuf[rn] = '\0';
                printf("%s", rbuf);
            }

            close(nm);
            continue;
        }

        // Non-WRITE read response
        char rbuf[BUFFER];
        ssize_t rn;
        while ((rn = read(nm, rbuf, sizeof(rbuf)-1)) > 0) {
            rbuf[rn] = '\0';
            printf("%s", rbuf);
        }

        close(nm);
    }

    printf("Client exited.\n");
    return 0;
}
