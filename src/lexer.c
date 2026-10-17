/* Quote removal, escapes, $ and ~ expansion, and globbing all happen here. */
#define _GNU_SOURCE
#include <ctype.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"

typedef struct {
    char *data;
    size_t len, cap;
} Buf;

static void buf_push(Buf *b, char c) {
    if (b->len + 2 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 32;
        b->data = realloc(b->data, b->cap);
    }
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

static void buf_free(Buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

/* pattern is text with quoted glob characters escaped, so only unquoted ones glob */
typedef struct {
    Buf text, pattern;
    bool has_glob;
    bool quoted;  /* "" is still a word */
} Word;

static void add_literal(Word *w, char c) {
    buf_push(&w->text, c);
    if (strchr("*?[]\\", c)) buf_push(&w->pattern, '\\');
    buf_push(&w->pattern, c);
}

static void add_literal_str(Word *w, const char *s) {
    for (; *s; s++) add_literal(w, *s);
}

static bool is_operator_char(char c) { return c == '|' || c == '&' || c == ';' || c == '<' || c == '>'; }
static bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static void expand_dollar(const char *line, size_t *i, const ExpandContext *ctx, Word *w) {
    char num[32];
    char c = line[*i + 1];

    if (c == '?') {
        snprintf(num, sizeof(num), "%d", ctx->last_status);
        add_literal_str(w, num);
        *i += 2;
    } else if (c == '$') {
        snprintf(num, sizeof(num), "%d", (int)ctx->shell_pid);
        add_literal_str(w, num);
        *i += 2;
    } else if (c == '!') {
        if (ctx->last_bg_pid) {
            snprintf(num, sizeof(num), "%d", (int)ctx->last_bg_pid);
            add_literal_str(w, num);
        }
        *i += 2;
    } else if (isdigit((unsigned char)c)) {
        /* no positional args, so $1..$9 are empty */
        if (c == '0') add_literal_str(w, "ysh");
        *i += 2;
    } else if (c == '{' || isalpha((unsigned char)c) || c == '_') {
        bool braces = c == '{';
        size_t start = *i + 1 + braces, end = start;
        while (isalnum((unsigned char)line[end]) || line[end] == '_') end++;
        if (braces && line[end] != '}') {
            add_literal(w, '$');
            *i += 1;
            return;
        }
        char *name = strndup(line + start, end - start);
        const char *value = getenv(name);
        if (value) add_literal_str(w, value);
        free(name);
        *i = end + braces;
    } else {
        add_literal(w, '$');
        *i += 1;
    }
}

static bool lex_word(const char *line, size_t *i, const ExpandContext *ctx, Token *tok) {
    Word w = {0};
    size_t start = *i;

    while (line[*i] && !is_blank(line[*i]) && !is_operator_char(line[*i])) {
        char c = line[*i];

        if (c == '\\') {
            if (line[*i + 1]) add_literal(&w, line[*i + 1]), *i += 2;
            else *i += 1;
        } else if (c == '\'') {
            w.quoted = true;
            size_t j = *i + 1;
            while (line[j] && line[j] != '\'') add_literal(&w, line[j++]);
            if (!line[j]) {
                fprintf(stderr, "ysh: unexpected end of line while looking for matching `''\n");
                buf_free(&w.text), buf_free(&w.pattern);
                return false;
            }
            *i = j + 1;
        } else if (c == '"') {
            w.quoted = true;
            size_t j = *i + 1;
            while (line[j] && line[j] != '"') {
                if (line[j] == '\\' && line[j + 1] && strchr("$\"\\`", line[j + 1])) {
                    add_literal(&w, line[j + 1]);
                    j += 2;
                } else if (line[j] == '$') {
                    expand_dollar(line, &j, ctx, &w);
                } else {
                    add_literal(&w, line[j++]);
                }
            }
            if (!line[j]) {
                fprintf(stderr, "ysh: unexpected end of line while looking for matching `\"'\n");
                buf_free(&w.text), buf_free(&w.pattern);
                return false;
            }
            *i = j + 1;
        } else if (c == '$') {
            expand_dollar(line, i, ctx, &w);
        } else if (c == '~' && *i == start && (line[*i + 1] == '/' || line[*i + 1] == '\0' ||
                                                is_blank(line[*i + 1]) || is_operator_char(line[*i + 1]))) {
            const char *home = getenv("HOME");
            add_literal_str(&w, home ? home : "~");
            *i += 1;
        } else if (c == '*' || c == '?' || c == '[') {
            buf_push(&w.text, c);
            buf_push(&w.pattern, c);
            w.has_glob = true;
            *i += 1;
        } else if (c == ']') {
            buf_push(&w.text, c);
            buf_push(&w.pattern, c);
            *i += 1;
        } else {
            add_literal(&w, c);
            *i += 1;
        }
    }

    tok->type = T_WORD;
    tok->words = NULL;
    tok->nwords = 0;

    if (w.has_glob) {
        glob_t g;
        if (glob(w.pattern.data, 0, NULL, &g) == 0 && g.gl_pathc > 0) {
            tok->words = malloc(g.gl_pathc * sizeof(char *));
            for (size_t k = 0; k < g.gl_pathc; k++) tok->words[k] = strdup(g.gl_pathv[k]);
            tok->nwords = (int)g.gl_pathc;
            globfree(&g);
            buf_free(&w.text), buf_free(&w.pattern);
            return true;
        }
        globfree(&g);
    }

    /* an unquoted word that expands to nothing disappears */
    if (w.text.len > 0 || w.quoted) {
        tok->words = malloc(sizeof(char *));
        tok->words[0] = w.text.data ? strdup(w.text.data) : strdup("");
        tok->nwords = 1;
    }
    buf_free(&w.text), buf_free(&w.pattern);
    return true;
}

static void push_token(TokenList *list, int *cap, Token tok) {
    if (list->count == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        list->tokens = realloc(list->tokens, *cap * sizeof(Token));
    }
    list->tokens[list->count++] = tok;
}

bool tokenize(const char *line, const ExpandContext *ctx, TokenList *out) {
    out->tokens = NULL;
    out->count = 0;
    int cap = 0;
    size_t i = 0;

    for (;;) {
        while (is_blank(line[i])) i++;
        Token tok = {T_END, NULL, 0, i, i};

        if (line[i] == '\0' || line[i] == '#') {
            push_token(out, &cap, tok);
            return true;
        }

        if (line[i] == '2' && line[i + 1] == '>') {
            if (line[i + 2] == '>') tok.type = T_ERR_APPEND, i += 3;
            else if (line[i + 2] == '&' && line[i + 3] == '1') tok.type = T_ERR_TO_OUT, i += 4;
            else tok.type = T_ERR, i += 2;
        } else if (line[i] == '|') {
            if (line[i + 1] == '|') tok.type = T_OR, i += 2;
            else tok.type = T_PIPE, i += 1;
        } else if (line[i] == '&') {
            if (line[i + 1] == '&') tok.type = T_AND, i += 2;
            else tok.type = T_AMP, i += 1;
        } else if (line[i] == ';') {
            tok.type = T_SEMI, i += 1;
        } else if (line[i] == '<') {
            tok.type = T_IN, i += 1;
        } else if (line[i] == '>') {
            if (line[i + 1] == '>') tok.type = T_APPEND, i += 2;
            else tok.type = T_OUT, i += 1;
        } else if (!lex_word(line, &i, ctx, &tok)) {
            free_tokens(out);
            return false;
        }
        tok.end = i;
        push_token(out, &cap, tok);
    }
}

void free_tokens(TokenList *list) {
    for (int t = 0; t < list->count; t++) {
        for (int k = 0; k < list->tokens[t].nwords; k++) free(list->tokens[t].words[k]);
        free(list->tokens[t].words);
    }
    free(list->tokens);
    list->tokens = NULL;
    list->count = 0;
}
