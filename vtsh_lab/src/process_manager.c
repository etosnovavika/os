#include "process_manager.h"
#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
int process_wait(pid_t pid) {
    int status;
    for (;;) {
        pid_t result = waitpid(pid, &status, 0);
        //цикл ожидания
        if (result == pid) break;
        if (result < 0 && errno == EINTR) continue;
        perror("vtsh: waitpid");
        return 1;
    }
    //обработка результатов
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}
