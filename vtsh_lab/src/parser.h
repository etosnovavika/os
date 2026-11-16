#ifndef VTSH_PARSER_H
#define VTSH_PARSER_H
#include "command.h"
/* 0 — успех, 1 — ошибка синтаксиса, -1 — нехватка памяти. */
int parse_plan(const char *line, ExecutionPlan *plan);
int parse_pipeline(const char *line, Pipeline *pipeline);
#endif
