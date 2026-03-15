#include "server.h"
#include "logging.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ----------------------------------------------------------
   split_sentences()
   - robust: checks allocations, trims spaces, returns NULL on alloc failure
   - out_count set to number of returned sentences (0 permitted)
---------------------------------------------------------- */
char **split_sentences(const char *content, int *out_count)
{
    log_event("SS", "FUNC", "split_sentences() called");

    if (!out_count) return NULL;
    *out_count = 0;

    if (!content)
    {
        log_event("SS", "WARN", "split_sentences(): content NULL");
        return NULL;
    }

    size_t len = strlen(content);
    char msg[128];
    snprintf(msg, sizeof(msg), "Splitting content of length %zu", len);
    log_event("SS", "INFO", msg);

    /* copy into writable buffer */
    char *buf = malloc(len + 1);
    if (!buf)
    {
        log_event("SS", "ERROR", "split_sentences(): malloc failed");
        return NULL;
    }
    memcpy(buf, content, len + 1);

    char **arr = NULL;
    int count = 0;
    size_t i = 0, start = 0;

    while (i < len)
    {
        if (buf[i] == '.' || buf[i] == '?' || buf[i] == '!')
        {
            size_t end = i;
            /* include following spaces as part of the chunk end position */
            while (end + 1 < len && buf[end + 1] == ' ')
                end++;

            size_t slen = end - start + 1;
            char *s = malloc(slen + 1);
            if (!s)
            {
                log_event("SS", "ERROR", "split_sentences(): malloc sentence failed");
                free(buf);
                free_sentences(arr, count);
                return NULL;
            }
            memcpy(s, buf + start, slen);
            s[slen] = '\0';

            /* trim leading/trailing spaces/tabs */
            int a = 0, b = (int)strlen(s) - 1;
            while (a <= b && (s[a] == ' ' || s[a] == '\t')) a++;
            while (b >= a && (s[b] == ' ' || s[b] == '\t')) s[b--] = '\0';
            if (a > 0) memmove(s, s + a, strlen(s + a) + 1);

            char **tmp = realloc(arr, sizeof(char *) * (count + 2));
            if (!tmp)
            {
                log_event("SS", "ERROR", "split_sentences(): realloc failed");
                free(s);
                free(buf);
                free_sentences(arr, count);
                return NULL;
            }
            arr = tmp;
            arr[count++] = s;
            arr[count] = NULL;

            start = end + 1;
            while (start < len && buf[start] == ' ')
                start++;
            i = start;
            continue;
        }
        i++;
    }

    /* trailing text (incomplete sentence) - keep as separate chunk */
    if (start < len)
    {
        while (start < len && buf[start] == ' ')
            start++;

        if (start < len)
        {
            size_t slen = len - start;
            char *s = malloc(slen + 1);
            if (!s)
            {
                log_event("SS", "ERROR", "split_sentences(): malloc trailing failed");
                free(buf);
                free_sentences(arr, count);
                return NULL;
            }
            memcpy(s, buf + start, slen);
            s[slen] = '\0';

            char **tmp2 = realloc(arr, sizeof(char *) * (count + 2));
            if (!tmp2)
            {
                log_event("SS", "ERROR", "split_sentences(): realloc failed (trail)");
                free(s);
                free(buf);
                free_sentences(arr, count);
                return NULL;
            }
            arr = tmp2;
            arr[count++] = s;
            arr[count] = NULL;
        }
    }

    free(buf);

    *out_count = count;
    snprintf(msg, sizeof(msg), "split_sentences(): produced %d sentences", count);
    log_event("SS", "INFO", msg);

    return arr;
}

/* ----------------------------------------------------------
   join_sentences()
   - builds a single string joining sentences with single spaces
   - returns strdup("") if no sentences
---------------------------------------------------------- */
char *join_sentences(char **sents, int count)
{
    if (!sents || count <= 0)
        return strdup("");

    size_t total = 0;
    for (int i = 0; i < count; i++)
        total += strlen(sents[i]) + 1; /* space or terminator */

    char *out = malloc(total + 1);
    if (!out)
    {
        log_event("SS", "ERROR", "join_sentences(): malloc failed");
        return NULL;
    }
    out[0] = '\0';

    size_t pos = 0;
    for (int i = 0; i < count; i++)
    {
        size_t sl = strlen(sents[i]);
        memcpy(out + pos, sents[i], sl);
        pos += sl;
        if (i != count - 1)
            out[pos++] = ' ';
    }
    out[pos] = '\0';
    return out;
}

/* ----------------------------------------------------------
   free_sentences()
---------------------------------------------------------- */
void free_sentences(char **s, int count)
{
    if (!s) return;
    for (int i = 0; i < count; i++)
        if (s[i]) free(s[i]);
    free(s);
}

/* ----------------------------------------------------------
   insert_sentence_at()
   - safe realloc/strdup checks
   - returns 1 on success, 0 on failure
---------------------------------------------------------- */
int insert_sentence_at(char ***sents_p, int *count_p, int idx, const char *text)
{
    if (!sents_p || !count_p) return 0;
    if (idx < 0 || idx > *count_p) return 0;
    if (!text || strlen(text) == 0) return 0;

    char **sents = *sents_p;
    char *news = strdup(text);
    if (!news) return 0;

    char **tmp = realloc(sents, sizeof(char *) * (*count_p + 2));
    if (!tmp)
    {
        free(news);
        return 0;
    }
    sents = tmp;

    for (int i = *count_p; i > idx; --i)
        sents[i] = sents[i - 1];

    sents[idx] = news;
    *count_p = *count_p + 1;
    sents[*count_p] = NULL;

    *sents_p = sents;
    return 1;
}

/* ----------------------------------------------------------
   insert_word_into_sentence()
   - robust tokenization and memory handling
   - supports OSN special rule: if sentence empty and word_idx==1 => treat as 0
   - returns 1 on success, 0 on failure
---------------------------------------------------------- */
int insert_word_into_sentence(char **sents, int s_idx, int word_idx, const char *text)
{
    if (!sents || s_idx < 0) return 0;
    char *sent = sents[s_idx];
    if (!sent) return 0;

    /* copy sentence to working buffer; preserve original sents content */
    char *working = strdup(sent);
    if (!working) return 0;

    /* detect and temporarily remove trailing punctuation */
    char punct = 0;
    int wlen = (int)strlen(working);
    if (wlen > 0 && (working[wlen - 1] == '.' || working[wlen - 1] == '!' || working[wlen - 1] == '?'))
    {
        punct = working[wlen - 1];
        working[wlen - 1] = '\0';
    }

    /* tokenize old words */
    char **old_words = NULL;
    int owc = 0;
    char *tok = strtok(working, " ");
    while (tok)
    {
        char *ow = strdup(tok);
        if (!ow)
        {
            /* cleanup */
            for (int i = 0; i < owc; i++) free(old_words[i]);
            free(old_words);
            free(working);
            return 0;
        }
        char **tmpow = realloc(old_words, sizeof(char *) * (owc + 1));
        if (!tmpow)
        {
            free(ow);
            for (int i = 0; i < owc; i++) free(old_words[i]);
            free(old_words);
            free(working);
            return 0;
        }
        old_words = tmpow;
        old_words[owc++] = ow;
        tok = strtok(NULL, " ");
    }

    /* OSN special rule: allow word_idx==1 on empty sentence as index 0 */
    if (owc == 0 && word_idx == 1)
        word_idx = 0;

    /* validate index (0..owc allowed) */
    if (word_idx < 0 || word_idx > owc)
    {
        for (int i = 0; i < owc; i++) free(old_words[i]);
        free(old_words);
        free(working);
        return 0;
    }

    /* tokenize new words (text may be NULL or empty) */
    char **new_words = NULL;
    int nwc = 0;
    if (text && text[0] != '\0')
    {
        char *tcopy = strdup(text);
        if (!tcopy)
        {
            for (int i = 0; i < owc; i++) free(old_words[i]);
            free(old_words);
            free(working);
            return 0;
        }

        char *nt = strtok(tcopy, " ");
        while (nt)
        {
            char *nw = strdup(nt);
            if (!nw)
            {
                free(tcopy);
                for (int i = 0; i < owc; i++) free(old_words[i]);
                free(old_words);
                for (int i = 0; i < nwc; i++) free(new_words[i]);
                free(new_words);
                free(working);
                return 0;
            }
            char **tmpnw = realloc(new_words, sizeof(char *) * (nwc + 1));
            if (!tmpnw)
            {
                free(nw);
                free(tcopy);
                for (int i = 0; i < owc; i++) free(old_words[i]);
                free(old_words);
                for (int i = 0; i < nwc; i++) free(new_words[i]);
                free(new_words);
                free(working);
                return 0;
            }
            new_words = tmpnw;
            new_words[nwc++] = nw;
            nt = strtok(NULL, " ");
        }
        free(tcopy);
    }

    /* build final words array: words before insertion, new words, remaining old words */
    char **final_words = NULL;
    int fwc = 0;

    for (int i = 0; i < word_idx; i++)
    {
        char *fw = strdup(old_words[i]);
        if (!fw)
        {
            for (int j = 0; j < owc; j++) free(old_words[j]);
            free(old_words);
            for (int j = 0; j < nwc; j++) free(new_words[j]);
            free(new_words);
            for (int j = 0; j < fwc; j++) free(final_words[j]);
            free(final_words);
            free(working);
            return 0;
        }
        char **tmpf = realloc(final_words, sizeof(char *) * (fwc + 1));
        if (!tmpf)
        {
            free(fw);
            for (int j = 0; j < owc; j++) free(old_words[j]);
            free(old_words);
            for (int j = 0; j < nwc; j++) free(new_words[j]);
            free(new_words);
            for (int j = 0; j < fwc; j++) free(final_words[j]);
            free(final_words);
            free(working);
            return 0;
        }
        final_words = tmpf;
        final_words[fwc++] = fw;
    }

    for (int i = 0; i < nwc; i++)
    {
        char *fw = strdup(new_words[i]);
        if (!fw)
        {
            for (int j = 0; j < owc; j++) free(old_words[j]);
            free(old_words);
            for (int j = 0; j < nwc; j++) free(new_words[j]);
            free(new_words);
            for (int j = 0; j < fwc; j++) free(final_words[j]);
            free(final_words);
            free(working);
            return 0;
        }
        char **tmpf = realloc(final_words, sizeof(char *) * (fwc + 1));
        if (!tmpf)
        {
            free(fw);
            for (int j = 0; j < owc; j++) free(old_words[j]);
            free(old_words);
            for (int j = 0; j < nwc; j++) free(new_words[j]);
            free(new_words);
            for (int j = 0; j < fwc; j++) free(final_words[j]);
            free(final_words);
            free(working);
            return 0;
        }
        final_words = tmpf;
        final_words[fwc++] = fw;
    }

    for (int i = word_idx; i < owc; i++)
    {
        char *fw = strdup(old_words[i]);
        if (!fw)
        {
            for (int j = 0; j < owc; j++) free(old_words[j]);
            free(old_words);
            for (int j = 0; j < nwc; j++) free(new_words[j]);
            free(new_words);
            for (int j = 0; j < fwc; j++) free(final_words[j]);
            free(final_words);
            free(working);
            return 0;
        }
        char **tmpf = realloc(final_words, sizeof(char *) * (fwc + 1));
        if (!tmpf)
        {
            free(fw);
            for (int j = 0; j < owc; j++) free(old_words[j]);
            free(old_words);
            for (int j = 0; j < nwc; j++) free(new_words[j]);
            free(new_words);
            for (int j = 0; j < fwc; j++) free(final_words[j]);
            free(final_words);
            free(working);
            return 0;
        }
        final_words = tmpf;
        final_words[fwc++] = fw;
    }

    /* join final words into new sentence buffer */
    size_t needed = 1; /* null */
    for (int i = 0; i < fwc; i++)
        needed += strlen(final_words[i]) + 1; /* word + space */

    char *new_sent = malloc(needed + (punct ? 1 : 0));
    if (!new_sent)
    {
        for (int j = 0; j < owc; j++) free(old_words[j]);
        free(old_words);
        for (int j = 0; j < nwc; j++) free(new_words[j]);
        free(new_words);
        for (int j = 0; j < fwc; j++) free(final_words[j]);
        free(final_words);
        free(working);
        return 0;
    }

    new_sent[0] = '\0';
    for (int i = 0; i < fwc; i++)
    {
        if (i) strcat(new_sent, " ");
        strcat(new_sent, final_words[i]);
    }

    if (punct)
    {
        size_t cur = strlen(new_sent);
        new_sent[cur] = punct;
        new_sent[cur + 1] = '\0';
    }

    /* replace sentence in sents array */
    free(sents[s_idx]);
    sents[s_idx] = new_sent;

    /* cleanup */
    for (int i = 0; i < owc; i++) free(old_words[i]);
    free(old_words);
    for (int i = 0; i < nwc; i++) free(new_words[i]);
    free(new_words);
    for (int i = 0; i < fwc; i++) free(final_words[i]);
    free(final_words);
    free(working);

    log_event("SS", "INFO", "Word inserted successfully");
    return 1;
}
