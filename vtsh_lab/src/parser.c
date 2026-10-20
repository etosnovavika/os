#define _POSIX_C_SOURCE 200809L
#include "parser.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Освобождает память аргументов и путей файлов одной команды.
void command_free(Command *cmd) {
    for (size_t i = 0; i < cmd->argc; ++i) free(cmd->argv[i]);
    free(cmd->argv);
    free(cmd->input_path);
    free(cmd->output_path);
    cmd->argv = NULL;
    cmd->argc = 0;
}

// Освобождает все команды конвейера и сам массив команд.
void pipeline_free(Pipeline *pipeline) {
    for (size_t i = 0; i < pipeline->count; ++i)
        command_free(&pipeline->commands[i]);

    free(pipeline->commands);
    pipeline->commands = NULL;
    pipeline->count = 0;
}

// Проверяет операторы, которые не должны обрабатываться внутри конвейера.
static int reserved(const char *s) {
    return !strcmp(s, "&") || !strcmp(s, ";") ||
           !strcmp(s, "&&") || !strcmp(s, "||");
}

// Увеличивает массив команд и добавляет новую пустую Command.
static int add_command(Pipeline *p) {
    if (p->count == SIZE_MAX / sizeof(Command)) return -1;

    Command *next = realloc(p->commands, (p->count + 1) * sizeof(Command));
    if (!next) return -1;

    p->commands = next;
    p->commands[p->count++] = (Command){0};
    return 0;
}

// Добавляет аргумент в argv + NULL
static int add_word(Command *cmd, char *word) {
    if (cmd->argc > (SIZE_MAX / sizeof(char *)) - 2) return -1;

    char **next = realloc(cmd->argv, (cmd->argc + 2) * sizeof(char *));
    if (!next) return -1;

    cmd->argv = next;
    cmd->argv[cmd->argc++] = word;
    cmd->argv[cmd->argc] = NULL;
    return 0;
}

// Разбирает конвейер: команды, аргументы, |, <, > и >>.
int parse_pipeline(const char *line, Pipeline *pipeline) {
    const char *p = line;
    int expect_redir = 0; // 0 - нет, 1 - <, 2 - >, 3 - >>

    while (*p) {
        // Выделяем очередное слово, разделяя строку по пробелам.
        while (isspace((unsigned char)*p)) ++p;
        if (!*p) break;

        const char *begin = p;
        while (*p && !isspace((unsigned char)*p)) ++p;

        char *word = strndup(begin, (size_t)(p - begin));
        if (!word) return -1;

        // После <, > или >> сохраняем следующее слово как имя файла.
        if (expect_redir) {
            if (reserved(word) || !strcmp(word, "|") ||
                !strcmp(word, "<") || !strcmp(word, ">") ||
                !strcmp(word, ">>")) {
                fprintf(stderr, "vtsh: syntax error: expected filename after redirection\n");
                free(word);
                return 1;
            }

            Command *cmd = &pipeline->commands[pipeline->count - 1];
            char **target = expect_redir == 1
                ? &cmd->input_path
                : &cmd->output_path;

            free(*target);
            *target = word;

            if (expect_redir != 1)
                cmd->append_output = expect_redir == 3;

            expect_redir = 0;
            continue;
        }

        // Отклоняем неподдерживаемые операторы.
        if (reserved(word)) {
            fprintf(stderr, "vtsh: unsupported operator: %s\n", word);
            free(word);
            return 1;
        }

        // Оператор | завершает текущую команду и создаёт следующую.
        if (!strcmp(word, "|")) {
            free(word);

            if (!pipeline->count ||
                !pipeline->commands[pipeline->count - 1].argc) {
                fprintf(stderr, "vtsh: syntax error near |\n");
                return 1;
            }

            if (add_command(pipeline) < 0) return -1;
            continue;
        }

        // Запоминаем тип перенаправления и ожидаем имя файла.
        if (!strcmp(word, "<") || !strcmp(word, ">") ||
            !strcmp(word, ">>")) {

            if (!pipeline->count && add_command(pipeline) < 0) {
                free(word);
                return -1;
            }

            expect_redir = !strcmp(word, "<")
                ? 1
                : (!strcmp(word, ">") ? 2 : 3);

            free(word);
            continue;
        }

        // Обычное слово добавляем в argv последней команды.
        if (!pipeline->count && add_command(pipeline) < 0) {
            free(word);
            return -1;
        }

        if (add_word(&pipeline->commands[pipeline->count - 1], word) < 0) {
            free(word);
            return -1;
        }
    }

    // Проверяем, что нет незавершённого перенаправления или пустых команд.
    if (expect_redir) {
        fprintf(stderr, "vtsh: syntax error: missing redirection filename\n");
        return 1;
    }

    for (size_t i = 0; i < pipeline->count; ++i) {
        if (!pipeline->commands[i].argc) {
            fprintf(stderr, "vtsh: syntax error: missing command\n");
            return 1;
        }
    }

    return 0;
}

// Освобождает весь план выполнения вместе с его конвейерами.
void plan_free(ExecutionPlan *plan) {
    for (size_t i = 0; i < plan->count; ++i)
        pipeline_free(&plan->pipelines[i]);

    free(plan->pipelines);
    plan->pipelines = NULL;
    plan->count = 0;
}

// Создаёт новый конвейер в плане и разбирает его текст.
static int plan_add(ExecutionPlan *plan, const char *text) {
    if (plan->count >= SIZE_MAX / sizeof(Pipeline)) return -1;

    Pipeline *new_items = realloc(
        plan->pipelines,
        (plan->count + 1) * sizeof(Pipeline)
    );
    if (!new_items) return -1;

    plan->pipelines = new_items;
    Pipeline *next = &plan->pipelines[plan->count];
    *next = (Pipeline){0};
    ++plan->count;

    return parse_pipeline(text, next);
}

// Разделяет всю строку по оператору || на отдельные конвейеры.
int parse_plan(const char *line, ExecutionPlan *plan) {
    const char *start = line;
    const char *p = line;

    while (1) {
        while (*p && isspace((unsigned char)*p)) ++p;
        if (!*p) break;

        const char *token = p;
        while (*p && !isspace((unsigned char)*p)) ++p;

        // Нашли ||: сохраняем участок до него как отдельный конвейер.
        if ((size_t)(p - token) == 2 &&
            token[0] == '|' && token[1] == '|') {

            char *segment = strndup(start, (size_t)(token - start));
            if (!segment) return -1;

            int result = plan_add(plan, segment);
            free(segment);
            if (result) return result;

            if (!plan->pipelines[plan->count - 1].count) {
                fprintf(stderr, "vtsh: syntax error near ||\n");
                return 1;
            }

            start = p;
        }
    }

    // Добавляем последний участок после последнего ||.
    char *segment = strdup(start);
    if (!segment) return -1;

    int result = plan_add(plan, segment);
    free(segment);
    if (result) return result;

    // Проверяем, что после || не осталась пустая команда.
    if (!plan->pipelines[plan->count - 1].count &&
        plan->count > 1) {
        fprintf(stderr, "vtsh: syntax error: missing command after ||\n");
        return 1;
    }

    return 0;
}