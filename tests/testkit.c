/*
 * testkit.c - helpers shared by the C test programs.
 */
#include "testkit.h"

#include <string.h>

#include "mcs51/ihex.h"

int tk_failures;
static int tests_run, tests_failed;

void tk_run(const char *name, void (*fn)(void))
{
    int before = tk_failures;
    fn();
    tests_run++;
    if (tk_failures != before)
        tests_failed++;
    printf("%s %s\n", tk_failures == before ? "ok  " : "FAIL", name);
}

int tk_summary(void)
{
    printf("%d test%s, %d failed\n", tests_run, tests_run == 1 ? "" : "s", tests_failed);
    return tests_failed ? 1 : 0;
}

mcs51 *tk_cpu(const uint8_t *code, size_t len)
{
    static mcs51 cpu;
    mcs51_init(&cpu);
    mcs51_load_code(&cpu, 0, code, len);
    return &cpu;
}

bool tk_run_to(mcs51 *cpu, uint16_t addr, unsigned max_steps)
{
    for (unsigned i = 0; i < max_steps; i++) {
        if (cpu->pc == addr && cpu->irq.pending < 0 && !cpu->idle)
            return true;
        if (mcs51_step(cpu) != MCS51_OK)
            return false;
    }
    return false;
}

unsigned tk_step_cycles(mcs51 *cpu)
{
    uint64_t before = cpu->cycles;
    mcs51_step(cpu);
    return (unsigned)(cpu->cycles - before);
}

const char *tk_path(const char *relpath)
{
    static char path[1024];
    snprintf(path, sizeof path, "%s/%s", MCS51_SOURCE_DIR, relpath);
    return path;
}

mcs51_board *tk_board(const char *relpath)
{
    static mcs51_board board;
    if (!mcs51_board_init(&board)) {
        fprintf(stderr, "board memory map rejected\n");
        tk_failures++;
        return NULL;
    }
    mcs51_ihex_info info;
    if (mcs51_ihex_load_file(tk_path(relpath), board.cpu.code, MCS51_CODE_SIZE, &info) != 0) {
        fprintf(stderr, "%s: %s\n", tk_path(relpath), info.error);
        tk_failures++;
        return NULL;
    }
    return &board;
}

unsigned tk_parity(uint8_t v)
{
    unsigned p = 0;
    for (; v; v &= (uint8_t)(v - 1))
        p ^= 1u;
    return p;
}
