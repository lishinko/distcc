"""测试 distcc 的响应文件展开，可在 POSIX 和 Windows 上直接运行。

现有 comfychair 测试框架用 os.fork() 启动子进程，Windows 原生 Python
即使从 MSYS2 启动也没有这个接口。这组测试只需运行 distcc 并检查退出码、
日志和产物，因此用 unittest 和 subprocess.run()，无需另装 MSYS2 Python。

默认从 PATH 查找 distcc 和 cc。可用 DISTCC_RSP_TEST_CLIENT 和
DISTCC_RSP_TEST_COMPILER 指定可执行文件。默认使用 localhost；
设置 DISTCC_RSP_TEST_SERVER 可改用一个已启动的 TCP 服务端
（例如 127.0.0.1:42000,lzo），并验证编译确实在该端完成。
"""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


WINDOWS_PATHS = os.name == "nt" or sys.platform.startswith(("cygwin", "msys"))


class ResponseFileTest(unittest.TestCase):
    def setUp(self):
        self.client = self.find_program("DISTCC_RSP_TEST_CLIENT", "distcc")
        self.compiler = self.find_program("DISTCC_RSP_TEST_COMPILER", "cc")
        tempdir = tempfile.TemporaryDirectory(prefix="distcc-rsp-")
        self.addCleanup(tempdir.cleanup)
        self.work = Path(tempdir.name)
        self.log = self.work / "distcc.log"
        self.server = os.environ.get("DISTCC_RSP_TEST_SERVER")
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith("DISTCC_")}
        self.env.update(DISTCC_HOSTS=self.server or "localhost",
                        DISTCC_DIR=self.work.as_posix(),
                        TMPDIR=self.work.as_posix(),
                        DISTCC_LOG=self.log.as_posix(),
                        DISTCC_VERBOSE="1",
                        DISTCC_FALLBACK="0",
                        DISTCC_NO_REWRITE_CROSS="1")

    def find_program(self, variable, default):
        name = os.environ.get(variable, default)
        path = shutil.which(name)
        self.assertIsNotNone(path, "未找到 {}（可用 {} 指定）".format(name, variable))
        return Path(path).absolute().as_posix()

    def run_distcc(self, *args):
        return subprocess.run([self.client, *args], cwd=str(self.work),
                              env=self.env, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True, timeout=30)

    def compile_response(self, separator, escaped_space=False):
        for name in ("rsp files", "source files", "object files"):
            (self.work / name).mkdir()
        (self.work / "source files" / "sample.c").write_text(
            "#if !defined(RSP_VALUE) || RSP_VALUE != 42\n"
            "#error wrong response file options\n"
            "#endif\n"
            "int response_file_value(void) { return RSP_VALUE; }\n",
            encoding="ascii")

        # 顶层引用、嵌套引用、源文件和输出都走相同的路径风格。
        def path_arg(directory, filename):
            path = separator.join((directory, filename))
            if escaped_space:
                return path.replace(" ", "\\ ")
            return '"{}"'.format(path)

        inner = " ".join(("-DRSP_VALUE=42", "-c",
                          path_arg("source files", "sample.c"), "-o",
                          path_arg("object files", "sample.o")))
        (self.work / "rsp files" / "inner.rsp").write_bytes(
            inner.encode("ascii") + b"\r\n")
        nested = separator.join(("rsp files", "inner.rsp"))
        (self.work / "rsp files" / "outer.rsp").write_text(
            '"@{}"\n'.format(nested), encoding="ascii")

        outer = separator.join(("rsp files", "outer.rsp"))
        result = self.run_distcc(self.compiler, "@" + outer)
        log = self.log.read_text(encoding="utf-8", errors="replace")
        details = result.stdout + result.stderr + log
        self.assertEqual(result.returncode, 0, details)
        output = self.work / "object files" / "sample.o"
        self.assertTrue(output.is_file(), details)
        self.assertGreater(output.stat().st_size, 0)
        for path in (outer, nested):
            self.assertIn("expanding response file " + path, log)
        if self.server:
            self.assertIn(" on {} completed ok".format(self.server), log)

    def test_forward_slash_paths(self):
        self.compile_response("/")

    @unittest.skipUnless(WINDOWS_PATHS, "需要 Windows 路径语义")
    def test_backslash_paths(self):
        self.compile_response("\\")

    @unittest.skipIf(WINDOWS_PATHS, "需要 GNU 响应文件引号规则")
    def test_gnu_escaped_space(self):
        self.compile_response("/", escaped_space=True)

    def test_empty_response_file(self):
        (self.work / "empty.rsp").touch()
        result = self.run_distcc("@empty.rsp")
        self.assertEqual(result.returncode, 101, result.stderr)
        self.assertIn("missing option/operand", result.stderr)


if __name__ == "__main__":
    unittest.main()
