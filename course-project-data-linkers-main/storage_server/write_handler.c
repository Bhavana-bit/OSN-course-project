#include "server.h"
#include "logging.h"
#include "errors.h"

static int ends_with_sentence_delim(const char *s)
{
    if (!s) return 0;
    int L = strlen(s);
    for (int i=L-1; i>=0; i--)
    {
        if (s[i]==' ' || s[i]=='\t') continue;
        if (s[i]=='.' || s[i]=='!' || s[i]=='?')
            return 1;
        break;
    }
    return 0;
}

void handle_write(const char *cmd, int cs)
{
    char fname[256];
    int s_idx;

    if (sscanf(cmd, "%255s %d", fname, &s_idx) != 2)
    {
        send_error(cs, E400_INVALID_CMD);
        return;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", DATA_PATH, fname);

    if (access(path, F_OK) != 0)
    {
        send_error(cs, E404_NOT_FOUND);
        return;
    }

    char *original = read_whole_file(path);
    if (!original)
    {
        send_error(cs, E500_INTERNAL);
        return;
    }

    save_undo(fname);

    int scount_raw = 0;
    char **sents = split_sentences(original, &scount_raw);

    int len = strlen(original);
    int file_has_delim =
        (len > 0 &&
        (original[len-1]=='.' || original[len-1]=='!' || original[len-1]=='?'));

    int scount = scount_raw;
    int only_incomplete = 0;

    if (scount_raw == 0)
    {
        only_incomplete = 1;

        if (sents)
            free_sentences(sents, scount_raw);

        sents = malloc(sizeof(char*) * 2);
        sents[0] = strdup(original);
        sents[1] = NULL;
        scount = 1;
    }

    free(original);

    if (only_incomplete)
    {
        if (s_idx != 0)
        {
            send_error(cs, E400_INVALID_CMD);
            free_sentences(sents, scount);
            return;
        }
    }
    else
    {
        if (s_idx < 0 || s_idx > scount)
        {
            send_error(cs, E400_INVALID_CMD);
            free_sentences(sents, scount);
            return;
        }
    }

    sendall(cs, "WRITE MODE: Awaiting operations...\n", 35);

    int failed = 0;
    char buf[4096];

    while (1)
    {
        ssize_t n = read(cs, buf, sizeof(buf)-1);
        if (n <= 0)
            break;

        buf[n] = 0;
        buf[strcspn(buf,"\r\n")] = 0;

        if (strcmp(buf,"ETIRW")==0)
            break;

        int w_idx;
        char text[2048]={0};
        int args = sscanf(buf, "%d %[^\n]", &w_idx, text);

        if (args < 1)
        {
            send_error(cs, E400_INVALID_CMD);
            failed = 1;
            continue;
        }

        if (!only_incomplete && (s_idx < 0 || s_idx > scount))
        {
            send_error(cs, E400_INVALID_CMD);
            failed = 1;
            continue;
        }

        /* -----------------------------------------------
           1️⃣ APPEND NEW SENTENCE (WRITE a.txt <scount>)
           Only allowed if previous sentence ends with delimiter
        ------------------------------------------------ */
        if (s_idx == scount && file_has_delim)
        {
            if (scount > 0)
            {
                char *prev = sents[scount-1];
                if (!ends_with_sentence_delim(prev))
                {
                    send_error(cs, E400_INVALID_CMD);
                    failed = 1;
                    continue;
                }
            }

            if (!insert_sentence_at(&sents,&scount,s_idx,text))
            {
                send_error(cs,E500_INTERNAL);
                failed = 1;
            }
            continue;
        }

        int pc = 0;
        char **parts = split_sentences(text, &pc);

        if (pc == 0)
        {
            send_error(cs, E400_INVALID_CMD);
            failed = 1;
            continue;
        }

        if (!insert_word_into_sentence(sents, s_idx, w_idx, parts[0]))
        {
            send_error(cs, E400_INVALID_CMD);
            failed = 1;
            free_sentences(parts, pc);
            continue;
        }

        /* -----------------------------------------------
           2️⃣ INSERT EXTRA SENTENCES FROM PARTS
           Allowed *only when updated sentence ends with delim*
        ------------------------------------------------ */
        if (pc > 1 && file_has_delim)
        {
            if (!ends_with_sentence_delim(sents[s_idx]))
            {
                send_error(cs, E400_INVALID_CMD);
                failed = 1;
                free_sentences(parts, pc);
                continue;
            }

            int pos = s_idx + 1;
            for (int i=1; i<pc; i++)
                insert_sentence_at(&sents, &scount, pos++, parts[i]);
        }

        free_sentences(parts, pc);
    }

    if (failed)
    {
        send_error(cs, E500_INTERNAL);
        free_sentences(sents, scount);
        return;
    }

    char *joined = join_sentences(sents, scount);

    if (!write_whole_file(path, joined))
        send_error(cs, E500_INTERNAL);
    else
        sendall(cs, "Write Successful!\n", 18);

    free(joined);
    free_sentences(sents, scount);
}
