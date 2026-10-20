#define _POSIX_C_SOURCE 200809L
#include "executor.h"
#include "process_manager.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Настраивает перенаправление ввода (<) и вывода (>, >>).
static int apply_redirections(const Command *cmd) {
    if (cmd->input_path) {
        int fd = open(cmd->input_path, O_RDONLY);
        if (fd < 0) { perror("vtsh: input redirection"); return -1; }

        // Подменяем стандартный ввод открытым файлом.
        if (dup2(fd, STDIN_FILENO) < 0) {
            perror("vtsh: dup2 input"); close(fd); return -1;
        }
        close(fd);
    }

    if (cmd->output_path) {
        int flags = O_WRONLY | O_CREAT | (cmd->append_output ? O_APPEND : O_TRUNC);
        int fd = open(cmd->output_path, flags, 0666);
        if (fd < 0) { perror("vtsh: output redirection"); return -1; }

        // Подменяем стандартный вывод открытым файлом.
        if (dup2(fd, STDOUT_FILENO) < 0) {
            perror("vtsh: dup2 output"); close(fd); return -1;
        }
        close(fd);
    }
    return 0;
}

// Запускает внешнюю команду через fork() и execvp().
static int run_external(const Command *cmd) {
    pid_t pid = fork();

    if (pid < 0) { perror("vtsh: fork"); return 1; }

    if (pid == 0) {
        // Дочерний процесс: настраиваем потоки и запускаем программу.
        if (apply_redirections(cmd) < 0) _exit(1);

        execvp(cmd->argv[0], cmd->argv);

        // Сюда попадём только при ошибке execvp().
        int err = errno;
        const char *reason = strerror(err);
        dprintf(STDERR_FILENO, "vtsh: %s: %s\n", cmd->argv[0], reason);
        _exit(err == ENOENT || err == ENOTDIR ? 127 : 126);
    }

    return process_wait(pid);
}

// Выполняет одну команду: встроенную cd/exit или внешнюю программу.
static int execute_command(const Command *cmd, int last_status, bool *should_exit) {
    if (!cmd->argc) return last_status;

    // exit завершает оболочку и возвращает указанный код.
    if (!strcmp(cmd->argv[0], "exit")) {
        if (cmd->argc > 2) {
            fprintf(stderr, "vtsh: exit: too many arguments\n");
            return 2;
        }

        if (cmd->argc == 1) {
            *should_exit = true;
            return last_status;
        }

        char *end;
        errno = 0;
        long value = strtol(cmd->argv[1], &end, 10);

        if (errno || end == cmd->argv[1] || *end) {
            fprintf(stderr, "vtsh: exit: numeric argument required\n");
            *should_exit = true;
            return 2;
        }

        *should_exit = true;
        return (unsigned char)value;
    }

    // cd меняет рабочий каталог самого процесса оболочки.
    if (!strcmp(cmd->argv[0], "cd")) {
        if (cmd->argc > 2) {
            fprintf(stderr, "vtsh: cd: too many arguments\n");
            return 1;
        }

        const char *path = cmd->argc == 2 ? cmd->argv[1] : getenv("HOME");

        if (!path) {
            fprintf(stderr, "vtsh: cd: HOME not set\n");
            return 1;
        }

        if (chdir(path) < 0) {
            perror("vtsh: cd");
            return 1;
        }

        return 0;
    }

    // Если команда не встроенная, запускаем отдельный процесс.
    return run_external(cmd);
}


// Выполняет несколько команд, соединённых оператором |.
static int run_pipeline(const Pipeline *pipeline) {
    const size_t n = pipeline->count;

    // Массив PID нужен, чтобы дождаться всех дочерних процессов.
    pid_t *pids = calloc(n, sizeof(*pids));
    if (!pids) { perror("vtsh: calloc"); return 1; }

    size_t started = 0;
    int previous_read = -1;
    int failed = 0;

    for (size_t i = 0; i < n; ++i) {
        int fds[2] = {-1, -1};

        // Создаём pipe между текущей и следующей командой.
        // Для последней команды pipe не нужен.
        if (i + 1 < n && pipe(fds) < 0) {
            perror("vtsh: pipe");
            failed = 1;
            break;
        }

        pid_t pid = fork();

        if (pid < 0) {
            perror("vtsh: fork");
            if (fds[0] >= 0) close(fds[0]);
            if (fds[1] >= 0) close(fds[1]);
            failed = 1;
            break;
        }

        if (pid == 0) {
            // previous_read — чтение из предыдущего pipe.
            // fds[1] — запись в следующий pipe.
            if (previous_read >= 0 && dup2(previous_read, STDIN_FILENO) < 0) {
                perror("vtsh: dup2 stdin");
                _exit(1);
            }

            if (fds[1] >= 0 && dup2(fds[1], STDOUT_FILENO) < 0) {
                perror("vtsh: dup2 stdout");
                _exit(1);
            }

            if (previous_read >= 0) close(previous_read);
            if (fds[0] >= 0) close(fds[0]);
            if (fds[1] >= 0) close(fds[1]);

            const Command *cmd = &pipeline->commands[i];

            // Перенаправления файлов имеют приоритет над pipe.
            if (apply_redirections(cmd) < 0) _exit(1);

            // Встроенные команды внутри pipe выполняются в дочернем процессе.
            if (!strcmp(cmd->argv[0], "cd") || !strcmp(cmd->argv[0], "exit")) {
                bool should_exit = false;
                _exit(execute_command(cmd, 0, &should_exit));
            }

            // Заменяем дочерний процесс внешней программой.
            execvp(cmd->argv[0], cmd->argv);

            int err = errno;
            dprintf(STDERR_FILENO, "vtsh: %s: %s\n", cmd->argv[0], strerror(err));
            _exit(err == ENOENT || err == ENOTDIR ? 127 : 126);
        }

        // Родитель запоминает PID и закрывает ненужные концы pipe.
        pids[started++] = pid;

        if (previous_read >= 0) close(previous_read);
        if (fds[1] >= 0) close(fds[1]);

        previous_read = fds[0];
    }

    if (previous_read >= 0) close(previous_read);

    int result = failed ? 1 : 0;

    for (size_t i = 0; i < started; ++i) {
        int status = process_wait(pids[i]);
        if (!failed && i == n - 1) result = status;
    }

    free(pids);
    return result;
}


// Выбирает способ выполнения: одна команда или целый конвейер.
int execute_pipeline(const Pipeline *pipeline, int last_status, bool *should_exit) {
    if (!pipeline->count) return last_status;

    if (pipeline->count == 1) {
        const Command *cmd = &pipeline->commands[0];

        // Для cd/exit с перенаправлением временно меняем поток самой оболочки, а после выполнения восстанавливаем их.
        if (cmd->argc &&
            (!strcmp(cmd->argv[0], "cd") || !strcmp(cmd->argv[0], "exit")) &&
            (cmd->input_path || cmd->output_path)) {

            int saved_in = -1, saved_out = -1;
            fflush(stdout);

            if (cmd->input_path) {
                saved_in = dup(STDIN_FILENO);
                if (saved_in < 0) {
                    perror("vtsh: dup stdin");
                    return 1;
                }
            }

            if (cmd->output_path) {
                saved_out = dup(STDOUT_FILENO);
                if (saved_out < 0) {
                    perror("vtsh: dup stdout");
                    if (saved_in >= 0) close(saved_in);
                    return 1;
                }
            }

            int status;

            if (apply_redirections(cmd) < 0)
                status = 1;
            else
                status = execute_command(cmd, last_status, should_exit);

            fflush(stdout);

            // Возвращаем исходные stdin и stdout оболочки.
            if (saved_in >= 0) {
                if (dup2(saved_in, STDIN_FILENO) < 0)
                    perror("vtsh: restore stdin");
                close(saved_in);
            }

            if (saved_out >= 0) {
                if (dup2(saved_out, STDOUT_FILENO) < 0)
                    perror("vtsh: restore stdout");
                close(saved_out);
            }

            return status;
        }

        // Одна команда без особой обработки перенаправлений.
        return execute_command(cmd, last_status, should_exit);
    }

    // Несколько команд - запускаем их через pipe.
    return run_pipeline(pipeline);
}