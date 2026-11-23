/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003, 2004 by Martin Pool <mbp@samba.org>
 * Copyright 2007 Google Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */


                /* "I have a bone to pick, and a few to break." */

/**
 * @file
 *
 * Functions for understanding and manipulating argument vectors.
 *
 * The few options explicitly handled by the client are processed in its
 * main().  At the moment, this is just --help and --version, so this function
 * never has to worry about them.
 *
 * We recognize two basic forms "distcc gcc ..." and "distcc ...", with no
 * explicit compiler name.  This second one is used if you have a Makefile
 * that can't manage two-word values for $CC; eventually it might support
 * putting a link to distcc on your path as 'gcc'.  We call this second one an
 * implicit compiler.
 *
 * We need to distinguish the two by working out whether the first argument
 * "looks like" a compiler name or not.  I think the two cases in which we
 * should assume it's implicit are "distcc -c hello.c" (starts with a hyphen),
 * and "distcc hello.c" (starts with a source filename.)
 *
 * In the case of implicit compilation "distcc --help" will always give you
 * distcc's help, not gcc's, and similarly for --version.  I don't see much
 * that we can do about that.
 *
 * @todo We don't need to run the full argument scanner on the server, only
 * something simple to recognize input and output files.  That would perhaps
 * make the function simpler, and also mean that if argument recognizer bugs
 * are fixed in the future, they only need to be fixed on the client, not on
 * the server.  An even better solution is to have the client tell the server
 * where to put the input and output files.
 *
 * @todo Perhaps make the argument parser driven by a data table.  (Would that
 * actually be clearer?)  Perhaps use regexps to recognize strings.
 *
 * @todo We could also detect options like "-x cpp-output" or "-x
 * assembler-with-cpp", because they should override language detection based
 * on extension.  I haven't seen anyone use them yet though.  In fact, since
 * we don't assemble remotely it is moot for the only reported case, the
 * Darwin C library.  We would also need to update the option when passing it
 * to the server.
 *
 * @todo Perhaps assume that assembly code will not use both #include and
 * .include, and therefore if we preprocess locally we can distribute the
 * compilation?  Assembling is so cheap that it's not necessarily worth
 * distributing.
 **/


#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>

#include <sys/stat.h>

#include "distcc.h"
#include "trace.h"
#include "util.h"
#include "exitcode.h"
#include "snprintf.h"


int dcc_argv_append(char **argv, char *toadd)
{
    int l = dcc_argv_len(argv);
    argv[l] = toadd;
    argv[l+1] = NULL;           /* just make sure */
    return 0;
}


/*
 * Response file (@file) expansion.
 *
 * Build systems commonly keep long option lists in a response file and
 * invoke the compiler as "cc @options.rsp".  distcc must see the real
 * options, because it has to inspect them (to decide whether the job can be
 * distributed) and modify them (to run the preprocessor and to rewrite the
 * output file).  If they stay hidden in the response file, the invocation
 * looks like a link step and is always run locally.
 *
 * libiberty's expandargv() can be used for this, but libiberty is optional
 * at configure time, and it applies gcc's quoting rules everywhere.  A
 * native Windows compiler (for example the Android NDK clang) normally uses
 * the MSVC rules, in which backslashes are path separators rather than
 * escapes.  Expanding the files ourselves keeps the behaviour the same for
 * every build of distcc and lets us follow the quoting rules of the
 * platform correctly.
 */

/* Compilers running on Windows use the MSVC rules; compilers elsewhere use
 * POSIX shell-like rules.  Distcc drives a compiler for the platform it was
 * built for, so select the rules from the build platform. */
#ifndef DCC_RSP_WINDOWS_RULES
#  if defined(__CYGWIN__) || defined(_WIN32) || defined(_MSC_VER)
#    define DCC_RSP_WINDOWS_RULES 1
#  else
#    define DCC_RSP_WINDOWS_RULES 0
#  endif
#endif

/* Limit on the nesting of response files, so that a malformed or hostile
 * file cannot exhaust the stack. */
#define DCC_RSP_MAX_DEPTH 16

struct dcc_rsp_argv {
    char **args;
    int nargs;
    int size;
};

static int dcc_rsp_isspace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r'
        || c == '\f' || c == '\v';
}

static int dcc_rsp_reserve(struct dcc_rsp_argv *list, int extra)
{
    char **grown;
    int size;

    if (list->nargs + extra + 1 <= list->size)
        return 0;

    size = list->size ? list->size : 32;
    while (size < list->nargs + extra + 1)
        size *= 2;

    grown = realloc(list->args, size * (sizeof list->args[0]));
    if (!grown) {
        rs_log_error("failed to allocate response file argument list");
        return EXIT_OUT_OF_MEMORY;
    }
    list->args = grown;
    list->size = size;
    return 0;
}

/* Add one argument, taking ownership of the string. */
static int dcc_rsp_add(struct dcc_rsp_argv *list, char *arg)
{
    int ret;

    if (!arg) {
        /* An empty quoted argument, as in "". */
        arg = strdup("");
        if (!arg) {
            rs_log_error("failed to allocate response file argument");
            return EXIT_OUT_OF_MEMORY;
        }
    }

    if ((ret = dcc_rsp_reserve(list, 1)) != 0) {
        free(arg);
        return ret;
    }
    list->args[list->nargs++] = arg;
    list->args[list->nargs] = NULL;
    return 0;
}

static int dcc_rsp_add_str(struct dcc_rsp_argv *list, const char *s)
{
    char *copy = strdup(s);

    if (!copy) {
        rs_log_error("failed to allocate response file argument");
        return EXIT_OUT_OF_MEMORY;
    }
    return dcc_rsp_add(list, copy);
}

static void dcc_rsp_clear(struct dcc_rsp_argv *list)
{
    int i;

    for (i = 0; i < list->nargs; i++)
        free(list->args[i]);
    free(list->args);
    list->args = NULL;
    list->nargs = list->size = 0;
}

/* Append n characters to a growable, NUL-terminated string buffer. */
static int dcc_rsp_putn(char **buf, size_t *len, size_t *size,
                        const char *s, size_t n)
{
    char *grown;

    if (*len + n + 1 > *size) {
        size_t newsize = *size ? *size : 64;
        while (newsize < *len + n + 1)
            newsize *= 2;
        grown = realloc(*buf, newsize);
        if (!grown) {
            rs_log_error("failed to allocate response file token");
            return EXIT_OUT_OF_MEMORY;
        }
        *buf = grown;
        *size = newsize;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

static int dcc_rsp_putc(char **buf, size_t *len, size_t *size, char c)
{
    return dcc_rsp_putn(buf, len, size, &c, 1);
}

#if DCC_RSP_WINDOWS_RULES
/* Split text using the MSVC command-line rules, which is what native
 * Windows compilers (cl.exe, clang.exe) use for response files:
 *
 *   - spaces, tabs and newlines separate arguments;
 *   - a double quote starts or ends a quoted run, and inside quotes spaces
 *     are literal;
 *   - two double quotes inside a quoted run mean one literal quote;
 *   - backslashes are literal, except that 2n backslashes in front of a
 *     double quote produce n backslashes and leave the quote meaningful,
 *     while 2n+1 backslashes produce n backslashes and a literal quote.
 */
static int dcc_rsp_tokenize(const char *text, size_t len,
                            struct dcc_rsp_argv *out)
{
    size_t i = 0;
    char *arg = NULL;
    int ret;

    while (i < len) {
        size_t arg_len = 0, arg_size = 0;
        int started = 0, in_quotes = 0;

        arg = NULL;
        while (i < len && dcc_rsp_isspace(text[i]))
            i++;
        if (i >= len)
            break;

        while (i < len) {
            char c = text[i];

            if (!in_quotes && dcc_rsp_isspace(c))
                break;

            if (c == '\\') {
                size_t start = i, nbs, k;

                while (i < len && text[i] == '\\')
                    i++;
                nbs = i - start;

                if (i < len && text[i] == '"') {
                    for (k = 0; k < nbs / 2; k++) {
                        if ((ret = dcc_rsp_putc(&arg, &arg_len, &arg_size,
                                                '\\')))
                            goto fail;
                    }
                    if (nbs % 2) {
                        if ((ret = dcc_rsp_putc(&arg, &arg_len, &arg_size,
                                                '"')))
                            goto fail;
                        i++;    /* literal quote consumed */
                    }
                    /* Even count: leave the quote to the '"' case below. */
                } else {
                    if ((ret = dcc_rsp_putn(&arg, &arg_len, &arg_size,
                                            text + start, nbs)))
                        goto fail;
                }
                started = 1;
                continue;
            }

            if (c == '"') {
                started = 1;
                if (!in_quotes) {
                    in_quotes = 1;
                    i++;
                } else if (i + 1 < len && text[i + 1] == '"') {
                    if ((ret = dcc_rsp_putc(&arg, &arg_len, &arg_size, '"')))
                        goto fail;
                    i += 2;
                } else {
                    in_quotes = 0;
                    i++;
                }
                continue;
            }

            if ((ret = dcc_rsp_putc(&arg, &arg_len, &arg_size, c)))
                goto fail;
            started = 1;
            i++;
        }

        if (started) {
            if ((ret = dcc_rsp_add(out, arg)) != 0)
                return ret;
        } else {
            free(arg);
        }
    }
    return 0;

  fail:
    free(arg);
    return ret;
}
#else
/* Split text using the quoting rules of gcc's response file parser:
 * whitespace separates arguments, backslash escapes the next character
 * outside quotes, single or double quotes group characters, and characters
 * between quotes (including backslashes) are taken literally.
 */
static int dcc_rsp_tokenize(const char *text, size_t len,
                            struct dcc_rsp_argv *out)
{
    size_t i = 0;
    char *arg = NULL;
    int ret;

    while (i < len) {
        size_t arg_len = 0, arg_size = 0;

        arg = NULL;
        while (i < len && dcc_rsp_isspace(text[i]))
            i++;
        if (i >= len)
            break;

        while (i < len && !dcc_rsp_isspace(text[i])) {
            char c = text[i];

            if (c == '\'') {
                i++;
                while (i < len && text[i] != '\'') {
                    if ((ret = dcc_rsp_putc(&arg, &arg_len, &arg_size,
                                            text[i])))
                        goto fail;
                    i++;
                }
                if (i < len)
                    i++;        /* closing quote */
            } else if (c == '"') {
                i++;
                while (i < len && text[i] != '"') {
                    if ((ret = dcc_rsp_putc(&arg, &arg_len, &arg_size,
                                            text[i])))
                        goto fail;
                    i++;
                }
                if (i < len)
                    i++;        /* closing quote */
            } else if (c == '\\' && i + 1 < len) {
                i++;
                if ((ret = dcc_rsp_putc(&arg, &arg_len, &arg_size, text[i])))
                    goto fail;
                i++;
            } else {
                if ((ret = dcc_rsp_putc(&arg, &arg_len, &arg_size, c)))
                    goto fail;
                i++;
            }
        }

        if ((ret = dcc_rsp_add(out, arg)) != 0)
            return ret;
    }
    return 0;

  fail:
    free(arg);
    return ret;
}
#endif

/* Read a response file into memory.  Returns 0 on success, 1 if the file
 * cannot be used and should be left for the compiler to deal with, or an
 * exit code on out-of-memory.
 */
static int dcc_rsp_read_file(const char *path, char **text_ret, size_t *len_ret)
{
    FILE *f;
    char *buf = NULL;
    size_t len = 0, size = 0;

    f = fopen(path, "rb");
    if (!f)
        return 1;

    for (;;) {
        size_t n;

        if (len + 4096 + 1 > size) {
            size_t newsize = size ? size * 2 : 8192;
            char *grown;

            while (newsize < len + 4096 + 1)
                newsize *= 2;
            grown = realloc(buf, newsize);
            if (!grown) {
                rs_log_error("failed to allocate buffer for response file %s",
                             path);
                free(buf);
                fclose(f);
                return EXIT_OUT_OF_MEMORY;
            }
            buf = grown;
            size = newsize;
        }

        n = fread(buf + len, 1, size - len - 1, f);
        len += n;
        if (n == 0) {
            if (ferror(f)) {
                free(buf);
                fclose(f);
                return 1;
            }
            break;
        }
    }
    fclose(f);

    buf[len] = '\0';

    /* Skip a UTF-8 byte order mark. */
    if (len >= 3 && (unsigned char) buf[0] == 0xef
        && (unsigned char) buf[1] == 0xbb
        && (unsigned char) buf[2] == 0xbf) {
        memmove(buf, buf + 3, len - 3 + 1);
        len -= 3;
    }

    /* UTF-16 files cannot be shipped to the remote side meaningfully; let
     * the local compiler read them itself. */
    if (len >= 2 && (((unsigned char) buf[0] == 0xff
                      && (unsigned char) buf[1] == 0xfe)
                     || ((unsigned char) buf[0] == 0xfe
                         && (unsigned char) buf[1] == 0xff))) {
        free(buf);
        return 1;
    }

    *text_ret = buf;
    *len_ret = len;
    return 0;
}

static int dcc_rsp_expand_argv(char **in, struct dcc_rsp_argv *out,
                               const char **stack, int depth)
{
    int i, ret;

    if (!in)
        return 0;

    for (i = 0; in[i]; i++) {
        char *a = in[i];
        struct dcc_rsp_argv sub;
        char *text = NULL;
        size_t len = 0;
        int k, cycle = 0;

        /* "@@file" is an escape: do not expand it.  A lone "@" is an
         * ordinary argument. */
        if (a[0] != '@' || a[1] == '\0' || a[1] == '@') {
            if ((ret = dcc_rsp_add_str(out, a)) != 0)
                return ret;
            continue;
        }

        if (depth >= DCC_RSP_MAX_DEPTH) {
            rs_log_warning("response file %s nested too deeply; "
                           "passing it to the compiler", a + 1);
            if ((ret = dcc_rsp_add_str(out, a)) != 0)
                return ret;
            continue;
        }

        for (k = 0; k < depth; k++) {
            if (strcmp(stack[k], a + 1) == 0) {
                cycle = 1;
                break;
            }
        }
        if (cycle) {
            rs_log_warning("response file %s includes itself; "
                           "passing it to the compiler", a + 1);
            if ((ret = dcc_rsp_add_str(out, a)) != 0)
                return ret;
            continue;
        }

        ret = dcc_rsp_read_file(a + 1, &text, &len);
        if (ret == 1) {
            /* Unreadable (or unsupported encoding): keep the compiler's
             * normal behaviour for this argument. */
            rs_trace("cannot expand response file %s; passing it through",
                     a + 1);
            if ((ret = dcc_rsp_add_str(out, a)) != 0)
                return ret;
            continue;
        } else if (ret != 0) {
            return ret;
        }

        rs_trace("expanding response file %s", a + 1);

        memset(&sub, 0, sizeof sub);
        ret = dcc_rsp_tokenize(text, len, &sub);
        free(text);

        if (ret == 0) {
            stack[depth] = a + 1;
            ret = dcc_rsp_expand_argv(sub.args, out, stack, depth + 1);
        }
        dcc_rsp_clear(&sub);
        if (ret != 0)
            return ret;
    }

    return 0;
}

/**
 * Expand @file arguments into the arguments contained in those files.
 *
 * This is used by the client before it looks at the command line, so that
 * the options hidden in response files take part in the normal distcc
 * argument scanning and can be distributed to a remote server.
 *
 * Arguments that are not response files, and response files that cannot be
 * read, are passed through unchanged so that a local compilation behaves
 * exactly like a plain compiler invocation.
 *
 * On success *argv_ptr is replaced by a newly allocated array of newly
 * allocated strings; the original array is not modified or freed.
 *
 * @returns 0 on success, or EXIT_OUT_OF_MEMORY.
 **/
int dcc_expand_response_files(char ***argv_ptr)
{
    struct dcc_rsp_argv out;
    const char *stack[DCC_RSP_MAX_DEPTH];
    int ret;

    memset(&out, 0, sizeof out);

    ret = dcc_rsp_add_str(&out, (*argv_ptr)[0]);
    if (ret == 0)
        ret = dcc_rsp_expand_argv(*argv_ptr + 1, &out, stack, 0);

    if (ret != 0) {
        dcc_rsp_clear(&out);
        return ret;
    }

    rs_trace("expanded response files into %d arguments", out.nargs);
    *argv_ptr = out.args;
    return 0;
}

static void dcc_note_compiled(const char *input_file, const char *output_file)
{
    const char *input_base, *output_base;

    input_base = dcc_find_basename(input_file);
    output_base = dcc_find_basename(output_file);

    rs_log(RS_LOG_INFO|RS_LOG_NONAME,
           "compile from %s to %s", input_base, output_base);
}

/**
 * Parse arguments, extract ones we care about, and also work out
 * whether it will be possible to distribute this invocation remotely.
 *
 * This is a little hard because the cc argument rules are pretty complex, but
 * the function still ought to be simpler than it already is.
 *
 * This code is called on both the client and the server, though they use the
 * results differently.
 *
 * This function makes a copy of the arguments, modified to ensure that
 * the arguments include '-o <filename>'.  This is returned in *ret_newargv.
 * The copy is dynamically allocated and the caller is responsible for
 * deallocating it.
 *
 * @returns 0 if it's ok to distribute this compilation, or an error code.
 **/
int dcc_scan_args(char *argv[], char **input_file, char **output_file,
                  char ***ret_newargv)
{
    int seen_opt_c = 0, seen_opt_s = 0;
    int i;
    char *a;
    int ret;

     /* allow for -o foo.o */
    if ((ret = dcc_copy_argv(argv, ret_newargv, 4)) != 0)
        return ret;
    argv = *ret_newargv;

    /* FIXME: new copy of argv is leaked */

    dcc_trace_argv("scanning arguments", argv);

    /* Things like "distcc -c hello.c" with an implied compiler are
     * handled earlier on by inserting a compiler name.  At this
     * point, argv[0] should always be a compiler name. */
    if (argv[0][0] == '-') {
        rs_log_error("unrecognized distcc option: %s", argv[0]);
        exit(EXIT_BAD_ARGUMENTS);
    }

    *input_file = *output_file = NULL;

    for (i = 0; (a = argv[i]); i++) {
        if (a[0] == '-') {
            if (!strcmp(a, "-E")) {
                rs_trace("-E call for cpp must be local");
                return EXIT_LOCAL_CPP;
            } else if (!strcmp(a, "-MD") || !strcmp(a, "-MMD")) {
                /* These two generate dependencies as a side effect.  They
                 * should work with the way we call cpp. */
            } else if (!strcmp(a, "-MG") || !strcmp(a, "-MP")) {
                /* These just modify the behaviour of other -M* options and do
                 * nothing by themselves. */
            } else if (!strcmp(a, "-MF") || !strcmp(a, "-MT") ||
                       !strcmp(a, "-MQ")) {
                /* As above but with extra argument. */
                i++;
            } else if (!strncmp(a, "-MF", 3) || !strncmp(a, "-MT", 3) ||
                       !strncmp(a, "-MQ", 3)) {
                /* As above, without extra argument. */
            } else if (a[1] == 'M') {
                /* -M(anything else) causes the preprocessor to
                    produce a list of make-style dependencies on
                    header files, either to stdout or to a local file.
                    It implies -E, so only the preprocessor is run,
                    not the compiler.  There would be no point trying
                    to distribute it even if we could. */
                rs_trace("%s implies -E (maybe) and must be local", a);
                return EXIT_DISTCC_FAILED;
            } else if (!strcmp(a, "-march=native")) {
                rs_trace("-march=native generates code for local machine; "
                         "must be local");
                return EXIT_DISTCC_FAILED;
            } else if (!strcmp(a, "-mtune=native")) {
                rs_trace("-mtune=native optimizes for local machine; "
                         "must be local");
                return EXIT_DISTCC_FAILED;
            } else if (!strcmp(a, "-flto")) {
                rs_trace("LTO cc invocations are not worth distributing");
                return EXIT_DISTCC_FAILED;
            } else if (str_startswith("-Wa,", a)) {
                /* Look for assembler options that would produce output
                 * files and must be local.
                 *
                 * Writing listings to stdout could be supported but it might
                 * be hard to parse reliably. */
                if (strstr(a, ",-a") || strstr(a, "--MD")) {
                    rs_trace("%s must be local", a);
                    return EXIT_DISTCC_FAILED;
                }
            } else if (str_startswith("-specs=", a)) {
                rs_trace("%s must be local", a);
                return EXIT_DISTCC_FAILED;
            } else if (!strcmp(a, "-S")) {
                seen_opt_s = 1;
            } else if (!strcmp(a, "-fprofile-arcs")
                       || !strcmp(a, "-ftest-coverage")
		       || !strcmp(a, "--coverage")
		       || !strncmp(a, "-fprofile-generate", 18) /* it also has an -fprofile-generate=<path> form */
		       || !strncmp(a, "-fprofile-use", 13)
		       || !strncmp(a, "-fauto-profile", 14)
		       || !strcmp(a, "-fprofile-correction")) {
                rs_log_info("compiler will emit/use profile info; must be local");
                return EXIT_DISTCC_FAILED;
            } else if (!strcmp(a, "-frepo")) {
                rs_log_info("compiler will emit .rpo files; must be local");
                return EXIT_DISTCC_FAILED;
            } else if (str_startswith("-x", a)
                       && (str_endswith("-header", a + 2)
                           || (!strcmp(a, "-x") && argv[i+1]
                               && str_endswith("-header", argv[i+1])))) {
                /* PCH 是本地编译器的产物，不能发给其他主机生成。 */
                rs_log_info("precompiled header generation must be local");
                return EXIT_DISTCC_FAILED;
            } else if (!strcmp(a, "-include-pch")) {
                /* PCH 路径是选项的值，不能被当成第二个源文件。 */
                if (!argv[i+1]) {
                    rs_log_info("-include-pch requires a path; running locally");
                    return EXIT_DISTCC_FAILED;
                }
                i++;
            } else if (str_startswith("-x", a)
                       && argv[i+1]
                       && !str_startswith("c", argv[i+1])
                       && !str_startswith("c++", argv[i+1])
                       && !str_startswith("objective-c", argv[i+1])
                       && !str_startswith("objective-c++", argv[i+1])
                       && !str_startswith("go", argv[i+1])
                       ) {
                rs_log_info("gcc's -x handling is complex; running locally for %s", argv[i+1] ? argv[i+1] : "empty");
                return EXIT_DISTCC_FAILED;
            } else if (str_startswith("-dr", a)) {
                rs_log_info("gcc's debug option %s may write extra files; "
                            "running locally", a);
                return EXIT_DISTCC_FAILED;
            } else if (!strcmp(a, "-c")) {
                seen_opt_c = 1;
            } else if (!strcmp(a, "-o")) {
                /* Whatever follows must be the output */
                a = argv[++i];
                goto GOT_OUTPUT;
            } else if (str_startswith("-o", a)) {
                a += 2;         /* skip "-o" */
                goto GOT_OUTPUT;
            }
        } else {
            if (dcc_is_source(a)) {
                rs_trace("found input file \"%s\"", a);
                if (*input_file) {
                    rs_log_info("do we have two inputs?  i give up");
                    return EXIT_DISTCC_FAILED;
                }
                *input_file = a;
            } else if (str_endswith(".o", a)) {
              GOT_OUTPUT:
                rs_trace("found object/output file \"%s\"", a);
                if (*output_file) {
                    rs_log_info("called for link?  i give up");
                    return EXIT_DISTCC_FAILED;
                }
                *output_file = a;
            }
        }
    }

    /* TODO: ccache has the heuristic of ignoring arguments that are not
     * extant files when looking for the input file; that's possibly
     * worthwhile.  Of course we can't do that on the server. */

    if (!seen_opt_c && !seen_opt_s) {
        rs_log_info("compiler apparently called not for compile");
        return EXIT_DISTCC_FAILED;
    }

    if (!*input_file) {
        rs_log_info("no visible input file");
        return EXIT_DISTCC_FAILED;
    }

    if (dcc_source_needs_local(*input_file))
        return EXIT_DISTCC_FAILED;

    if (!*output_file) {
        /* This is a commandline like "gcc -c hello.c".  They want
         * hello.o, but they don't say so.  For example, the Ethereal
         * makefile does this.
         *
         * Note: this doesn't handle a.out, the other implied
         * filename, but that doesn't matter because it would already
         * be excluded by not having -c or -S.
         */
        char *ofile;

        /* -S takes precedence over -c, because it means "stop after
         * preprocessing" rather than "stop after compilation." */
        if (seen_opt_s) {
            if (dcc_output_from_source(*input_file, ".s", &ofile))
                return EXIT_DISTCC_FAILED;
        } else if (seen_opt_c) {
            if (dcc_output_from_source(*input_file, ".o", &ofile))
                return EXIT_DISTCC_FAILED;
        } else {
            rs_log_crit("this can't be happening(%d)!", __LINE__);
            return EXIT_DISTCC_FAILED;
        }
        rs_log_info("no visible output file, going to add \"-o %s\" at end",
                      ofile);
        dcc_argv_append(argv, strdup("-o"));
        dcc_argv_append(argv, ofile);
        *output_file = ofile;
    }

    dcc_note_compiled(*input_file, *output_file);

    if (strcmp(*output_file, "-") == 0) {
        /* Different compilers may treat "-o -" as either "write to
         * stdout", or "write to a file called '-'".  We can't know,
         * so we just always run it locally.  Hopefully this is a
         * pretty rare case. */
        rs_log_info("output to stdout?  running locally");
        return EXIT_DISTCC_FAILED;
    }

    return 0;
}



/**
 * Used to change "-c" or "-S" to "-E", so that we get preprocessed
 * source.
 **/
int dcc_set_action_opt(char **a, const char *new_c)
{
    int gotone = 0;

    for (; *a; a++)
        if (!strcmp(*a, "-c") || !strcmp(*a, "-S")) {
            *a = strdup(new_c);
            if (*a == NULL) {
                rs_log_error("strdup failed");
                exit(EXIT_OUT_OF_MEMORY);
            }
            gotone = 1;
            /* keep going; it's not impossible they wrote "gcc -c -c
             * -c hello.c" */
        }

    if (!gotone) {
        rs_log_error("failed to find -c or -S");
        return EXIT_DISTCC_FAILED;
    } else {
        return 0;
    }
}



/**
 * Change object file or suffix of -o to @p ofname
 * Frees the old value, if it exists.
 *
 * It's crucially important that in every case where an output file is
 * detected by dcc_scan_args(), it's also correctly identified here.
 * It might be better to make the code shared.
 **/
int dcc_set_output(char **a, char *ofname)
{
    int i;

    for (i = 0; a[i]; i++)
        if (0 == strcmp(a[i], "-o") && a[i+1] != NULL) {
            rs_trace("changed output from \"%s\" to \"%s\"", a[i+1], ofname);
            free(a[i+1]);
            a[i+1] = strdup(ofname);
            if (a[i+1] == NULL) {
                rs_log_crit("failed to allocate space for output parameter");
                return EXIT_OUT_OF_MEMORY;
            }
            dcc_trace_argv("command after", a);
            return 0;
        } else if (0 == strncmp(a[i], "-o", 2)) {
            char *newptr;
            rs_trace("changed output from \"%s\" to \"%s\"", a[i]+2, ofname);
            free(a[i]);
            if (asprintf(&newptr, "-o%s", ofname) == -1) {
                rs_log_crit("failed to allocate space for output parameter");
                return EXIT_OUT_OF_MEMORY;
            }
            a[i] = newptr;
            dcc_trace_argv("command after", a);
            return 0;
        }

    rs_log_error("failed to find \"-o\"");
    return EXIT_DISTCC_FAILED;
}

/**
 * Change input file to a copy of @p ifname; called on compiler.
 * Frees the old value.
 *
 * @todo Unify this with dcc_scan_args
 *
 * @todo Test this by making sure that when the modified arguments are
 * run through scan_args, the new ifname is identified as the input.
 **/
int dcc_set_input(char **a, char *ifname)
{
    int i;

    for (i =0; a[i]; i++)
        if (dcc_is_source(a[i])) {
            rs_trace("changed input from \"%s\" to \"%s\"", a[i], ifname);
            free(a[i]);
            a[i] = strdup(ifname);
            if (a[i] == NULL) {
                rs_log_crit("failed to allocate space for input parameter");
                return EXIT_OUT_OF_MEMORY;
            }
            dcc_trace_argv("command after", a);
            return 0;
        }

    rs_log_error("failed to find input file");
    return EXIT_DISTCC_FAILED;
}

/* Subroutine of dcc_expand_preprocessor_options().
 * Calculate how many extra arguments we'll need to convert
 * a "-Wp,..." option into regular gcc options.
 * Returns the number of extra arguments needed.
 */
static int count_extra_args(char *dash_Wp_option) {
    int extra_args = 0;
    char *comma = dash_Wp_option + strlen("-Wp");
    while (comma != NULL) {
        char *opt = comma + 1;
        comma = strchr(opt, ',');
        if (str_startswith("-MD,", opt) ||
            str_startswith("-MMD,", opt))
        {
            char *filename = comma + 1;
            comma = strchr(filename, ',');
            extra_args += 3;  /* "-MD", "-MF", filename. */
        } else {
            extra_args++;
        }
    }
    return extra_args;
}

/* Subroutine of dcc_expand_preprocessor_options().
 * Convert a "-Wp,..." option into one or more regular gcc options.
 * Copy the resulting gcc options to dest_argv, which should be
 * pre-allocated by the caller.
 * Destructively modifies dash_Wp_option as it goes.
 * Returns 0 on success, nonzero for error (out of memory).
 */
static int copy_extra_args(char **dest_argv, char *dash_Wp_option,
                           int extra_args) {
    int i = 0;
    char *comma = dash_Wp_option + strlen("-Wp");
    while (comma != NULL) {
        char *opt = comma + 1;
        comma = strchr(opt, ',');
        if (comma) *comma = '\0';
        dest_argv[i] = strdup(opt);
        if (!dest_argv[i]) return EXIT_OUT_OF_MEMORY;
        i++;
        if (strcmp(opt, "-MD") == 0 || strcmp(opt, "-MMD") == 0) {
            char *filename;
            if (!comma) {
                rs_log_warning("'-Wp,-MD' or '-Wp,-MMD' option is missing "
                               "filename argument");
                break;
            }
            filename = comma + 1;
            comma = strchr(filename, ',');
            if (comma) *comma = '\0';
            dest_argv[i] = strdup("-MF");
            if (!dest_argv[i]) return EXIT_OUT_OF_MEMORY;
            i++;
            dest_argv[i] = strdup(filename);
            if (!dest_argv[i]) return EXIT_OUT_OF_MEMORY;
            i++;
        }
    }
    assert(i == extra_args);
    return 0;
}


/*
 * Convert any "-Wp," options into regular gcc options.
 * We do this because it simplifies the command-line
 * option handling elsewhere; this is the only place
 * that needs to parse "-Wp," options.
 * Returns 0 on success, nonzero for error (out of memory).
 *
 * The argv array pointed to by argv_ptr when this function
 * is called must have been dynamically allocated.  It remains
 * the caller's responsibility to deallocate it.
 */
int dcc_expand_preprocessor_options(char ***argv_ptr) {
    int i, j, ret;
    char **argv = *argv_ptr;
    char **new_argv;
    int argc = dcc_argv_len(argv);
    for (i = 0; argv[i]; i++) {
        if (str_startswith("-Wp,", argv[i])) {
            /* First, calculate how many extra arguments we'll need. */
            int extra_args = count_extra_args(argv[i]);
            assert(extra_args >= 1);

            new_argv = calloc(argc + extra_args, sizeof(char *));
            if (!new_argv) {
                return EXIT_OUT_OF_MEMORY;
            }
            for (j = 0; j < i; j++) {
                new_argv[j] = argv[j];
            }
            if ((ret = copy_extra_args(new_argv + i, argv[i],
                                       extra_args)) != 0) {
                free(new_argv);
                return ret;
            }
            for (j = i + 1; j <= argc; j++) {
                new_argv[j + extra_args - 1] = argv[j];
            }
            free(argv);
            *argv_ptr = argv = new_argv;
            i += extra_args - 1;
            argc += extra_args - 1;
        }
    }
    return 0;
}
