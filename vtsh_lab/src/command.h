#ifndef VTSH_COMMAND_H
#define VTSH_COMMAND_H
#include <stddef.h>
//структуры
typedef struct { char **argv; size_t argc; char *input_path; char *output_path; int append_output; } Command;
typedef struct { Command *commands; size_t count; } Pipeline;
typedef struct { Pipeline *pipelines; size_t count; } ExecutionPlan;
//функции для освобождения памяти
void plan_free(ExecutionPlan *plan);
void command_free(Command *cmd);
void pipeline_free(Pipeline *pipeline);
#endif
