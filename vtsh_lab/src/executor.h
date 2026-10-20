#ifndef VTSH_EXECUTOR_H
#define VTSH_EXECUTOR_H
#include "command.h"
#include <stdbool.h>
int execute_pipeline(const Pipeline *pipeline, int last_status, bool *should_exit);
#endif
