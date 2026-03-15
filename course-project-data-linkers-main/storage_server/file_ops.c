#include "server.h"
#include "logging.h"
#include "errors.h" // ensure errors available

#include <sys/stat.h>
#include <sys/types.h>
/* ----------------- VIEW helpers ----------------- */
void view_list(int cs, int show_all)
{
    log_event("SS", "VIEW", show_all ? "VIEW -a list" : "VIEW list");

    DIR *d = opendir(DATA_PATH);
    if (!d)
    {
        log_event("SS", "ERROR", "VIEW: Could not open data directory");
        send_error(cs, E500_INTERNAL);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL)
    {
        if (entry->d_name[0] == '.')
            continue;
        if (strstr(entry->d_name, ".meta") || strstr(entry->d_name, ".undo") || strstr(entry->d_name, ".access"))
            continue;

        char owner[128] = "unknown", ts[128] = "unknown";
        int w = 0, c = 0;

        read_meta(entry->d_name, &w, &c, owner, sizeof(owner), ts, sizeof(ts));

        if (!show_all && strcmp(owner, "user1") != 0)
            continue;

        char line[512];
        snprintf(line, sizeof(line), "--> %s\n", entry->d_name);
        sendall(cs, line, strlen(line));
    }

    closedir(d);
}

void view_long(int cs, int show_all)
{
    log_event("SS", "VIEW", show_all ? "VIEW -al long" : "VIEW -l long");

    DIR *d = opendir(DATA_PATH);
    if (!d)
    {
        log_event("SS", "ERROR", "VIEW LONG: Could not open data directory");
        send_error(cs, E500_INTERNAL);
        return;
    }

    sendall(cs,
            "---------------------------------------------------------\n"
            "|  Filename  | Words | Chars | Last Access Time | Owner |\n"
            "---------------------------------------------------------\n",
            strlen(
                "---------------------------------------------------------\n"
                "|  Filename  | Words | Chars | Last Access Time | Owner |\n"
                "---------------------------------------------------------\n"));

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL)
    {
        if (entry->d_name[0] == '.')
            continue;
        if (strstr(entry->d_name, ".meta") || strstr(entry->d_name, ".undo") || strstr(entry->d_name, ".access"))
            continue;

        int w = 0, c = 0;
        char owner[64] = "unknown", meta_time[64] = "unknown";

        read_meta(entry->d_name, &w, &c, owner, sizeof(owner), meta_time, sizeof(meta_time));

        if (!show_all && strcmp(owner, "user1") != 0)
            continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", DATA_PATH, entry->d_name);

        struct stat st;
        char atime_str[64] = "unknown";

        if (stat(path, &st) == 0)
        {
            struct tm tm;
            localtime_r(&st.st_atime, &tm);
            strftime(atime_str, sizeof(atime_str), "%Y-%m-%d %H:%M", &tm);
        }

        char row[512];
        snprintf(row, sizeof(row),
                 "| %-10s | %5d | %5d | %-16s | %-5s |\n",
                 entry->d_name, w, c, atime_str, owner);

        sendall(cs, row, strlen(row));
    }

    sendall(cs, "---------------------------------------------------------\n", strlen("---------------------------------------------------------\n"));
    closedir(d);
}

void handle_view(char *cmd, int cs)
{
    while (*cmd == ' ')
        cmd++;
    int len = strlen(cmd);
    while (len > 0 && (cmd[len - 1] == ' ' || cmd[len - 1] == '\r' || cmd[len - 1] == '\n'))
        cmd[--len] = '\0';

    log_event("SS", "COMMAND", cmd);

    if (strcmp(cmd, "VIEW") == 0)
        view_list(cs, 0);
    else if (strcmp(cmd, "VIEW -a") == 0)
        view_list(cs, 1);
    else if (strcmp(cmd, "VIEW -l") == 0)
        view_long(cs, 0);
    else if (strcmp(cmd, "VIEW -al") == 0 || strcmp(cmd, "VIEW -la") == 0)
        view_long(cs, 1);
    else {
        log_event("SS", "ERROR", "VIEW: Invalid usage");
        send_error(cs, E400_INVALID_CMD);
    }
}

/* ----------------- CREATE / READ / INFO / DELETE / STREAM ----------------- */
void handle_create(const char *fname_in, int cs)
{
    log_event("SS", "CREATE", fname_in ? fname_in : "NULL");

    if (!fname_in)
    {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    while (*fname_in == ' ')
        fname_in++;

    char fname[512];
    int i = 0;
    while (fname_in[i] && fname_in[i] != '\n' && i < 511)
        fname[i++] = fname_in[i];
    while (i > 0 && fname[i - 1] == ' ')
        i--;
    fname[i] = '\0';

    if (strlen(fname) == 0) {
        log_event("SS", "ERROR", "CREATE: empty filename");
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", DATA_PATH, fname);
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);

    if (fd < 0)
    {
        log_event("SS", "ERROR", "CREATE failed (exists or no permission)");
        /* If file exists -> conflict, otherwise internal */
        if (access(path, F_OK) == 0)
            send_error(cs, E409_CONFLICT);
        else
            send_error(cs, E500_INTERNAL);
        return;
    }

    close(fd);
    update_metadata(fname, "user1");

    char acc_path[512];
    snprintf(acc_path, sizeof(acc_path), "%s/%s.access", DATA_PATH, fname);
    FILE *acc = fopen(acc_path, "w");

    if (acc)
    {
        fprintf(acc, "user1:RW\n");
        fclose(acc);
    }
    else
    {
        log_event("SS", "WARN", "CREATE: could not create access file (non-fatal)");
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Created %s\n", fname);
    sendall(cs, msg, strlen(msg));

    log_event("SS", "CREATE_OK", fname);
}

void handle_read(const char *fname_in, int cs)
{
    log_event("SS", "READ", fname_in ? fname_in : "NULL");

    if (!fname_in)
    {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    while (*fname_in == ' ')
        fname_in++;

    char fname[512];
    int i = 0;
    while (fname_in[i] && fname_in[i] != '\n' && i < 511)
        fname[i++] = fname_in[i];
    while (i > 0 && fname[i - 1] == ' ')
        i--;
    fname[i] = '\0';

    if (strlen(fname) == 0) {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", DATA_PATH, fname);

    FILE *f = fopen(path, "r");
    if (!f)
    {
        log_event("SS", "ERROR", "READ: File not found");
        send_error(cs, E404_NOT_FOUND);
        return;
    }

    char buf[1024];
    while (fgets(buf, sizeof(buf), f))
        sendall(cs, buf, strlen(buf));

    fclose(f);
}

void handle_info(const char *fname_in, int cs)
{
    log_event("SS", "INFO", fname_in ? fname_in : "NULL");

    if (!fname_in)
    {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    while (*fname_in == ' ')
        fname_in++;

    char fname[512];
    int i = 0;
    while (fname_in[i] && fname_in[i] != '\n' && i < 511)
        fname[i++] = fname_in[i];
    while (i > 0 && fname[i - 1] == ' ')
        i--;
    fname[i] = '\0';

    if (strlen(fname) == 0) {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    char path[512], meta_path[512];
    snprintf(path, sizeof(path), "%s/%s", DATA_PATH, fname);
    snprintf(meta_path, sizeof(meta_path), "%s/%s.meta", DATA_PATH, fname);

    struct stat st;
    if (stat(path, &st) != 0)
    {
        log_event("SS", "ERROR", "INFO: File not found");
        send_error(cs, E404_NOT_FOUND);
        return;
    }

    int words = 0, chars = 0;
    char owner[128] = "unknown";
    char ts_created[128] = "unknown";

    FILE *m = fopen(meta_path, "r");
    if (m)
    {
        fscanf(m, "%d|%d|%127[^|]|%127[^\n]", &words, &chars, ts_created, owner);
        fclose(m);
    }

    char last_mod[128], last_acc[128];
    struct tm tm;

    localtime_r(&st.st_mtime, &tm);
    strftime(last_mod, sizeof(last_mod), "%Y-%m-%d %H:%M:%S", &tm);

    localtime_r(&st.st_atime, &tm);
    strftime(last_acc, sizeof(last_acc), "%Y-%m-%d %H:%M:%S", &tm);

    char acc_users[128][128];
    char acc_perms[128][8];

    int a = load_access(fname, acc_users, acc_perms, 128);

    char access_line[1024] = "";
    for (int j = 0; j < a; j++)
    {
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%s (%s)%s",
                 acc_users[j], acc_perms[j],
                 (j == a - 1 ? "" : ", "));
        strcat(access_line, tmp);
    }

    char out[1024];
    snprintf(out, sizeof(out),
             "--> File: %s\n"
             "--> Owner: %s\n"
             "--> Created: %s\n"
             "--> Last Modified: %s\n"
             "--> Size: %ld bytes\n"
             "--> Access: %s\n"
             "--> Last Accessed: %s by %s\n",
             fname, owner, ts_created, last_mod, st.st_size, access_line, last_acc, owner);

    sendall(cs, out, strlen(out));
}

void handle_delete(const char *fname_in, int cs)
{
    log_event("SS", "DELETE", fname_in ? fname_in : "NULL");

    if (!fname_in)
    {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    while (*fname_in == ' ')
        fname_in++;

    char fname[512];
    int i = 0;

    while (fname_in[i] && fname_in[i] != '\n' && i < 511)
        fname[i++] = fname_in[i];
    while (i > 0 && fname[i - 1] == ' ')
        i--;

    fname[i] = '\0';

    if (strlen(fname) == 0) {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    char path[512], meta_path[512], undo_path[512], undo_meta[512];
    snprintf(path, sizeof(path), "%s/%s", DATA_PATH, fname);
    snprintf(meta_path, sizeof(meta_path), "%s/%s.meta", DATA_PATH, fname);
    snprintf(undo_path, sizeof(undo_path), "%s/%s.undo", DATA_PATH, fname);
    snprintf(undo_meta, sizeof(undo_meta), "%s/%s.undo.meta", DATA_PATH, fname);

    if (access(path, F_OK) != 0)
    {
        log_event("SS", "ERROR", "DELETE: File not found");
        send_error(cs, E404_NOT_FOUND);
        return;
    }

    if (unlink(path) != 0) {
        log_event("SS", "ERROR", "DELETE: unlink failed");
        send_error(cs, E500_INTERNAL);
        return;
    }
    unlink(meta_path);
    unlink(undo_path);
    unlink(undo_meta);

    char msg[256];
    snprintf(msg, sizeof(msg), "File '%s' deleted successfully!\n", fname);
    sendall(cs, msg, strlen(msg));

    snprintf(msg, sizeof(msg), "Deleted %s", fname);
    log_event("SS", "DELETE_OK", msg);
}

// --- STREAM (updated) ---
void handle_stream(const char *fname_in, int cs)
{
    log_event("SS", "STREAM", fname_in ? fname_in : "NULL");

    if (!fname_in) {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    while (*fname_in == ' ')
        fname_in++;

    char fname[512];
    int i = 0;
    while (fname_in[i] && fname_in[i] != '\n' && i < 511)
        fname[i++] = fname_in[i];
    while (i > 0 && fname[i - 1] == ' ')
        i--;
    fname[i] = '\0';

    if (strlen(fname) == 0) {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", DATA_PATH, fname);

    FILE *f = fopen(path, "r");
    if (!f) {
        log_event("SS", "ERROR", "STREAM: File not found");
        send_error(cs, E404_NOT_FOUND);
        return;
    }

    char word[512];

    while (fscanf(f, "%511s", word) == 1)
    {
        strcat(word, "\n");

        if (sendall(cs, word, strlen(word)) < 0)
        {
            log_event("SS", "ERROR", "Client disconnected during STREAM");
            fclose(f);
            return;
        }

        usleep(100000); // 0.1 sec
    }

    fclose(f);
    log_event("SS", "STREAM_OK", fname);
}

/* ----------------- LIST ----------------- */
void handle_list(int cs)
{
    log_event("SS", "LIST", "List owners");

    DIR *d = opendir(DATA_PATH);
    if (!d)
    {
        log_event("SS", "ERROR", "LIST: Could not open data directory");
        send_error(cs, E500_INTERNAL);
        return;
    }

    char owners[128][128];
    int count = 0;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL)
    {
        if (entry->d_name[0] == '.')
            continue;

        if (!strstr(entry->d_name, ".meta"))
            continue;

        char meta_path[512];
        snprintf(meta_path, sizeof(meta_path), "%s/%s", DATA_PATH, entry->d_name);

        FILE *m = fopen(meta_path, "r");
        if (!m)
            continue;

        int words, chars;
        char timestamp[256], owner[256];

        if (fscanf(m, "%d|%d|%255[^|]|%255[^\n]", &words, &chars, timestamp, owner) == 4)
        {
            int exists = 0;

            for (int i = 0; i < count; i++)
            {
                if (strcmp(owners[i], owner) == 0)
                {
                    exists = 1;
                    break;
                }
            }

            if (!exists && count < 128)
            {
                strcpy(owners[count++], owner);
            }
        }

        fclose(m);
    }

    closedir(d);

    for (int i = 0; i < count; i++)
    {
        char line[256];
        snprintf(line, sizeof(line), "--> %s\n", owners[i]);
        sendall(cs, line, strlen(line));
    }

    log_event("SS", "LIST_OK", "Listed owners");
}

/* ----------------- ACCESS helpers ----------------- */
int load_access(const char *fname, char users[][128], char perms[][8], int max)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.access", DATA_PATH, fname);

    FILE *fp = fopen(path, "r");
    if (!fp)
    {
        log_event("SS", "INFO", "load_access: No access file");
        return 0;
    }

    int count = 0;
    while (count < max && fscanf(fp, "%127[^:]:%7s\n", users[count], perms[count]) == 2)
        count++;

    fclose(fp);
    return count;
}

void save_access(const char *fname, char users[][128], char perms[][8], int count)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.access", DATA_PATH, fname);

    FILE *fp = fopen(path, "w");
    if (!fp)
    {
        log_event("SS", "ERROR", "save_access failed");
        return;
    }

    for (int i = 0; i < count; i++)
        fprintf(fp, "%s:%s\n", users[i], perms[i]);

    fclose(fp);
}

/* ----------------- ADDACCESS / REMACCESS ----------------- */
void handle_addaccess(const char *cmd, int cs)
{
    log_event("SS", "ADDACCESS", cmd);

    char mode[4], fname[256], user[256];

    if (sscanf(cmd, "%3s %255s %255s", mode, fname, user) != 3)
    {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    int giveR = strcmp(mode, "-R") == 0;
    int giveW = strcmp(mode, "-W") == 0;

    if (!giveR && !giveW)
    {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    char users[128][128];
    char perms[128][8];
    int n = load_access(fname, users, perms, 128);

    int found = -1;
    for (int i = 0; i < n; i++)
    {
        if (strcmp(users[i], user) == 0)
        {
            found = i;
            break;
        }
    }

    if (found == -1)
    {
        strncpy(users[n], user, sizeof(users[n]) - 1);
        users[n][sizeof(users[n]) - 1] = '\0';
        strncpy(perms[n], giveR ? "R" : "W", sizeof(perms[n]) - 1);
        perms[n][sizeof(perms[n]) - 1] = '\0';
        n++;
    }
    else
    {
        if (giveR && !strchr(perms[found], 'R'))
            strncat(perms[found], "R", sizeof(perms[found]) - strlen(perms[found]) - 1);
        if (giveW && !strchr(perms[found], 'W'))
            strncat(perms[found], "W", sizeof(perms[found]) - strlen(perms[found]) - 1);
    }

    save_access(fname, users, perms, n);

    sendall(cs, "Access granted successfully!\n", strlen("Access granted successfully!\n"));
    log_event("SS", "ADDACCESS_OK", cmd);
}

void handle_remaccess(const char *cmd, int cs)
{
    log_event("SS", "REMACCESS", cmd);

    char fname[256], user[256];

    if (sscanf(cmd, "%255s %255s", fname, user) != 2)
    {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    char users[128][128];
    char perms[128][8];

    int n = load_access(fname, users, perms, 128);
    if (n == 0)
    {
        send_error(cs, E404_NOT_FOUND);
        return;
    }

    int found = -1;
    for (int i = 0; i < n; i++)
    {
        if (strcmp(users[i], user) == 0)
        {
            found = i;
            break;
        }
    }

    if (found == -1)
    {
        send_error(cs, E404_NOT_FOUND);
        return;
    }

    for (int i = found; i < n - 1; i++)
    {
        strcpy(users[i], users[i + 1]);
        strcpy(perms[i], perms[i + 1]);
    }

    n--;

    save_access(fname, users, perms, n);

    sendall(cs, "Access removed successfully!\n", strlen("Access removed successfully!\n"));

    log_event("SS", "REMACCESS_OK", cmd);
}

/* ----------------- EXEC ----------------- */
void handle_exec(const char *fname_in, int cs)
{
    log_event("SS", "EXEC", fname_in ? fname_in : "NULL");

    if (!fname_in) {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    while (*fname_in == ' ')
        fname_in++;

    char fname[512];
    int i = 0;

    while (fname_in[i] && fname_in[i] != '\n' && i < 511)
        fname[i++] = fname_in[i];

    while (i > 0 && fname[i - 1] == ' ')
        i--;

    fname[i] = '\0';

    if (strlen(fname) == 0) {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    /* Build file path */
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", DATA_PATH, fname);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        log_event("SS", "ERROR", "EXEC: File not found");
        send_error(cs, E404_NOT_FOUND);
        return;
    }

    char line[1024];

    while (fgets(line, sizeof(line), fp))
    {
        /* trim newline */
        line[strcspn(line, "\r\n")] = '\0';

        if (strlen(line) == 0)
            continue;

        /* ---- RUN LINE AS SHELL COMMAND ---- */
        FILE *pipe = popen(line, "r");
        if (!pipe) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Failed to run command: %s\n", line);
            sendall(cs, msg, strlen(msg));
            continue;
        }

        char out[1024];
        while (fgets(out, sizeof(out), pipe))
            sendall(cs, out, strlen(out));

        pclose(pipe);
    }

    fclose(fp);

    log_event("SS", "EXEC_OK", fname);
}

/* ----------------- UNDO handler ----------------- */
void handle_undo(const char *fname_in, int cs)
{
    log_event("SS", "UNDO", fname_in ? fname_in : "NULL");

    if (!fname_in)
    {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    const char *p = fname_in;
    while (*p == ' ')
        p++;

    if (*p == '\0')
    {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    char fname[512];
    strncpy(fname, p, sizeof(fname) - 1);
    fname[sizeof(fname) - 1] = '\0';

    while (strlen(fname) > 0 &&
           (fname[strlen(fname) - 1] == ' ' ||
            fname[strlen(fname) - 1] == '\r' ||
            fname[strlen(fname) - 1] == '\n'))
    {
        fname[strlen(fname) - 1] = '\0';
    }

    char undo_path[512];
    snprintf(undo_path, sizeof(undo_path), "%s/%s.undo", DATA_PATH, fname);

    if (access(undo_path, F_OK) != 0)
    {
        log_event("SS", "UNDO_FAIL", "Nothing to undo");
        send_error(cs, E404_NOT_FOUND);
        return;
    }

    if (perform_undo(fname))
    {
        log_event("SS", "UNDO_OK", fname);
        sendall(cs, "Undo Successful!\n", strlen("Undo Successful!\n"));
    }
    else
    {
        log_event("SS", "UNDO_FAIL", fname);
        send_error(cs, E500_INTERNAL);
    }
}
/* ----------------------------------------------------------
   FOLDER SUPPORT (CREATEFOLDER / MOVE / VIEWFOLDER)
---------------------------------------------------------- */

/* ----------------- CREATEFOLDER ----------------- */
void handle_createfolder(const char *folder_in, int cs)
{
    log_event("SS", "CREATEFOLDER", folder_in ? folder_in : "NULL");

    if (!folder_in)
    {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    while (*folder_in == ' ')
        folder_in++;

    char folder[512];
    int i = 0;

    while (folder_in[i] && folder_in[i] != '\n' && i < 511)
        folder[i++] = folder_in[i];
    while (i > 0 && folder[i - 1] == ' ')
        i--;
    folder[i] = '\0';

    if (strlen(folder) == 0)
    {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", DATA_PATH, folder);

    if (mkdir(path, 0755) != 0)
    {
        log_event("SS", "ERROR", "CREATEFOLDER failed");
        send_error(cs, E409_CONFLICT);
        return;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "Folder '%s' created successfully!\n", folder);
    sendall(cs, msg, strlen(msg));

    log_event("SS", "CREATEFOLDER_OK", folder);
}

/* ----------------- VIEWFOLDER ----------------- */
void handle_viewfolder(const char *folder_in, int cs)
{
    log_event("SS", "VIEWFOLDER", folder_in ? folder_in : "NULL");

    if (!folder_in)
    {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    while (*folder_in == ' ')
        folder_in++;

    char folder[512];
    int i = 0;

    while (folder_in[i] && folder_in[i] != '\n' && i < 511)
        folder[i++] = folder_in[i];
    while (i > 0 && folder[i - 1] == ' ')
        i--;
    folder[i] = '\0';

    if (strlen(folder) == 0)
    {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", DATA_PATH, folder);

    DIR *d = opendir(path);
    if (!d)
    {
        send_error(cs, E404_NOT_FOUND);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL)
    {
        if (entry->d_name[0] == '.')
            continue;

        char line[256];
        snprintf(line, sizeof(line), "--> %s\n", entry->d_name);
        sendall(cs, line, strlen(line));
    }

    closedir(d);
    log_event("SS", "VIEWFOLDER_OK", folder);
}

/* ----------------- MOVE <file> <folder> ----------------- */
void handle_move(const char *args, int cs)
{
    log_event("SS", "MOVE", args ? args : "NULL");

    char file[256], folder[256];

    if (sscanf(args, "%255s %255s", file, folder) != 2)
    {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    char src[512], dst[512];

    snprintf(src, sizeof(src), "%s/%s", DATA_PATH, file);
    snprintf(dst, sizeof(dst), "%s/%s/%s", DATA_PATH, folder, file);

    /* Check file exists */
    if (access(src, F_OK) != 0)
    {
        send_error(cs, E404_NOT_FOUND);
        return;
    }

    /* Check folder exists */
    char folder_path[512];
    snprintf(folder_path, sizeof(folder_path), "%s/%s", DATA_PATH, folder);

    struct stat st;
    if (stat(folder_path, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        send_error(cs, E404_NOT_FOUND);
        return;
    }

    /* Perform move */
    if (rename(src, dst) != 0)
    {
        send_error(cs, E500_INTERNAL);
        return;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "Moved '%s' → folder '%s'\n", file, folder);
    sendall(cs, msg, strlen(msg));

    log_event("SS", "MOVE_OK", args);
}

/* ----------------------------------------------------------
   CHECKPOINT SYSTEM
   Directory structure:
   DATA_PATH/checkpoints/<filename>/<tag>.chk
   DATA_PATH/checkpoints/<filename>/<tag>.meta
---------------------------------------------------------- */

static void ensure_checkpoint_dir(const char *filename) {
    char cp_root[512], cp_file_dir[512];

    snprintf(cp_root, sizeof(cp_root), "%s/checkpoints", DATA_PATH);
    mkdir(cp_root, 0755);

    snprintf(cp_file_dir, sizeof(cp_file_dir), "%s/checkpoints/%s", DATA_PATH, filename);
    mkdir(cp_file_dir, 0755);
}

/* ------------ CHECKPOINT <file> <tag> ------------ */
void handle_checkpoint(const char *args, int cs)
{
    log_event("SS", "CHECKPOINT", args ? args : "NULL");

    char fname[256], tag[256];

    if (sscanf(args, "%255s %255s", fname, tag) != 2) {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    char src_file[512], src_meta[512];
    snprintf(src_file, sizeof(src_file), "%s/%s", DATA_PATH, fname);
    snprintf(src_meta, sizeof(src_meta), "%s/%s.meta", DATA_PATH, fname);

    if (access(src_file, F_OK) != 0) {
        send_error(cs, E404_NOT_FOUND);
        return;
    }

    ensure_checkpoint_dir(fname);

    char dst_file[512], dst_meta[512];
    snprintf(dst_file, sizeof(dst_file), "%s/checkpoints/%s/%s.chk",
             DATA_PATH, fname, tag);
    snprintf(dst_meta, sizeof(dst_meta), "%s/checkpoints/%s/%s.meta",
             DATA_PATH, fname, tag);

    if (!copy_file_path(src_file, dst_file)) {
        send_error(cs, E500_INTERNAL);
        return;
    }

    if (access(src_meta, F_OK) == 0)
        copy_file_path(src_meta, dst_meta);

    char msg[256];
    snprintf(msg, sizeof(msg), "Checkpoint '%s' created for file '%s'\n", tag, fname);
    sendall(cs, msg, strlen(msg));

    log_event("SS", "CHECKPOINT_OK", msg);
}

/* ------------ VIEWCHECKPOINT <file> <tag> ------------ */
void handle_viewcheckpoint(const char *args, int cs)
{
    log_event("SS", "VIEWCHECKPOINT", args ? args : "NULL");

    char fname[256], tag[256];

    if (sscanf(args, "%255s %255s", fname, tag) != 2) {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    char chk_file[512];
    snprintf(chk_file, sizeof(chk_file),
             "%s/checkpoints/%s/%s.chk", DATA_PATH, fname, tag);

    FILE *f = fopen(chk_file, "r");
    if (!f) {
        send_error(cs, E404_NOT_FOUND);
        return;
    }

    char buf[1024];
    while (fgets(buf, sizeof(buf), f))
        sendall(cs, buf, strlen(buf));

    fclose(f);

    log_event("SS", "VIEWCHECKPOINT_OK", fname);
}

/* ------------ REVERT <file> <tag> ------------ */
void handle_revert(const char *args, int cs)
{
    log_event("SS", "REVERT", args ? args : "NULL");

    char fname[256], tag[256];

    if (sscanf(args, "%255s %255s", fname, tag) != 2) {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    char chk_file[512], chk_meta[512];
    snprintf(chk_file, sizeof(chk_file),
             "%s/checkpoints/%s/%s.chk", DATA_PATH, fname, tag);
    snprintf(chk_meta, sizeof(chk_meta),
             "%s/checkpoints/%s/%s.meta", DATA_PATH, fname, tag);

    if (access(chk_file, F_OK) != 0) {
        send_error(cs, E404_NOT_FOUND);
        return;
    }

    char dst_file[512], dst_meta[512];
    snprintf(dst_file, sizeof(dst_file), "%s/%s", DATA_PATH, fname);
    snprintf(dst_meta, sizeof(dst_meta), "%s/%s.meta", DATA_PATH, fname);

    if (!copy_file_path(chk_file, dst_file)) {
        send_error(cs, E500_INTERNAL);
        return;
    }

    if (access(chk_meta, F_OK) == 0)
        copy_file_path(chk_meta, dst_meta);

    char msg[256];
    snprintf(msg, sizeof(msg), "Reverted file '%s' to checkpoint '%s'\n", fname, tag);
    sendall(cs, msg, strlen(msg));

    log_event("SS", "REVERT_OK", msg);
}

/* ------------ LISTCHECKPOINTS <file> ------------ */
void handle_listcheckpoints(const char *args, int cs)
{
    log_event("SS", "LISTCHECKPOINTS", args ? args : "NULL");

    char fname[256];
    if (sscanf(args, "%255s", fname) != 1) {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    char dirpath[512];
    snprintf(dirpath, sizeof(dirpath), "%s/checkpoints/%s", DATA_PATH, fname);

    DIR *d = opendir(dirpath);
    if (!d) {
        send_error(cs, E404_NOT_FOUND);
        return;
    }

    struct dirent *entry;

    while ((entry = readdir(d)) != NULL)
    {
        if (entry->d_name[0] == '.')
            continue;

        if (!strstr(entry->d_name, ".chk"))
            continue;

        char tag[256];
        strncpy(tag, entry->d_name, sizeof(tag)-1);
        tag[sizeof(tag)-1] = '\0';

        tag[strlen(tag) - 4] = '\0'; // remove ".chk"

        char line[256];
        snprintf(line, sizeof(line), "--> %s\n", tag);
        sendall(cs, line, strlen(line));
    }

    closedir(d);

    log_event("SS", "LISTCHECKPOINTS_OK", fname);
}

