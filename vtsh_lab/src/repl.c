#define _POSIX_C_SOURCE 200809L
#include "repl.h"
#include "parser.h"
#include "executor.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int repl_run(void) {
    char *line = NULL;
    size_t capacity = 0;
    int last_status = 0;
    bool interactive = isatty(STDIN_FILENO);
    //бесконечный цикл
    for (;;) {
        if (interactive) { fputs("vtsh> ", stdout); fflush(stdout); }
        errno = 0;
        ssize_t count = getline(&line, &capacity, stdin);
        //обработка CTRL-D
        if (count < 0) {
            if (ferror(stdin)) { perror("vtsh: getline"); last_status = 1; }
            break;
        }
        ExecutionPlan plan = {0};
        int result = parse_plan(line, &plan);
        if (result == -1) {
            fprintf(stderr, "vtsh: out of memory\n");
            plan_free(&plan);
            last_status = 1;
            break;
        }
        //синтаксическая ошибка - повтор
        if (result == 1) {
            plan_free(&plan);
            last_status = 2;
            continue;
        }
        bool should_exit = false;
        //план содержит кол-во конвейеров
        for (size_t i = 0; i < plan.count; ++i) {
            //короткое замыкание или ленивая обработка
            if (i > 0 && last_status == 0) break;
            last_status = execute_pipeline(&plan.pipelines[i], last_status, &should_exit);
            if (should_exit) break;
        }
        plan_free(&plan);
        //если введена exit
        if (should_exit) break;
    }
    free(line);
    return last_status;
}
