
import ctypes as c
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MATCH, MISS, ERROR = 1, 0, -1


class Input(c.Structure):
    _fields_ = [('text', c.c_char_p), ('pos', c.c_size_t)]


Function = c.CFUNCTYPE(c.c_int, c.POINTER(Input), c.c_void_p, c.c_void_p)


class Parser(c.Structure):
    _fields_ = [('run', Function), ('data', c.c_void_p)]


class Pair(c.Structure):
    _fields_ = [('first', Parser), ('second', Parser)]


class Repeat(c.Structure):
    _fields_ = [('parser', Parser), ('minimum', c.c_size_t)]


class Command(c.Structure):
    _fields_ = [('argv', c.POINTER(c.c_char_p)), ('argc', c.c_size_t),
                ('input_path', c.c_char_p), ('output_path', c.c_char_p),
                ('append_output', c.c_int)]


class Pipeline(c.Structure):
    _fields_ = [('commands', c.POINTER(Command)), ('count', c.c_size_t)]


class Plan(c.Structure):
    _fields_ = [('pipelines', c.POINTER(Pipeline)), ('count', c.c_size_t)]


class ParserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        # Компилируем именно текущие исходники, а не используем старый бинарник.
        library = Path(cls.temp.name) / ('parser.dylib' if sys.platform == 'darwin' else 'parser.so')
        subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-shared', '-fPIC',
            str(ROOT / 'src/parser.c'), str(ROOT / 'src/combinators.c'),
            '-o', str(library)], check=True)
        cls.lib = c.CDLL(str(library))
        cls.lib.parse_plan.argtypes = [c.c_char_p, c.POINTER(Plan)]
        cls.lib.parse_plan.restype = c.c_int
        cls.lib.plan_free.argtypes = [c.POINTER(Plan)]
        cls.lib.plan_free.restype = None
        cls.lib.parse_pipeline.argtypes = [c.c_char_p, c.POINTER(Pipeline)]
        cls.lib.parse_pipeline.restype = c.c_int
        cls.lib.pipeline_free.argtypes = [c.POINTER(Pipeline)]
        cls.lib.pipeline_free.restype = None
        cls.lib.pc_apply.argtypes = [Parser, c.POINTER(Input), c.c_void_p]
        cls.lib.pc_apply.restype = c.c_int

    def parser(self, name, data=None):
        return Parser(Function((name, self.lib)),
                      c.cast(c.pointer(data), c.c_void_p) if data is not None else None)

    def literal(self, text):
        return self.parser('pc_literal', c.create_string_buffer(text))

    def apply(self, parser, text):
        source = Input(text, 0)
        result = self.lib.pc_apply(parser, c.byref(source), None)
        return result, source.pos

    def test_plan_structure(self):
        plan = Plan()
        try:
            self.assertEqual(self.lib.parse_plan(
                b'< input echo one > old >> final | cat || echo fallback', c.byref(plan)), 0)
            self.assertEqual(plan.count, 2)
            self.assertEqual(plan.pipelines[0].count, 2)
            cmd = plan.pipelines[0].commands[0]
            self.assertEqual([cmd.argv[i] for i in range(cmd.argc)], [b'echo', b'one'])
            self.assertIsNone(cmd.argv[cmd.argc])
            self.assertEqual(cmd.input_path, b'input')
            self.assertEqual(cmd.output_path, b'final')
            self.assertEqual(cmd.append_output, 1)
        finally:
            self.lib.plan_free(c.byref(plan))
        self.lib.plan_free(c.byref(plan))  # Повторное освобождение безопасно.

    def test_invalid_plans_are_empty(self):
        for text in [b'| echo', b'echo |', b'echo ||', b'|| echo', b'echo >',
                     b'echo < | cat', b'echo > || cat', b'> file', b'echo && cat',
                     b'echo ; cat', b'echo &', b'echo | | cat', b'echo || || cat',
                     b'echo > > file']:
            with self.subTest(text=text):
                plan = Plan()
                try:
                    self.assertEqual(self.lib.parse_plan(text, c.byref(plan)), 1)
                    self.assertEqual(plan.count, 0)
                    self.assertFalse(plan.pipelines)
                finally:
                    self.lib.plan_free(c.byref(plan))

    def test_empty_and_attached_operators(self):
        for text, count in [(b' \t\n', 0), (b'echo a|b x>>y', 1)]:
            plan = Plan()
            try:
                self.assertEqual(self.lib.parse_plan(text, c.byref(plan)), 0)
                self.assertEqual(plan.count, count)
                if count:
                    self.assertEqual(plan.pipelines[0].commands[0].argc, 3)
            finally:
                self.lib.plan_free(c.byref(plan))

    def test_pipeline_interface(self):
        for text, status, count in [(b'echo | cat', 0, 2), (b'echo || cat', 1, 0)]:
            pipeline = Pipeline()
            try:
                self.assertEqual(self.lib.parse_pipeline(text, c.byref(pipeline)), status)
                self.assertEqual(pipeline.count, count)
            finally:
                self.lib.pipeline_free(c.byref(pipeline))

    def test_literal_rollback(self):
        self.assertEqual(self.apply(self.literal(b'|'), b'  x'), (MISS, 0))

    def test_choice_and_operator_boundary(self):
        pair = Pair(self.literal(b'>'), self.literal(b'>>'))
        self.assertEqual(self.apply(self.parser('pc_choice', pair), b'  >>'), (MATCH, 4))

    def test_sequence_commits_after_first_match(self):
        pair = Pair(self.literal(b'x'), self.literal(b'y'))
        self.assertEqual(self.apply(self.parser('pc_sequence', pair), b'x')[0], ERROR)

    def test_many_minimum_and_repetition(self):
        for text, minimum, expected in [(b'', 0, MATCH), (b'', 1, MISS),
                                        (b'x x', 1, MATCH), (b'x', 2, ERROR)]:
            with self.subTest(text=text, minimum=minimum):
                repeat = Repeat(self.literal(b'x'), minimum)
                self.assertEqual(self.apply(self.parser('pc_many', repeat), text)[0], expected)

    def test_separated_elements(self):
        pair = Pair(self.literal(b'x'), self.literal(b'|'))
        parser = self.parser('pc_sep_by1', pair)
        self.assertEqual(self.apply(parser, b'x | x'), (MATCH, 5))
        self.assertEqual(self.apply(parser, b'x |')[0], ERROR)

    def test_empty_parser_cannot_loop_forever(self):
        @Function
        def empty(source, data, output):
            return MATCH
        parser = Parser(empty, None)
        repeat = Repeat(parser, 0)
        self.assertEqual(self.apply(self.parser('pc_many', repeat), b'')[0], ERROR)
        pair = Pair(self.literal(b'x'), parser)
        self.assertEqual(self.apply(self.parser('pc_sep_by1', pair), b'x')[0], ERROR)


if __name__ == '__main__':
    unittest.main()
