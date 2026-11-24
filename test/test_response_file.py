"""测试 distcc 的响应文件展开和 Clang PCH 分发，可在 POSIX 和 Windows 上直接运行。

现有 comfychair 测试框架用 os.fork() 启动子进程，Windows 原生 Python
即使从 MSYS2 启动也没有这个接口。这组测试只需运行 distcc 并检查退出码、
日志和产物，因此用 unittest 和 subprocess.run()，无需另装 MSYS2 Python。

默认从 PATH 查找 distcc 和 cc。可用 DISTCC_RSP_TEST_CLIENT 和
DISTCC_RSP_TEST_COMPILER 指定可执行文件。默认使用 localhost；
设置 DISTCC_RSP_TEST_SERVER 可改用一个已启动的 TCP 服务端
（例如 127.0.0.1:42000,lzo），并验证编译确实在该端完成。
PCH 用例需要 Clang；Unity/PCH 分发用例必须指定服务端，不能用 localhost
代替远端验证。所有分发用例都关闭故障回退，以免本地成功掩盖远端失败。
"""

import json
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

    @unittest.skipUnless(WINDOWS_PATHS, "需要 Windows 响应文件引号规则")
    def test_cpp_string_macro_from_response_file(self):
        (self.work / "macro.cpp").write_text(
            "constexpr char name[] = RSP_NAME;\n"
            "static_assert(sizeof(name) == sizeof(\"Spatial.Foundation\"), \"name\");\n",
            encoding="ascii")
        (self.work / "macro args.rsp").write_text(
            r'-DRSP_NAME=\"Spatial.Foundation\" -c macro.cpp -o macro.o' + "\n",
            encoding="ascii")

        result = self.run_distcc(self.compiler, "@macro args.rsp")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.work / "macro.o").is_file())
        log = self.log.read_text(encoding="utf-8", errors="replace")
        self.assertIn("expanding response file macro args.rsp", log)
        self.assertIn("win32 command line:", log)
        if self.server:
            self.assertIn(" on {} completed ok".format(self.server), log)

    @unittest.skipUnless(os.name == "nt", "需要 Windows 原生 Python")
    def test_native_argument_round_trip(self):
        # 让原生子进程回传实际收到的参数，检查空参数、引号及反斜杠。
        expected = ["", "two words", 'a"b', 'a\\"b', 'a\\\\"b',
                    "path with space\\", "path with space\\\\", "plain\\path"]
        # 响应文件名带空格，使 MSYS 启动层将带引号的 @file 交给 distcc 展开。
        args = ["-c", "import json, sys; print(json.dumps(sys.argv[1:]))", *expected]
        (self.work / "native args.rsp").write_text(
            subprocess.list2cmdline(args) + "\n", encoding="ascii")
        result = self.run_distcc(Path(sys.executable).as_posix(), "@native args.rsp")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout), expected)

    @unittest.skipUnless(os.name == "nt", "需要从原生 Windows 进程启动 distcc")
    def test_ninja_style_unquoted_response_file(self):
        (self.work / "rsp").mkdir()
        (self.work / "rsp" / "flags.rsp").write_text(
            r'-DRSP_NAME=\"Spatial.Foundation\" -DRSP_TAIL=42' + "\n",
            encoding="ascii")
        (self.work / "sample.cpp").write_text(
            "constexpr char name[] = RSP_NAME;\n"
            "static_assert(sizeof(name) == sizeof(\"Spatial.Foundation\"), \"name\");\n"
            "static_assert(RSP_TAIL == 42, \"tail\");\n",
            encoding="ascii")

        # Python 不会给无空格的参数加引号，与 Ninja 的调用方式一致。
        result = self.run_distcc(self.compiler, r"@rsp\flags.rsp",
                                 "-o", "sample.o", "-c", "sample.cpp")
        log = self.log.read_text(encoding="utf-8", errors="replace")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr + log)
        self.assertIn(r"expanding response file rsp\flags.rsp", log)
        self.assertTrue((self.work / "sample.o").is_file())
        if self.server:
            self.assertIn(" on {} completed ok".format(self.server), log)

    def require_clang(self):
        version = subprocess.run([self.compiler, "--version"],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 text=True, timeout=10)
        if "clang" not in version.stdout.lower():
            self.skipTest("需要 Clang 的 -include-pch 支持")

    def test_pch_generation_stays_local(self):
        self.require_clang()
        (self.work / "prefix.cpp").write_text(
            "struct PchType { int value; };\n", encoding="ascii")
        # 即使没有可用远端且禁止回退，PCH 生成也应主动选择本地。
        self.env["DISTCC_HOSTS"] = "127.0.0.1:1/1"
        for language in ("c-header", "c++-header",
                         "objective-c-header", "objective-c++-header"):
            for joined in (False, True):
                with self.subTest(language=language, joined=joined):
                    self.log.unlink(missing_ok=True)
                    (self.work / "prefix.pch").unlink(missing_ok=True)
                    options = ["-x" + language] if joined else ["-x", language]
                    result = self.run_distcc(
                        self.compiler, *options, "-c", "prefix.cpp", "-o", "prefix.pch")
                    log = self.log.read_text(encoding="utf-8", errors="replace")
                    self.assertEqual(result.returncode, 0,
                                     result.stdout + result.stderr + log)
                    self.assertIn("precompiled header generation must be local", log)
                    self.assertGreater((self.work / "prefix.pch").stat().st_size, 0)

    def test_unity_source_with_pch_compiles_remotely(self):
        self.compile_unity_source_with_pch()

    def test_cmake_xclang_pch_compiles_remotely(self):
        self.compile_unity_source_with_pch(wrapped=True)

    def compile_unity_source_with_pch(self, wrapped=False):
        self.require_clang()
        if not self.server:
            self.skipTest("需要 DISTCC_RSP_TEST_SERVER 验证远端编译")
        (self.work / "pch files").mkdir()
        (self.work / "pch files" / "prefix.h").write_text(
            "struct PchType { int value; };\n"
            "#define PCH_VALUE 42\n", encoding="ascii")
        # .cpp 自身没有 include，模拟依赖强制引入头文件的 Unity 单元。
        (self.work / "first.cpp").write_text(
            "PchType first{PCH_VALUE};\n"
            + ("static_assert(EXTRA_VALUE == 7, \"include\");\n" if wrapped else ""),
            encoding="ascii")
        (self.work / "second.cpp").write_text(
            "int result() { return first.value; }\n", encoding="ascii")
        (self.work / "unity.cpp").write_text(
            '#include "first.cpp"\n#include "second.cpp"\n', encoding="ascii")
        result = self.run_distcc(
            self.compiler, "-x", "c++-header", "-c", "pch files/prefix.h",
            "-o", "pch files/cache.pch")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # 额外用 .cpp 后缀，验证扫描器不会把 PCH 路径误认成源文件。
        shutil.copyfile(self.work / "pch files/cache.pch",
                        self.work / "pch files/cache.cpp")
        (self.work / "pch files/extra.h").write_text(
            "#define EXTRA_VALUE 7\n", encoding="ascii")
        shutil.copyfile(self.work / "pch files/extra.h",
                        self.work / "pch files/extra.cpp")
        for pch in ("pch files/cache.pch", "pch files/cache.cpp"):
            with self.subTest(pch=pch):
                self.log.unlink()
                (self.work / "unity.o").unlink(missing_ok=True)
                options = '-include-pch "{}"'.format(pch)
                extra = "pch files/extra.cpp" if pch.endswith(".cpp") else "pch files/extra.h"
                if wrapped:
                    # 两组真实 CMake 形式，并额外保留一个无关的 cc1 选项。
                    options = (
                        '-Xclang -include-pch -Xclang "{}" '
                        '-Xclang -include -Xclang "{}" '
                        '-Xclang -fno-spell-checking').format(pch, extra)
                (self.work / "unity.rsp").write_text(
                    options + " -c unity.cpp -o unity.o\n", encoding="ascii")
                result = self.run_distcc(self.compiler, "@unity.rsp")
                log = self.log.read_text(encoding="utf-8", errors="replace")
                self.assertEqual(result.returncode, 0,
                                 result.stdout + result.stderr + log)
                self.assertIn(" on {} completed ok".format(self.server), log)
                remote_args = [line for line in log.splitlines()
                               if "(dcc_strip_local_args) result:" in line]
                self.assertEqual(len(remote_args), 1, log)
                self.assertNotIn("-include-pch", remote_args[0])
                self.assertNotIn(pch, remote_args[0])
                if wrapped:
                    self.assertNotIn(extra, remote_args[0])
                    self.assertNotIn("-include ", remote_args[0])
                    self.assertIn("-Xclang -fno-spell-checking", remote_args[0])
                    self.assertEqual(remote_args[0].split().count("-Xclang"), 1)
                self.assertGreater((self.work / "unity.o").stat().st_size, 0)


if __name__ == "__main__":
    unittest.main()
