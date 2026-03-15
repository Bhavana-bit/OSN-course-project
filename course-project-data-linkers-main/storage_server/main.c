#include "server.h"
#include "logging.h"
#include "errors.h"

/*
   Storage Server MAIN
   - Initializes data directory
   - Registers with Name Manager (with SS_ID + heartbeat)
   - Accepts and handles commands
*/

int main()
{
    /* Prepare data folder */
    ensure_data_dir();

    /* Register SS with Name Server (Fault-tolerant version) */
    int id = register_ss_with_nm();
    if (id < 0)
    {
        fprintf(stderr, "[SS] ERROR: Failed to register with Name Manager.\n");
        log_event("SS", "FATAL", "Could not register with NM");
        exit(EXIT_FAILURE);
    }

    /* Setup logging AFTER registration so logs include SS_ID */
    log_init("ss.log");
    log_event("SS", "START", "Storage Server started");

    /* Create listening socket */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket");
        log_event("SS", "FATAL", "Socket creation failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        log_event("SS", "FATAL", "Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 16) < 0)
    {
        perror("listen");
        log_event("SS", "FATAL", "Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("[SS] Storage Server running on port %d...\n", PORT);
    log_event("SS", "INFO", "Listening for client connections");

    /* ======================== MAIN LOOP ======================== */
    while (1)
    {
        int cs = accept(server_fd, NULL, NULL);
        if (cs < 0)
        {
            log_event("SS", "ERROR", "Accept failed");
            continue;
        }

        log_event("SS", "CONNECT", "Client connected");

        char buf[BUFFER];
        ssize_t n = read(cs, buf, sizeof(buf) - 1);
        if (n <= 0)
        {
            log_event("SS", "ERROR", "Client sent no data");
            close(cs);
            continue;
        }

        buf[n] = '\0';
        buf[strcspn(buf, "\r\n")] = '\0';  // Clean newline

        /* Log incoming command */
        char logmsg[512];
        snprintf(logmsg, sizeof(logmsg), "Received command: '%s'", buf);
        log_event("SS", "COMMAND", logmsg);

        /* trim leading spaces */
        char *cmd = buf;
        while (*cmd == ' ') cmd++;

        /* ------------------ COMMAND ROUTING ------------------ */
        if      (strncmp(cmd, "CREATE ", 7) == 0)          handle_create(cmd + 7, cs);
        else if (strncmp(cmd, "READ ", 5) == 0)            handle_read(cmd + 5, cs);
        else if (strncmp(cmd, "VIEW", 4) == 0)             handle_view(cmd, cs);
        else if (strncmp(cmd, "WRITE ", 6) == 0)           handle_write(cmd + 6, cs);
        else if (strncmp(cmd, "UNDO ", 5) == 0)            handle_undo(cmd + 5, cs);
        else if (strncmp(cmd, "INFO ", 5) == 0)            handle_info(cmd + 5, cs);
        else if (strncmp(cmd, "DELETE ", 7) == 0)          handle_delete(cmd + 7, cs);
        else if (strncmp(cmd, "STREAM ", 7) == 0)          handle_stream(cmd + 7, cs);
        else if (strcmp(cmd,  "LIST") == 0)                handle_list(cs);

        else if (strncmp(cmd, "ADDACCESS ", 10) == 0)      handle_addaccess(cmd + 10, cs);
        else if (strncmp(cmd, "REMACCESS ", 10) == 0)      handle_remaccess(cmd + 10, cs);

        else if (strncmp(cmd, "EXEC ", 5) == 0)            handle_exec(cmd + 5, cs);

        /* FOLDER SYSTEM */
        else if (strncmp(cmd, "CREATEFOLDER ", 13) == 0)   handle_createfolder(cmd + 13, cs);
        else if (strncmp(cmd, "VIEWFOLDER ", 11) == 0)     handle_viewfolder(cmd + 11, cs);
        else if (strncmp(cmd, "MOVE ", 5) == 0)            handle_move(cmd + 5, cs);

        /* CHECKPOINT SYSTEM */
        else if (strncmp(cmd, "CHECKPOINT ", 11) == 0)     handle_checkpoint(cmd + 11, cs);
        else if (strncmp(cmd, "VIEWCHECKPOINT ", 15) == 0) handle_viewcheckpoint(cmd + 15, cs);
        else if (strncmp(cmd, "REVERT ", 7) == 0)          handle_revert(cmd + 7, cs);
        else if (strncmp(cmd, "LISTCHECKPOINTS ", 16) == 0) handle_listcheckpoints(cmd + 16, cs);

        /* Unknown command */
        else {
            send_error(cs, E400_INVALID_CMD);
            log_event("SS", "INVALID", cmd);
        }

        log_event("SS", "DISCONNECT", "Client disconnected");
        close(cs);
    }

    /* Cleanup (will not normally reach here) */
    log_event("SS", "STOP", "Storage Server shutting down");
    log_close();
    close(server_fd);
    return 0;
}
