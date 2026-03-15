// nm.c
// Name Manager (NM) - handles SS registration, heartbeat, liveness, forwarding and basic replication
// Compatible with SS that sends: "REGISTER_SS <port>\n", "HEARTBEAT <ss_id>\n", "RECOVER_SS <ss_id>\n"
// and expects response "SS_ID <id> REPLICA_OF <replica_of>\n" on registration.
//
// - Forwards client commands to an available SS (round-robin / first-available).
// - For WRITE commands, acts as interactive relay (client -> SS until ETIRW, then SS -> client).
// - Performs simple asynchronous replication for CREATE and DELETE by broadcasting same command to other SSs.
//
// NOTE: adjust constants (NM_PORT) via server.h
//
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>

#include "logging.h"
#include "errors.h"

// NM listens on port 5000
#define NM_PORT 5000

// Maximum SS entries
#define MAX_SS 128


#define MAX_SS 128
#define SS_TIMEOUT_SECS 15   // if no heartbeat for this many seconds, mark SS as down

typedef struct {
    int id;             // assigned SS id (>=1)
    int port;           // port where SS listens
    int up;             // 1 = up, 0 = down
    int replica_of;     // id of primary this SS is replica of (or -1)
    time_t last_seen;   // time of last heartbeat or registration
} ss_entry;

static ss_entry ss_list[MAX_SS];
static int ss_count = 0;
static int next_ss_id = 1;
static pthread_mutex_t ss_lock = PTHREAD_MUTEX_INITIALIZER;
static int nm_port_global = NM_PORT;

/* sendall helper */
ssize_t sendall(int sock, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = send(sock, buf + sent, len - sent, 0);
        if (n <= 0)
            return -1;
        sent += n;
    }
    return sent;
}

/* find first active SS; -1 if none */
static int pick_active_ss()
{
    pthread_mutex_lock(&ss_lock);
    for (int i = 0; i < ss_count; ++i)
    {
        if (ss_list[i].up)
        {
            int id = ss_list[i].id;
            pthread_mutex_unlock(&ss_lock);
            return id;
        }
    }
    pthread_mutex_unlock(&ss_lock);
    return -1;
}

/* get ss_entry by id; returns pointer or NULL */
static ss_entry *get_ss_by_id(int id)
{
    for (int i = 0; i < ss_count; ++i)
        if (ss_list[i].id == id)
            return &ss_list[i];
    return NULL;
}

/* get index in array by id, or -1 */
static int get_ss_index(int id)
{
    for (int i = 0; i < ss_count; ++i)
        if (ss_list[i].id == id)
            return i;
    return -1;
}

/* mark SS up */
static void mark_ss_up(int id, int port)
{
    pthread_mutex_lock(&ss_lock);
    int idx = get_ss_index(id);
    if (idx >= 0)
    {
        ss_list[idx].up = 1;
        ss_list[idx].port = port;
        ss_list[idx].last_seen = time(NULL);
    }
    pthread_mutex_unlock(&ss_lock);
}

/* add a new SS (returns assigned id) */
static int add_ss(int port, int *assigned_replica_of)
{
    pthread_mutex_lock(&ss_lock);
    if (ss_count >= MAX_SS)
    {
        pthread_mutex_unlock(&ss_lock);
        return -1;
    }

    int id = next_ss_id++;
    ss_entry e;
    e.id = id;
    e.port = port;
    e.up = 1;
    e.last_seen = time(NULL);
    e.replica_of = -1; // simple: no replica assignment here
    ss_list[ss_count++] = e;

    // simple replication pairing: if there's an existing primary without replica, assign that as primary and mark this as its replica_of
    // find a primary that does not yet have a replica (replica_of == -1 and id != e.id)
    for (int i = 0; i < ss_count - 1; ++i)
    {
        if (ss_list[i].replica_of == -1 && ss_list[i].id != id)
        {
            // assign this new server as replica for that primary if that primary is up
            ss_list[ss_count-1].replica_of = ss_list[i].id;
            break;
        }
    }

    if (assigned_replica_of)
        *assigned_replica_of = ss_list[ss_count-1].replica_of;

    pthread_mutex_unlock(&ss_lock);
    return id;
}

/* background thread: check for timeouts */
static void *watchdog_thread(void *arg)
{
    (void)arg;
    while (1)
    {
        time_t now = time(NULL);
        pthread_mutex_lock(&ss_lock);
        for (int i = 0; i < ss_count; ++i)
        {
            if (ss_list[i].up && (now - ss_list[i].last_seen) > SS_TIMEOUT_SECS)
            {
                char msg[128];
                snprintf(msg, sizeof(msg), "SS %d timed out (last_seen=%ld)", ss_list[i].id, (long)ss_list[i].last_seen);
                log_event("NM", "TIMEOUT", msg);
                ss_list[i].up = 0;
            }
        }
        pthread_mutex_unlock(&ss_lock);
        sleep(3);
    }
    return NULL;
}

/* asynchronously replicate command string to all other SSs (simple broadcast).
   cmd_line must be a NUL-terminated string (no trailing newline required).
   This runs in a detached thread.*/
typedef struct {
    char *cmd;
    int exclude_id; // don't send to this one (e.g. primary)
} repl_job;

static void *repl_worker(void *arg)
{
    repl_job *job = (repl_job *)arg;
    if (!job) return NULL;

    pthread_mutex_lock(&ss_lock);
    for (int i = 0; i < ss_count; ++i)
    {
        if (!ss_list[i].up) continue;
        if (ss_list[i].id == job->exclude_id) continue;

        int port = ss_list[i].port;
        pthread_mutex_unlock(&ss_lock);

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
            pthread_mutex_lock(&ss_lock);
            continue;
        }

        struct sockaddr_in saddr;
        memset(&saddr, 0, sizeof(saddr));
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &saddr.sin_addr);

        if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) >= 0)
        {
            // send the command and close
            char out[4096];
            snprintf(out, sizeof(out), "%s\n", job->cmd);
            sendall(sock, out, strlen(out));
        }
        close(sock);

        pthread_mutex_lock(&ss_lock);
    }
    pthread_mutex_unlock(&ss_lock);

    free(job->cmd);
    free(job);
    return NULL;
}

static void spawn_replication_broadcast(const char *cmd_line, int exclude_id)
{
    repl_job *job = malloc(sizeof(repl_job));
    if (!job) return;
    job->cmd = strdup(cmd_line);
    job->exclude_id = exclude_id;

    pthread_t t;
    pthread_create(&t, NULL, repl_worker, job);
    pthread_detach(t);
}

/* Relay interactive WRITE: client->SS until ETIRW, then SS->client */
static void handle_interactive_write_relay(int client_sock, int ss_sock)
{
    char buf[4096];
    ssize_t n;
    int saw_etirw = 0;

    // forward client->ss until ETIRW observed in client stream
    while ((n = read(client_sock, buf, sizeof(buf))) > 0)
    {
        if (sendall(ss_sock, buf, (size_t)n) < 0) break;

        if (memmem(buf, (size_t)n, "ETIRW", 5) != NULL)
        {
            saw_etirw = 1;
            break;
        }
    }

    // Now read from SS and forward to client until SS closes
    while ((n = read(ss_sock, buf, sizeof(buf))) > 0)
    {
        if (sendall(client_sock, buf, (size_t)n) < 0) break;
    }
    (void)saw_etirw;
}

/* Forward non-interactive command to SS and pipe response back; optionally return response buffer */
static int forward_command_and_stream_response(int client_sock, int ss_port, const char *cmd)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(ss_port);
    inet_pton(AF_INET, "127.0.0.1", &saddr.sin_addr);

    if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0)
    {
        close(sock);
        return -1;
    }

    char cmdline[4096];
    snprintf(cmdline, sizeof(cmdline), "%s\n", cmd);
    if (sendall(sock, cmdline, strlen(cmdline)) < 0)
    {
        close(sock);
        return -1;
    }

    // read response from SS and forward to client
    char buf[4096];
    ssize_t r;
    while ((r = read(sock, buf, sizeof(buf))) > 0)
    {
        if (sendall(client_sock, buf, (size_t)r) < 0) break;
    }

    close(sock);
    return 0;
}

/* handle client commands (connected to NM) */
static void handle_client_connection(int cs)
{
    char buf[8192];
    ssize_t n = read(cs, buf, sizeof(buf)-1);
    if (n <= 0) { close(cs); return; }
    buf[n] = '\0';
    buf[strcspn(buf, "\r\n")] = '\0';

    char logmsg[1024];
    snprintf(logmsg, sizeof(logmsg), "Client command: '%s'", buf);
    log_event("NM", "REQUEST", logmsg);

    // SEARCH can be served locally (not implemented here) — for now forward to SS
    // pick an active SS
    int ss_id = pick_active_ss();
    if (ss_id < 0)
    {
        send_error(cs, E503_NM_FAILURE);
        log_event("NM", "ERROR", "No SS available (all down)");
        close(cs);
        return;
    }

    // get SS port
    pthread_mutex_lock(&ss_lock);
    int idx = get_ss_index(ss_id);
    int ss_port = ss_list[idx].port;
    pthread_mutex_unlock(&ss_lock);

    // If command is WRITE, do interactive relay
    char *cmd = buf;
    while (*cmd == ' ') cmd++;

    if (strncmp(cmd, "WRITE ", 6) == 0)
    {
        // send initial WRITE line to SS then relay interactive session
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
            send_error(cs, E500_INTERNAL);
            close(cs);
            return;
        }
        struct sockaddr_in saddr;
        memset(&saddr, 0, sizeof(saddr));
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons(ss_port);
        inet_pton(AF_INET, "127.0.0.1", &saddr.sin_addr);

        if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0)
        {
            send_error(cs, E503_NM_FAILURE);
            close(sock);
            close(cs);
            return;
        }

        // send the WRITE header line
        char writeline[4096];
        snprintf(writeline, sizeof(writeline), "%s\n", cmd);
        if (sendall(sock, writeline, strlen(writeline)) < 0)
        {
            send_error(cs, E500_INTERNAL);
            close(sock);
            close(cs);
            return;
        }

        // now relay client <-> SS interactively
        handle_interactive_write_relay(cs, sock);
        close(sock);

        // NOTE: we cannot easily replicate interactive writes here without capturing all client data;
        // in this implementation we don't replicate interactive write content. (You can extend to capture and replay.)
        close(cs);
        return;
    }

    // For CREATE/DELETE we also asynchronously broadcast the same command to other SSs for replication
    int is_create = (strncmp(cmd, "CREATE ", 7) == 0);
    int is_delete = (strncmp(cmd, "DELETE ", 7) == 0);

    // Forward to chosen SS and stream response
    if (forward_command_and_stream_response(cs, ss_port, cmd) < 0)
    {
        send_error(cs, E503_NM_FAILURE);
        close(cs);
        return;
    }

    // spawn replication broadcast in background (doesn't wait)
    if (is_create || is_delete)
    {
        spawn_replication_broadcast(cmd, ss_id);
    }

    close(cs);
}

/* handle SS registration/heartbeat/recover requests */
static void handle_nm_control(int cs)
{
    char buf[1024];
    ssize_t n = read(cs, buf, sizeof(buf)-1);
    if (n <= 0) { close(cs); return; }
    buf[n] = '\0';
    buf[strcspn(buf, "\r\n")] = '\0';

    char logmsg[256];
    snprintf(logmsg, sizeof(logmsg), "Control message: '%s'", buf);
    log_event("NM", "CONTROL", logmsg);

    char *p = buf;
    while (*p == ' ') p++;

    if (strncmp(p, "REGISTER_SS ", 12) == 0)
    {
        int port = atoi(p + 12);
        if (port <= 0)
        {
            send_error(cs, E400_INVALID_CMD);
            close(cs);
            return;
        }
        int assigned_replica_of = -1;
        int id = add_ss(port, &assigned_replica_of);
        if (id < 0)
        {
            send_error(cs, E500_INTERNAL);
            close(cs);
            return;
        }

        // respond with SS_ID <id> REPLICA_OF <replica_of>\n
        char out[128];
        snprintf(out, sizeof(out), "SS_ID %d REPLICA_OF %d\n", id, assigned_replica_of >= 0 ? assigned_replica_of : -1);
        sendall(cs, out, strlen(out));

        char msg[128];
        snprintf(msg, sizeof(msg), "SS REGISTERED on port %d (id=%d, replica_of=%d)", port, id, assigned_replica_of);
        log_event("NM", "REGISTER", msg);
        close(cs);
        return;
    }

    if (strncmp(p, "HEARTBEAT ", 10) == 0)
    {
        int id = atoi(p + 10);
        pthread_mutex_lock(&ss_lock);
        int idx = get_ss_index(id);
        if (idx >= 0)
        {
            ss_list[idx].last_seen = time(NULL);
            ss_list[idx].up = 1;
            pthread_mutex_unlock(&ss_lock);
            sendall(cs, "OK\n", 3);
            close(cs);
            return;
        }
        pthread_mutex_unlock(&ss_lock);
        send_error(cs, E400_INVALID_CMD);
        close(cs);
        return;
    }

    if (strncmp(p, "RECOVER_SS ", 11) == 0)
    {
        int id = atoi(p + 11);
        pthread_mutex_lock(&ss_lock);
        int idx = get_ss_index(id);
        if (idx >= 0)
        {
            ss_list[idx].up = 1;
            ss_list[idx].last_seen = time(NULL);
            pthread_mutex_unlock(&ss_lock);
            sendall(cs, "RECOVERED\n", 10);
            log_event("NM", "RECOVER", "Recovered SS marked up");
            close(cs);
            return;
        }
        pthread_mutex_unlock(&ss_lock);
        send_error(cs, E400_INVALID_CMD);
        close(cs);
        return;
    }

    // also allow REGISTERU <user> <port> (user registration) — respond with REGISTERED\n
    if (strncmp(p, "REGISTERU ", 10) == 0)
    {
        // minimal handling: just accept and respond
        log_event("NM", "REGISTERU", "User registration forwarded/accepted");
        sendall(cs, "REGISTERED\n", 11);
        close(cs);
        return;
    }

    // unknown control
    send_error(cs, E400_INVALID_CMD);
    close(cs);
}

/* main loop */
int main()
{
    log_init("nm.log");
    log_event("NM", "START", "Name Server started");

    // initialize ss_list
    pthread_mutex_lock(&ss_lock);
    ss_count = 0;
    next_ss_id = 1;
    pthread_mutex_unlock(&ss_lock);

    // spawn watchdog
    pthread_t wd;
    pthread_create(&wd, NULL, watchdog_thread, NULL);
    pthread_detach(wd);

    // create listening socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket");
        exit(1);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(nm_port_global);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        exit(1);
    }

    if (listen(server_fd, 32) < 0)
    {
        perror("listen");
        exit(1);
    }

    printf("[NM] Running on port %d\n", nm_port_global);
    log_event("NM", "INFO", "Listening for connections");

    while (1)
    {
        int cs = accept(server_fd, NULL, NULL);
        if (cs < 0) continue;

        log_event("NM", "CONNECT", "Client/SS connected");

        // peek a little to decide if this is SS control msg (REGISTER/HEARTBEAT/RECOVER) or client command
        fd_set readfds;
        struct timeval tv;
        FD_ZERO(&readfds);
        FD_SET(cs, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms

        // try to read the first line without blocking long
        char buf[8192];
        ssize_t n = recv(cs, buf, sizeof(buf)-1, MSG_PEEK);
        if (n <= 0)
        {
            close(cs);
            continue;
        }

        // Read the actual line (blocking read); use a small buffer read
        n = read(cs, buf, sizeof(buf)-1);
        if (n <= 0) { close(cs); continue; }
        buf[n] = '\0';
        buf[strcspn(buf, "\r\n")] = '\0';

        // Determine if this looks like a control message from SS
        // control keywords: REGISTER_SS, HEARTBEAT, RECOVER_SS, REGISTERU
        char tmp[64];
        int scanned = sscanf(buf, "%63s", tmp);
        if (scanned == 1 &&
            (strcmp(tmp, "REGISTER_SS") == 0 ||
             strcmp(tmp, "HEARTBEAT") == 0 ||
             strcmp(tmp, "RECOVER_SS") == 0 ||
             strcmp(tmp, "REGISTERU") == 0))
        {
            // Push back the read data into a new temporary socket handler by using a small helper:
            // We've already consumed the data from socket, so call handle_nm_control using a small wrapper that uses the buffer.
            // For simplicity, write the buffer back into a new temporary handler: handle by creating a small child thread that processes this message.
            // We'll create a tiny function for this.
            // But simpler: we've already read the message content in 'buf' — we can mimic handle_nm_control by processing 'buf' directly here.

            // Process SS control directly:
            // Duplicate behaviour of handle_nm_control but using buf string
            char *p = buf;
            while (*p == ' ') p++;

            if (strncmp(p, "REGISTER_SS ", 12) == 0)
            {
                int port = atoi(p + 12);
                if (port <= 0)
                {
                    send_error(cs, E400_INVALID_CMD);
                    close(cs);
                    continue;
                }
                int assigned_replica_of = -1;
                int id = add_ss(port, &assigned_replica_of);
                if (id < 0)
                {
                    send_error(cs, E500_INTERNAL);
                    close(cs);
                    continue;
                }

                char out[128];
                snprintf(out, sizeof(out), "SS_ID %d REPLICA_OF %d\n", id, assigned_replica_of >= 0 ? assigned_replica_of : -1);
                sendall(cs, out, strlen(out));
                char m[128];
                snprintf(m, sizeof(m), "SS REGISTERED on port %d (id=%d, replica_of=%d)", port, id, assigned_replica_of);
                log_event("NM", "REGISTER", m);
                close(cs);
                continue;
            }
            else if (strncmp(p, "HEARTBEAT ", 10) == 0)
            {
                int id = atoi(p + 10);
                pthread_mutex_lock(&ss_lock);
                int idx = get_ss_index(id);
                if (idx >= 0)
                {
                    ss_list[idx].last_seen = time(NULL);
                    ss_list[idx].up = 1;
                    pthread_mutex_unlock(&ss_lock);
                    sendall(cs, "OK\n", 3);
                    close(cs);
                    continue;
                }
                pthread_mutex_unlock(&ss_lock);
                send_error(cs, E400_INVALID_CMD);
                close(cs);
                continue;
            }
            else if (strncmp(p, "RECOVER_SS ", 11) == 0)
            {
                int id = atoi(p + 11);
                pthread_mutex_lock(&ss_lock);
                int idx = get_ss_index(id);
                if (idx >= 0)
                {
                    ss_list[idx].up = 1;
                    ss_list[idx].last_seen = time(NULL);
                    pthread_mutex_unlock(&ss_lock);
                    sendall(cs, "RECOVERED\n", 10);
                    log_event("NM", "RECOVER", "Recovered SS marked up");
                    close(cs);
                    continue;
                }
                pthread_mutex_unlock(&ss_lock);
                send_error(cs, E400_INVALID_CMD);
                close(cs);
                continue;
            }
            else if (strncmp(p, "REGISTERU ", 10) == 0)
            {
                log_event("NM", "REGISTERU", "User registration accepted");
                sendall(cs, "REGISTERED\n", 11);
                close(cs);
                continue;
            }
            else
            {
                send_error(cs, E400_INVALID_CMD);
                close(cs);
                continue;
            }
        }
        else
        {
            // Treat as a client command — we already read the whole command into buf, so handle it directly
            // For that reuse handle_client_connection logic but since it reads from socket again (we already consumed), we'll process buf here.
            // To reuse existing handler, simplest is to create a temporary memory-based handling: forward the command in buf.
            // We'll implement logic inline similar to handle_client_connection but using 'buf' content and cs fd.

            // For simplicity forward by picking an active SS and sending buf.
            char *cmd = buf;
            while (*cmd == ' ') cmd++;

            int ssid = pick_active_ss();
            if (ssid < 0)
            {
                send_error(cs, E503_NM_FAILURE);
                log_event("NM", "ERROR", "No SS available (all down)");
                close(cs);
                continue;
            }
            pthread_mutex_lock(&ss_lock);
            int sidx = get_ss_index(ssid);
            int ss_port = ss_list[sidx].port;
            pthread_mutex_unlock(&ss_lock);

            // If WRITE -> interactive: we already consumed the initial line; but need to enter interactive relay.
            if (strncmp(cmd, "WRITE ", 6) == 0)
            {
                // connect to SS
                int sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock < 0) { send_error(cs, E500_INTERNAL); close(cs); continue; }
                struct sockaddr_in saddr;
                memset(&saddr, 0, sizeof(saddr));
                saddr.sin_family = AF_INET;
                saddr.sin_port = htons(ss_port);
                inet_pton(AF_INET, "127.0.0.1", &saddr.sin_addr);
                if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0)
                {
                    send_error(cs, E503_NM_FAILURE);
                    close(sock); close(cs); continue;
                }

                // send initial WRITE line
                char bufcmd[4096];
                snprintf(bufcmd, sizeof(bufcmd), "%s\n", cmd);
                sendall(sock, bufcmd, strlen(bufcmd));

                // now relay client <-> SS
                handle_interactive_write_relay(cs, sock);
                close(sock);
                close(cs);
                continue;
            }
            else
            {
                // non-interactive forward and stream response
                if (forward_command_and_stream_response(cs, ss_port, cmd) < 0)
                {
                    send_error(cs, E503_NM_FAILURE);
                    close(cs);
                    continue;
                }

                // replication for CREATE/DELETE
                if (strncmp(cmd, "CREATE ", 7) == 0 || strncmp(cmd, "DELETE ", 7) == 0)
                {
                    spawn_replication_broadcast(cmd, ssid);
                }

                close(cs);
                continue;
            }
        }
    } // end while accept loop

    // never reached
    close(server_fd);
    log_event("NM", "STOP", "Name Server shutting down");
    log_close();
    return 0;
}
