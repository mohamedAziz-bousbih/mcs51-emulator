/*
 * testkit.h - minimal self-checking test harness for the C test programs.
 *
 * Each test program calls TK_RUN(test_fn) for its test functions and returns
 * tk_summary() from main(); a failed CHECK prints file, line and values but
 * lets the test continue, so one run reports every failure.
 */
#ifndef MCS51_TESTKIT_H
#define MCS51_TESTKIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "mcs51/board.h"
#include "mcs51/mcs51.h"

extern int tk_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            tk_failures++;                                                   \
        }                                                                    \
    } while (0)

#define CHECK_EQ(actual, expected)                                           \
    do {                                                                     \
        long long tk_a = (long long)(actual), tk_e = (long long)(expected);  \
        if (tk_a != tk_e) {                                                  \
            fprintf(stderr, "%s:%d: %s = %lld (0x%llX), expected %lld (0x%llX)\n", \
                    __FILE__, __LINE__, #actual, tk_a, (unsigned long long)tk_a, \
                    tk_e, (unsigned long long)tk_e);                         \
            tk_failures++;                                                   \
        }                                                                    \
    } while (0)

/* Stop a loop over many cases after the first failure so that an exhaustive
 * test does not print thousands of lines. */
#define CHECK_CASE(cond, ...)                                                \
    if (!(cond)) {                                                           \
        fprintf(stderr, "%s:%d: case failed: ", __FILE__, __LINE__);         \
        fprintf(stderr, __VA_ARGS__);                                        \
        fputc('\n', stderr);                                                 \
        tk_failures++;                                                       \
        return;                                                              \
    }

#define TK_RUN(fn) tk_run(#fn, fn)

void tk_run(const char *name, void (*fn)(void));
int tk_summary(void);

/* A freshly initialised CPU (shared static instance) with code at 0000h. */
mcs51 *tk_cpu(const uint8_t *code, size_t len);

/* Run until PC == addr (or max_steps instructions); returns false on timeout
 * or on a status other than MCS51_OK. */
bool tk_run_to(mcs51 *cpu, uint16_t addr, unsigned max_steps);

/* Execute one instruction at the current PC and return the machine cycles
 * it took. */
unsigned tk_step_cycles(mcs51 *cpu);

/* Path of a repository file (MCS51_SOURCE_DIR + "/" + relpath). */
const char *tk_path(const char *relpath);

/* Lab board (shared static instance) with an Intel HEX file loaded from the
 * repository; returns NULL (and records a failure) if loading fails. */
mcs51_board *tk_board(const char *relpath);

/* Parity of a byte (1 = odd number of ones). */
unsigned tk_parity(uint8_t v);

#endif /* MCS51_TESTKIT_H */
