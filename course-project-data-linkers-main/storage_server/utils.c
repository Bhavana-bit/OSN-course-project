#include "server.h"
#include "logging.h"
#include "errors.h"
#include <pthread.h>
#include <ctype.h>

static int ss_id = -1;         // Assigned by NM
static int replica_of = -1;    // If this SS is a replica for another SS
static int active = 1;

/* ==========================================================
   sendall() – safe sending
========================================================== */
ssize_t sendall(int sock, const char *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len)
    {
        ssize_t n = send(sock, buf + sent, len - sent, 0);
        if (n <= 0)
        {
            log_event("SS", "ERROR", "sendall(): socket write failed");
            return -1;
        }
        sent += n;
    }
    return sent;
}

/* ==========================================================
   HEARTBEAT THREAD – every 5 seconds
========================================================== */
void *heartbeat_thread(void *arg)
{
    while (active)
    {
        int sock = socket(AF_INET, SOCK_STREAM, 0);

        if (sock >= 0)
        {
            struct sockaddr_in nm;
            memset(&nm, 0, sizeof(nm));
            nm.sin_family = AF_INET;
            nm.sin_port = htons(NM_PORT);
            inet_pton(AF_INET, "127.0.0.1", &nm.sin_addr);

            if (connect(sock, (struct sockaddr *)&nm, sizeof(nm)) >= 0)
            {
                char msg[64];
                snprintf(msg, sizeof(msg), "HEARTBEAT %d\n", ss_id);
                sendall(sock, msg, strlen(msg));
            }
            close(sock);
        }
        sleep(5);
    }
    return NULL;
}

/* ==========================================================
   REGISTER STORAGE SERVER WITH NM
   Protocol: REGISTER_SS <port>
   NM responds: SS_ID <id> REPLICA_OF <id/-1>
========================================================== */
int register_ss_with_nm()
{
    log_event("SS", "STARTUP", "Registering SS with Name Manager");

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    struct sockaddr_in nm;
    memset(&nm, 0, sizeof(nm));
    nm.sin_family = AF_INET;
    nm.sin_port = htons(NM_PORT);
    inet_pton(AF_INET, "127.0.0.1", &nm.sin_addr);

    if (connect(sock, (struct sockaddr *)&nm, sizeof(nm)) < 0)
    {
        close(sock);
        log_event("SS", "ERROR", "Could not connect to NM");
        return -1;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "REGISTER_SS %d\n", PORT);
    sendall(sock, msg, strlen(msg));

    char resp[128];
    int n = recv(sock, resp, sizeof(resp) - 1, 0);
    close(sock);

    if (n <= 0)
        return -1;

    resp[n] = '\0';

    if (sscanf(resp, "SS_ID %d REPLICA_OF %d", &ss_id, &replica_of) == 2)
    {
        log_event("SS", "REGISTER", "SS successfully registered with NM");

        pthread_t hb;
        pthread_create(&hb, NULL, heartbeat_thread, NULL);
        pthread_detach(hb);

        return ss_id;
    }

    log_event("SS", "ERROR", "Invalid reply from NM during registration");
    return -1;
}

/* ==========================================================
   Notify NM after recovery
========================================================== */
void notify_nm_reconnected()
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return;

    struct sockaddr_in nm;
    memset(&nm, 0, sizeof(nm));
    nm.sin_family = AF_INET;
    nm.sin_port = htons(NM_PORT);
    inet_pton(AF_INET, "127.0.0.1", &nm.sin_addr);

    if (connect(sock, (struct sockaddr *)&nm, sizeof(nm)) >= 0)
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "RECOVER_SS %d\n", ss_id);
        sendall(sock, msg, strlen(msg));
    }
    close(sock);
}

/* ==========================================================
   REPLICATION: Push file to replica SS
========================================================== */
void replicate_file_to(int replica_port, const char *filename)
{
    char logmsg[256];
    snprintf(logmsg, sizeof(logmsg), "Replicating %s → SS(port=%d)", filename, replica_port);
    log_event("SS", "REPL", logmsg);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return;

    struct sockaddr_in r;
    memset(&r, 0, sizeof(r));
    r.sin_family = AF_INET;
    r.sin_port = htons(replica_port);
    inet_pton(AF_INET, "127.0.0.1", &r.sin_addr);

    if (connect(sock, (struct sockaddr *)&r, sizeof(r)) >= 0)
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "REPL_WRITE %s\n", filename);
        sendall(sock, msg, strlen(msg));

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", DATA_PATH, filename);

        char *content = read_whole_file(path);
        if (content)
        {
            sendall(sock, content, strlen(content));
            free(content);
        }
    }

    close(sock);
}

/* ==========================================================
   ensure_data_dir()
========================================================== */
void ensure_data_dir()
{
    struct stat st;
    if (stat(DATA_PATH, &st) != 0)
    {
        mkdir(DATA_PATH, 0755);
        log_event("SS", "INFO", "Created data directory");
    }
    else
        log_event("SS", "INFO", "Data directory exists");
}

/* ==========================================================
   Time formatting
========================================================== */
void format_time_now(char *out, size_t outlen)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(out, outlen, "%Y-%m-%d %H:%M:%S", &tm);
}

/* ==========================================================
   write_meta_simple()
========================================================== */
void write_meta_simple(const char *filename, int words, int chars, const char *owner)
{
    char meta_path[512];
    snprintf(meta_path, sizeof(meta_path), "%s/%s.meta", DATA_PATH, filename);

    char ts[64];
    format_time_now(ts, sizeof(ts));

    FILE *m = fopen(meta_path, "w");
    if (!m)
        return;

    fprintf(m, "%d|%d|%s|%s\n", words, chars, ts, owner);
    fclose(m);
}

/* ==========================================================
   read_meta()
========================================================== */
int read_meta(const char *filename, int *words, int *chars,
              char *owner, size_t owner_len,
              char *timestamp, size_t ts_len)
{
    char meta_path[512];
    snprintf(meta_path, sizeof(meta_path), "%s/%s.meta", DATA_PATH, filename);

    FILE *m = fopen(meta_path, "r");
    if (!m)
        return 0;

    char line[512];
    fgets(line, sizeof(line), m);
    fclose(m);

    int w, c;
    char ts[128], own[128];

    if (sscanf(line, "%d|%d|%127[^|]|%127[^\n]", &w, &c, ts, own) == 4)
    {
        *words = w;
        *chars = c;
        strncpy(timestamp, ts, ts_len - 1);
        strncpy(owner, own, owner_len - 1);
        return 1;
    }
    return 0;
}

/* ==========================================================
   update_metadata()
========================================================== */
void update_metadata(const char *filename, const char *owner)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", DATA_PATH, filename);

    FILE *fp = fopen(path, "r");
    if (!fp)
        return;

    int words = 0, chars = 0;
    int ch, in_word = 0;

    while ((ch = fgetc(fp)) != EOF)
    {
        chars++;
        if (isspace(ch))
            in_word = 0;
        else if (!in_word)
        {
            in_word = 1;
            words++;
        }
    }
    fclose(fp);

    write_meta_simple(filename, words, chars, owner);
}

/* ==========================================================
   read_whole_file()
========================================================== */
char *read_whole_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;

    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (n < 0)
    {
        fclose(f);
        return NULL;
    }

    char *buf = malloc(n + 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }

    fread(buf, 1, n, f);
    buf[n] = '\0';
    fclose(f);

    return buf;
}

/* ==========================================================
   write_whole_file()
========================================================== */
int write_whole_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;

    fputs(content, f);
    fclose(f);
    return 1;
}
