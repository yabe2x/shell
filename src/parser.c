/*
 * list      := pipeline ( (';' | '&' | '&&' | '||') pipeline )* [ ';' | '&' ]
 * pipeline  := command ( '|' command )*
 * command   := ( WORD | redirect WORD | '2>&1' )+
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"

static const char *token_name(TokenType t) {
    switch (t) {
    case T_PIPE: return "|";
    case T_AMP: return "&";
    case T_SEMI: return ";";
    case T_AND: return "&&";
    case T_OR: return "||";
    case T_IN: return "<";
    case T_OUT: return ">";
    case T_APPEND: return ">>";
    case T_ERR: return "2>";
    case T_ERR_APPEND: return "2>>";
    case T_ERR_TO_OUT: return "2>&1";
    case T_END: return "newline";
    default: return "word";
    }
}

static bool syntax_error(TokenType t) {
    fprintf(stderr, "ysh: syntax error near unexpected token `%s'\n", token_name(t));
    return false;
}

static bool is_redirect(TokenType t) {
    return t == T_IN || t == T_OUT || t == T_APPEND || t == T_ERR || t == T_ERR_APPEND || t == T_ERR_TO_OUT;
}

static void add_arg(Command *cmd, const char *arg) {
    cmd->argv = realloc(cmd->argv, (cmd->argc + 2) * sizeof(char *));
    cmd->argv[cmd->argc++] = strdup(arg);
    cmd->argv[cmd->argc] = NULL;
}

static void add_redir(Command *cmd, RedirType type, const char *file) {
    cmd->redirs = realloc(cmd->redirs, (cmd->nredirs + 1) * sizeof(Redir));
    cmd->redirs[cmd->nredirs].type = type;
    cmd->redirs[cmd->nredirs].file = file ? strdup(file) : NULL;
    cmd->nredirs++;
}

static void free_command(Command *cmd) {
    for (int i = 0; i < cmd->argc; i++) free(cmd->argv[i]);
    free(cmd->argv);
    for (int i = 0; i < cmd->nredirs; i++) free(cmd->redirs[i].file);
    free(cmd->redirs);
}

static void free_pipeline(Pipeline *pl) {
    for (int i = 0; i < pl->ncmds; i++) free_command(&pl->cmds[i]);
    free(pl->cmds);
    free(pl->text);
}

static bool parse_command(const TokenList *tl, int *t, Command *cmd) {
    memset(cmd, 0, sizeof(*cmd));
    bool any = false;

    for (;;) {
        const Token *tok = &tl->tokens[*t];
        if (tok->type == T_WORD) {
            for (int k = 0; k < tok->nwords; k++) add_arg(cmd, tok->words[k]);
            any = true;
            (*t)++;
        } else if (tok->type == T_ERR_TO_OUT) {
            add_redir(cmd, R_ERR_TO_OUT, NULL);
            any = true;
            (*t)++;
        } else if (is_redirect(tok->type)) {
            const Token *target = &tl->tokens[*t + 1];
            if (target->type != T_WORD) {
                free_command(cmd);
                return syntax_error(target->type);
            }
            if (target->nwords != 1) {
                fprintf(stderr, "ysh: ambiguous redirect\n");
                free_command(cmd);
                return false;
            }
            RedirType rt = tok->type == T_IN       ? R_IN
                           : tok->type == T_OUT    ? R_OUT
                           : tok->type == T_APPEND ? R_APPEND
                           : tok->type == T_ERR    ? R_ERR
                                                   : R_ERR_APPEND;
            add_redir(cmd, rt, target->words[0]);
            any = true;
            *t += 2;
        } else {
            break;
        }
    }
    if (!any) {
        free_command(cmd);
        return syntax_error(tl->tokens[*t].type);
    }
    return true;
}

bool parse(const TokenList *tl, const char *line, CommandList *out) {
    memset(out, 0, sizeof(*out));
    int t = 0;
    Connector connector = RUN_ALWAYS;

    while (tl->tokens[t].type != T_END) {
        Pipeline pl = {0};
        size_t text_start = tl->tokens[t].start;

        for (;;) {
            Command cmd;
            if (!parse_command(tl, &t, &cmd)) {
                free_pipeline(&pl);
                free_command_list(out);
                return false;
            }
            pl.cmds = realloc(pl.cmds, (pl.ncmds + 1) * sizeof(Command));
            pl.cmds[pl.ncmds++] = cmd;
            if (tl->tokens[t].type != T_PIPE) break;
            t++;
        }
        size_t text_end = tl->tokens[t - 1].end;
        pl.text = strndup(line + text_start, text_end - text_start);

        Connector next = RUN_ALWAYS;
        TokenType sep = tl->tokens[t].type;
        if (sep == T_AMP) pl.background = true;
        else if (sep == T_AND) next = RUN_IF_OK;
        else if (sep == T_OR) next = RUN_IF_FAILED;
        else if (sep != T_SEMI && sep != T_END) {
            free_pipeline(&pl);
            free_command_list(out);
            return syntax_error(sep);
        }
        if (sep != T_END) t++;
        if ((sep == T_AND || sep == T_OR) && tl->tokens[t].type == T_END) {
            free_pipeline(&pl);
            free_command_list(out);
            return syntax_error(T_END);
        }

        out->pipelines = realloc(out->pipelines, (out->count + 1) * sizeof(Pipeline));
        out->connectors = realloc(out->connectors, (out->count + 1) * sizeof(Connector));
        out->pipelines[out->count] = pl;
        out->connectors[out->count] = connector;
        out->count++;
        connector = next;
    }
    return true;
}

void free_command_list(CommandList *list) {
    for (int i = 0; i < list->count; i++) free_pipeline(&list->pipelines[i]);
    free(list->pipelines);
    free(list->connectors);
    memset(list, 0, sizeof(*list));
}
