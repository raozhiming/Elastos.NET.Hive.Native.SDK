/*
 * Copyright (c) 2019 Elastos Foundation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <stdbool.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#include <crystal.h>

#define MODE_LAUNCHER   0
#define MODE_CASES      1

static int mode = MODE_LAUNCHER;

int test_main(int argc, char *argv[]);
int launcher_main(int argc, char *argv[]);

#ifdef HAVE_SYS_RESOURCE_H
int sys_coredump_set(bool enable)
{
    const struct rlimit rlim = {
        enable ? RLIM_INFINITY : 0,
        enable ? RLIM_INFINITY : 0
    };

    return setrlimit(RLIMIT_CORE, &rlim);
}
#endif

static void usage(void)
{
    printf("Hive API unit tests.\n");
    printf("\n");
    printf("Usage: hivetests [OPTION]...\n");
    printf("\n");
    printf("First run options:\n");
    printf("      --cases               Run test cases only in manual mode.\n");
    printf("  -c, --config=CONFIG_FILE  Set config file path.\n");
    printf("\n");
    printf("Debugging options:\n");
    printf("      --debug               Wait for debugger attach after start.\n");
    printf("\n");
}

int main(int argc, char *argv[])
{
    int rc;
    char buffer[PATH_MAX];

    int opt;
    int idx;
    struct option options[] = {
        { "cases",     no_argument,       NULL, 1 },
        { "debug",     no_argument,       NULL, 2 },
        { "log-level", required_argument, NULL, 3 },
        { "log-file",  required_argument, NULL, 4 },
        { "data-dir",  required_argument, NULL, 5 },
        { "help",      no_argument,       NULL, 'h' },
        { "config",    required_argument, NULL, 'c' },
        { NULL,        0,                 NULL, 0 }
    };

#ifdef HAVE_SYS_RESOURCE_H
    sys_coredump_set(true);
#endif

    while ((opt = getopt_long(argc, argv, "r:c:h", options, &idx)) != -1) {
        switch (opt) {
        case 1:
            mode = MODE_CASES;
            break;

        case 2:
        case 3:
        case 4:
        case 5:
        case 'r':
        case 'c':
            break;

        default:
            usage();
            return -1;
        }
    }

    optind = 0;
    opterr = 0;
    optopt = 0;

    switch (mode) {
    case MODE_CASES:
        rc = test_main(argc, argv);
        break;

    case MODE_LAUNCHER:
        realpath(argv[0], buffer);
        argv[0] = buffer;
        rc = launcher_main(argc, argv);
        break;
    }

    return rc;
}
