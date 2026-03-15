#include "logging.h"

static FILE *log_fp = NULL;

void log_init(const char *filename)
{
    if (log_fp) return;
    log_fp = fopen(filename, "a");
    if (!log_fp)
    {
        perror("log_init");
        exit(1);
    }
}

void log_close()
{
    if (log_fp)
    {
        fclose(log_fp);
        log_fp = NULL;
    }
}

char *timestamp()
{
    static char buf[64];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

void log_event(const char *component,
               const char *event_type,
               const char *detail)
{
    if (!log_fp)
        return;

    fprintf(log_fp,
            "[%s] [%s] [%s] %s\n",
            timestamp(),
            component,
            event_type,
            detail);
    fflush(log_fp);

    /* Also echo to terminal for immediate visibility (NM should be chatty) */
    printf("[%s] [%s] %s: %s\n",
           timestamp(),
           component,
           event_type,
           detail);
}
