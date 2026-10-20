import os
import pathlib
import subprocess
import tempfile
import unittest

BINARY = pathlib.Path(__file__).resolve().parents[1] / 'build/bin/vtsh'


def run_shell(commands, env=None, cwd=None):
    return subprocess.run([str(BINARY)], input=commands, text=True,
                          capture_output=True, timeout=5, env=env, cwd=cwd)


class ShellTests(unittest.TestCase):
    def test_empty_and_eof(self):
        self.assertEqual(run_shell(' \n\n').returncode, 0)

    def test_absolute_path(self):
        self.assertEqual(run_shell('/bin/echo hello world\n').stdout, 'hello world\n')

    def test_multiple_spaces(self):
        self.assertEqual(run_shell('  /bin/echo    hello   \n').stdout, 'hello\n')

    def test_environment(self):
        env = os.environ.copy()
        env['VTSH_TEST_VAR'] = 'inherited'
        self.assertIn('VTSH_TEST_VAR=inherited', run_shell('/usr/bin/env\n', env=env).stdout)

    def test_missing_program(self):
        result = run_shell('/definitely/not/a/program\n')
        self.assertEqual(result.returncode, 127)

    def test_non_executable(self):
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d) / 'file'
            p.write_text('not executable')
            p.chmod(0o644)
            self.assertEqual(run_shell(str(p) + '\n').returncode, 126)

    def test_cd(self):
        with tempfile.TemporaryDirectory() as d:
            result = run_shell(f'cd {d}\n/bin/pwd\n')
            self.assertEqual(result.stdout.strip(), os.path.realpath(d))

    def test_exit_status(self):
        self.assertEqual(run_shell('exit 42\n').returncode, 42)

    def test_external_status(self):
        self.assertEqual(run_shell('/usr/bin/false\n').returncode, 1)

    def test_pipe_and_next_command(self):
        result = run_shell('/bin/echo one | /bin/cat\n/bin/echo alive\n')
        self.assertEqual(result.stdout, 'one\nalive\n')

    def test_pipe_three_stages(self):
        result = run_shell('/bin/echo hello | /bin/cat | /usr/bin/wc -c\n')
        self.assertEqual(result.stdout.strip(), '6')

    def test_pipe_last_stage_status(self):
        self.assertEqual(run_shell('/bin/echo hello | /usr/bin/false\n').returncode, 1)

    def test_pipe_missing_command(self):
        result = run_shell('/bin/echo hello |\n/bin/echo ok\n')
        self.assertEqual(result.stdout, 'ok\n')
        self.assertIn('syntax error', result.stderr)

    def test_pipe_missing_first_command(self):
        result = run_shell('| /bin/cat\n')
        self.assertEqual(result.returncode, 2)

    def test_pipe_large_stream(self):
        result = run_shell('/usr/bin/yes | /usr/bin/head -n 10000 | /usr/bin/wc -l\n')
        self.assertEqual(result.stdout.strip(), '10000')

    def test_command_after_error(self):
        result = run_shell('/definitely/missing\n/bin/echo next\n')
        self.assertEqual(result.stdout, 'next\n')
        self.assertEqual(result.returncode, 0)

    def test_exit_too_many_arguments(self):
        result = run_shell('exit 1 2\n/bin/echo still-running\n')
        self.assertIn('still-running', result.stdout)


    def test_exit_uses_previous_status(self):
        self.assertEqual(run_shell('/usr/bin/false\nexit\n').returncode, 1)

    def test_empty_line_keeps_status(self):
        self.assertEqual(run_shell('/usr/bin/false\n   \n').returncode, 1)

    def test_cd_home(self):
        with tempfile.TemporaryDirectory() as d:
            env = os.environ.copy()
            env['HOME'] = d
            result = run_shell('cd\n/bin/pwd\n', env=env)
            self.assertEqual(result.stdout.strip(), os.path.realpath(d))

    def test_invalid_exit_argument(self):
        result = run_shell('exit xyz\n/bin/echo should-not-run\n')
        self.assertEqual(result.returncode, 2)
        self.assertNotIn('should-not-run', result.stdout)

    def test_many_arguments(self):
        words = [f'arg{i}' for i in range(150)]
        result = run_shell('/bin/echo ' + ' '.join(words) + '\n')
        self.assertEqual(result.stdout.strip(), ' '.join(words))

    def test_missing_home(self):
        env = os.environ.copy()
        env.pop('HOME', None)
        result = run_shell('cd\n', env=env)
        self.assertEqual(result.returncode, 1)
        self.assertIn('HOME not set', result.stderr)

    def test_unsupported_logical_operator(self):
        result = run_shell('/bin/true && /bin/echo wrong\n/bin/echo ok\n')
        self.assertEqual(result.stdout, 'ok\n')

    def test_signal_status(self):
        import sys
        with tempfile.TemporaryDirectory() as d:
            script = pathlib.Path(d) / 'signal_self.py'
            script.write_text('import os, signal\nos.kill(os.getpid(), signal.SIGTERM)\n')
            result = run_shell(f'{sys.executable} {script}\n')
            self.assertEqual(result.returncode, 128 + 15)


    def test_cd_inside_pipeline_does_not_change_parent(self):
        with tempfile.TemporaryDirectory() as d:
            result = run_shell('cd /tmp | /bin/cat\n/bin/pwd\n', cwd=d)
            self.assertEqual(result.stdout.strip(), os.path.realpath(d))

    def test_exit_inside_pipeline_does_not_exit_shell(self):
        result = run_shell('exit 42 | /bin/cat\n/bin/echo alive\n')
        self.assertEqual(result.stdout, 'alive\n')
        self.assertEqual(result.returncode, 0)

    def test_missing_first_pipeline_stage_shell_recovers(self):
        result = run_shell('/definitely/missing | /bin/cat\n/bin/echo recovered\n')
        self.assertIn('recovered\n', result.stdout)
        self.assertIn('missing', result.stderr)
        self.assertEqual(result.returncode, 0)

    def test_missing_last_pipeline_stage_status(self):
        result = run_shell('/bin/echo hello | /definitely/missing\n')
        self.assertEqual(result.returncode, 127)

    def test_redirect_output_truncates(self):
        with tempfile.TemporaryDirectory() as d:
            path = pathlib.Path(d) / 'output.txt'
            path.write_text('old content')
            result = run_shell(f'/bin/echo hello > {path}\n')
            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.stdout, '')
            self.assertEqual(path.read_text(), 'hello\n')

    def test_redirect_output_append(self):
        with tempfile.TemporaryDirectory() as d:
            path = pathlib.Path(d) / 'output.txt'
            result = run_shell(f'/bin/echo one > {path}\n/bin/echo two >> {path}\n')
            self.assertEqual(result.returncode, 0)
            self.assertEqual(path.read_text(), 'one\ntwo\n')

    def test_redirect_input(self):
        with tempfile.TemporaryDirectory() as d:
            path = pathlib.Path(d) / 'input.txt'
            path.write_text('hello\nworld\n')
            result = run_shell(f'/bin/cat < {path}\n')
            self.assertEqual(result.stdout, 'hello\nworld\n')

    def test_redirect_pipeline_to_file(self):
        with tempfile.TemporaryDirectory() as d:
            path = pathlib.Path(d) / 'count.txt'
            result = run_shell(f'/bin/echo hello | /usr/bin/wc -c > {path}\n')
            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.stdout, '')
            self.assertEqual(path.read_text().strip(), '6')

    def test_redirect_input_pipeline(self):
        with tempfile.TemporaryDirectory() as d:
            path = pathlib.Path(d) / 'input.txt'
            path.write_text('abc\n')
            result = run_shell(f'/bin/cat < {path} | /usr/bin/wc -c\n')
            self.assertEqual(result.stdout.strip(), '4')

    def test_redirection_missing_filename_recovers(self):
        result = run_shell('/bin/echo hello >\n/bin/echo alive\n')
        self.assertIn('syntax error', result.stderr)
        self.assertEqual(result.stdout, 'alive\n')

    def test_redirection_invalid_input_recovers(self):
        result = run_shell('/bin/cat < /definitely/missing/file\n/bin/echo alive\n')
        self.assertIn('input redirection', result.stderr)
        self.assertEqual(result.stdout, 'alive\n')

    def test_redirection_invalid_output_recovers(self):
        result = run_shell('/bin/echo hello > /definitely/missing/dir/file\n/bin/echo alive\n')
        self.assertIn('output redirection', result.stderr)
        self.assertEqual(result.stdout, 'alive\n')

    def test_builtin_redirection_does_not_leak(self):
        with tempfile.TemporaryDirectory() as d:
            path = pathlib.Path(d) / 'output.txt'
            result = run_shell(f'cd {d} > {path}\n/bin/echo alive\n')
            self.assertEqual(result.stdout, 'alive\n')
            self.assertEqual(path.read_text(), '')

    def test_pipeline_output_override_no_hang(self):
        with tempfile.TemporaryDirectory() as d:
            path = pathlib.Path(d) / 'out.txt'
            result = run_shell(f'/bin/echo hello > {path} | /usr/bin/wc -c\n')
            self.assertEqual(result.stdout.strip(), '0')
            self.assertEqual(path.read_text(), 'hello\n')

    def test_redirection_missing_command(self):
        result = run_shell('> /tmp/vtsh_unused_test_output\n/bin/echo alive\n')
        self.assertEqual(result.stdout, 'alive\n')
        self.assertIn('syntax error', result.stderr)


if __name__ == '__main__':
    unittest.main()

class OrOperatorTests(unittest.TestCase):
    def test_or_fallback(self):
        self.assertEqual(run_shell('/usr/bin/false || /bin/echo fallback\n').stdout, 'fallback\n')

    def test_or_short_circuit(self):
        self.assertEqual(run_shell('/usr/bin/true || /bin/echo never\n').stdout, '')

    def test_or_three_stages(self):
        self.assertEqual(run_shell('/usr/bin/false || /usr/bin/false || /bin/echo recovered\n').stdout, 'recovered\n')

    def test_or_pipeline_success(self):
        self.assertEqual(run_shell('/bin/echo hello | /usr/bin/wc -c || /bin/echo never\n').stdout.strip(), '6')

    def test_or_pipeline_failure(self):
        self.assertEqual(run_shell('/bin/echo hello | /usr/bin/false || /bin/echo recovered\n').stdout, 'recovered\n')

    def test_or_missing_program(self):
        result = run_shell('/does/not/exist || /bin/echo recovered\n')
        self.assertEqual(result.stdout, 'recovered\n')
        self.assertEqual(result.returncode, 0)

    def test_or_syntax_recovery(self):
        result = run_shell('/bin/echo first ||\n/bin/echo after\n')
        self.assertEqual(result.stdout, 'after\n')
        self.assertIn('syntax error', result.stderr)

    def test_or_leading_syntax(self):
        result = run_shell('|| /bin/echo bad\n/bin/echo after\n')
        self.assertEqual(result.stdout, 'after\n')

    def test_or_not_run_exit(self):
        result = run_shell('/usr/bin/true || exit 17\n/bin/echo alive\n')
        self.assertEqual(result.stdout, 'alive\n')

    def test_or_redirection(self):
        with tempfile.TemporaryDirectory() as d:
            output = pathlib.Path(d) / 'result.txt'
            result = run_shell(f'/usr/bin/false || /bin/echo success > {output}\n')
            self.assertEqual(result.returncode, 0)
            self.assertEqual(output.read_text(), 'success\n')
