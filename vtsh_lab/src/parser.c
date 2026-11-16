#define _POSIX_C_SOURCE 200809L
#include "parser.h"
#include "combinators.h"
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
    *cmd = (Command){0};
}

// Освобождает все команды конвейера и сам массив команд.
void pipeline_free(Pipeline *pipeline) {
    for (size_t i = 0; i < pipeline->count; ++i)
        command_free(&pipeline->commands[i]);

    free(pipeline->commands);
    pipeline->commands = NULL;
    pipeline->count = 0;
}

/* простые парсеры читают строку  */
typedef struct { const char *begin; size_t length; } Word;

static PcResult word_parse(PcInput *in, const void *data, void *out) {
    (void)data;
    //пропускает пробелы
    pc_spaces(in);
    const char *start = in->text + in->pos;
    size_t n = 0;
    //ищет конец слова
    while (start[n] && !isspace((unsigned char)start[n])) ++n;
    if (!n) return PC_MISS;
    //проверяет что слово не оператор
    const char *operators[] = {"|", "||", "<", ">", ">>", "&", "&&", ";"};
    for (size_t i = 0; i < sizeof operators / sizeof *operators; ++i)
        if (strlen(operators[i]) == n && !strncmp(start, operators[i], n))
            return PC_MISS;
    //сохраняет начало и длину
    *(Word *)out = (Word){start, n};
    //передвигает позицию
    in->pos += n;
    return PC_MATCH;
}

//превращает найденное слово в аргумент команды
static PcResult argument_parse(PcInput *in, const void *data, void *out) {
    (void)data;
    Word word;
    PcResult r = pc_apply((PcParser){word_parse, NULL}, in, &word);
    if (r != PC_MATCH) return r;
    Command *cmd = out;
    if (cmd->argc > SIZE_MAX / sizeof(char *) - 2) return PC_OOM;
    //создает коптю слова
    char *value = strndup(word.begin, word.length);
    if (!value) return PC_OOM;
    //расширяется массив argv, +2 для слова и null
    char **args = realloc(cmd->argv, (cmd->argc + 2) * sizeof *args);
    if (!args) { free(value); return PC_OOM; }
    cmd->argv = args;
    cmd->argv[cmd->argc++] = value;
    cmd->argv[cmd->argc] = NULL;
    return PC_MATCH;
}

/* Путь сохраняется только после полного разбора перенаправления. */
static PcResult redirection_parse(PcInput *in, const void *data, void *out) {
    const char *op = data;
    // создаем послежовательность оператор + имя файла
    //через word_parse парсим слово
    PcPair syntax = {{pc_literal, op}, {word_parse, NULL}};
    Word word;
    PcResult r = pc_apply((PcParser){pc_sequence, &syntax}, in, &word);
    if (r != PC_MATCH) return r;
    //копия пути
    char *path = strndup(word.begin, word.length);
    if (!path) return PC_OOM;
    Command *cmd = out;
    //выбираем нужное поле команды < >> >
    char **target = op[0] == '<' ? &cmd->input_path : &cmd->output_path;
    //освобождаем путь для этого потока и заменяем его
    free(*target);
    *target = path;
    if (op[0] != '<') cmd->append_output = !strcmp(op, ">>");
    return PC_MATCH;
}

static PcResult command_parse(PcInput *in, const void *data, void *out) {
    (void)data;
    //парсер для >> > с именем файла
    PcPair outputs = {{redirection_parse, ">>"}, {redirection_parse, ">"}};
    // парсер для < с именем файла
    PcPair redirects = {{redirection_parse, "<"}, {pc_choice, &outputs}};
    // перенаправление или аргумент
    PcPair item = {{pc_choice, &redirects}, {argument_parse, NULL}};
    //один или больше айтемов
    PcRepeat items = {{pc_choice, &item}, 1};
    //формируем структ команды
    Command cmd = {0};
    PcResult r = pc_apply((PcParser){pc_many, &items}, in, &cmd);
    if (r == PC_MATCH && !cmd.argc) r = PC_ERROR;
    if (r != PC_MATCH) { command_free(&cmd); return r; }
    //если успех то добавлям ее в массив конвейера
    Pipeline *pipeline = out;
    if (pipeline->count >= SIZE_MAX / sizeof(Command)) {
        command_free(&cmd); return PC_OOM;
    }
    Command *next = realloc(pipeline->commands,
                           (pipeline->count + 1) * sizeof *next);
    if (!next) { command_free(&cmd); return PC_OOM; }
    pipeline->commands = next;
    pipeline->commands[pipeline->count++] = cmd;
    return PC_MATCH;
}

//собираем конвейер
static PcResult pipeline_parse(PcInput *in, const void *data, void *out) {
    (void)data;
    //собираем пару
    PcPair syntax = {{command_parse, NULL}, {pc_literal, "|"}};
    return pc_apply((PcParser){pc_sep_by1, &syntax}, in, out);
}

//добавление конейера в план
static PcResult plan_item_parse(PcInput *in, const void *data, void *out) {
    (void)data;
    Pipeline pipeline = {0};
    //разбираем план
    PcResult r = pc_apply((PcParser){pipeline_parse, NULL}, in, &pipeline);
    if (r != PC_MATCH) { pipeline_free(&pipeline); return r; }
    ExecutionPlan *plan = out;
    if (plan->count >= SIZE_MAX / sizeof(Pipeline)) {
        pipeline_free(&pipeline); return PC_OOM;
    }
    //при успезе добалвяем в план
    Pipeline *next = realloc(plan->pipelines, (plan->count + 1) * sizeof *next);
    if (!next) { pipeline_free(&pipeline); return PC_OOM; }
    plan->pipelines = next;
    plan->pipelines[plan->count++] = pipeline;
    return PC_MATCH;
}

void plan_free(ExecutionPlan *plan) {
    for (size_t i = 0; i < plan->count; ++i) pipeline_free(&plan->pipelines[i]);
    free(plan->pipelines);
    *plan = (ExecutionPlan){0};
}

//проверяет чтобы была разобрана вся строка, проверяет конец строки
static int finish(PcResult r, PcInput *in) {
    if (r == PC_OOM) return -1;
    pc_spaces(in);
    if (r != PC_MATCH || in->text[in->pos]) {
        fprintf(stderr, "vtsh: syntax error at position %zu\n", in->pos + 1);
        return 1;
    }
    return 0;
}

/* разбирает один конвейер, требует чтобы после | строка закончилась */
int parse_pipeline(const char *line, Pipeline *pipeline) {
    PcInput in = {line, 0};
    Pipeline parsed = {0};
    pc_spaces(&in);
    if (!in.text[in.pos]) return 0;
    int status = finish(pc_apply((PcParser){pipeline_parse, NULL}, &in, &parsed), &in);
    if (status) pipeline_free(&parsed);
    else *pipeline = parsed;
    return status;
}

//публичная точка входа
int parse_plan(const char *line, ExecutionPlan *plan) {
    PcInput in = {line, 0};
    ExecutionPlan parsed = {0};
    pc_spaces(&in);
    if (!in.text[in.pos]) return 0;
    PcPair syntax = {{plan_item_parse, NULL}, {pc_literal, "||"}};
    int status = finish(pc_apply((PcParser){pc_sep_by1, &syntax}, &in, &parsed), &in);
    if (status) plan_free(&parsed);
    else *plan = parsed;
    return status;
}
