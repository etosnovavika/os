#ifndef VTSH_PARSER_H
#define VTSH_PARSER_H
#include "command.h"
/* 0 success, 1 invalid syntax, -1 allocation failure. */
int parse_plan(const char *line, ExecutionPlan *plan);
int parse_pipeline(const char *line, Pipeline *pipeline);
#endif
