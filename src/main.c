/*
 * main.c - mcs51 command line: run, disassemble or inspect programs on the
 * emulated lab board.
 *
 *   mcs51 run prog.hex [--max-cycles N] [--clock HZ] [--trace] [--ports] [--rx TEXT]
 *   mcs51 disasm prog.hex
 *   mcs51 map
 *
 * Serial output of the program goes to stdout, everything else to stderr.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcs51/board.h"
#include "mcs51/ihex.h"
#include "mcs51/mcs51.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#define DEFAULT_MAX_CYCLES 100000000ull
#define DEFAULT_CLOCK 11059200.0

enum { EXIT_STOPPED = 0, EXIT_ERROR = 1, EXIT_ILLEGAL = 2 };

typedef struct options {
    const char *path;
    uint64_t max_cycles;
    double clock;
    bool trace;
    bool ports;
    const char *rx;
} options;

typedef struct monitor {
    mcs51_board *board;
    bool trace, ports;
    uint8_t port_last[4];
} monitor;

static void usage(FILE *f)
{
    fputs("usage: mcs51 run <program.hex> [options]\n"
          "       mcs51 disasm <program.hex>\n"
          "       mcs51 map\n"
          "\n"
          "run options:\n"
          "  --max-cycles N  stop after N machine cycles (default 100000000)\n"
          "  --clock HZ      oscillator frequency used for time reports (default 11059200)\n"
          "  --trace         print every instruction, interrupt, port change and MOVX\n"
          "                  access to stderr\n"
          "  --ports         print port, 8255 and latch output changes to stderr\n"
          "  --rx TEXT       bytes for the serial receiver; escapes \\r \\n \\t \\\\ \\xHH\n"
          "\n"
          "Serial output goes to stdout. Exit status: 0 halted, power down or cycle\n"
          "limit; 1 usage or load error; 2 illegal opcode.\n",
          f);
}

static bool parse_u64(const char *s, uint64_t *out)
{
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || end == s || *end || s[0] == '-')
        return false;
    *out = v;
    return true;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Queue --rx text on the serial line, expanding C-style escapes. */
static bool inject_text(mcs51 *cpu, const char *s)
{
    while (*s) {
        unsigned c = (unsigned char)*s++;
        if (c == '\\') {
            char e = *s ? *s++ : '\\';
            switch (e) {
            case 'r': c = '\r'; break;
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case '\\': c = '\\'; break;
            case 'x': {
                int hi = hex_digit(s[0]), lo = hi < 0 ? -1 : hex_digit(s[1]);
                if (lo < 0)
                    return false;
                c = (unsigned)(hi << 4 | lo);
                s += 2;
                break;
            }
            default:
                return false;
            }
        }
        if (!mcs51_serial_inject(cpu, (uint16_t)c))
            return false;
    }
    return true;
}

static bool load_program(mcs51 *cpu, const char *path, mcs51_ihex_info *info)
{
    if (mcs51_ihex_load_file(path, cpu->code, MCS51_CODE_SIZE, info) != 0) {
        fprintf(stderr, "mcs51: %s: %s\n", path, info->error);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------- monitoring */

static void on_serial(void *ctx, uint16_t data)
{
    (void)ctx;
    putchar(data & 0xFF);
    fflush(stdout);
}

static void on_port(void *ctx, unsigned port, uint8_t latch)
{
    monitor *m = ctx;
    if (latch == m->port_last[port])
        return;
    m->port_last[port] = latch;
    if (m->trace || m->ports)
        fprintf(stderr, "[%10" PRIu64 "] P%u = %02Xh\n", m->board->cpu.cycles, port, latch);
}

static void on_ppi(void *ctx, unsigned port, uint8_t value)
{
    monitor *m = ctx;
    fprintf(stderr, "[%10" PRIu64 "] PPI port %c = %02Xh\n", m->board->cpu.cycles, 'A' + port, value);
}

static void on_bus(void *ctx, uint16_t addr, uint8_t value, bool write, const mcs51_xbus_device *dev)
{
    monitor *m = ctx;
    if (m->trace)
        fprintf(stderr, "[%10" PRIu64 "] MOVX %s %04Xh = %02Xh  (%s)\n", m->board->cpu.cycles,
                write ? "write" : "read ", addr, value, dev ? dev->name : "not decoded");
    else if (write && dev && dev->read == NULL)
        fprintf(stderr, "[%10" PRIu64 "] %s = %02Xh\n", m->board->cpu.cycles, dev->name, value);
}

static const char *irq_name(int source)
{
    static const char *const names[MCS51_IRQ_COUNT] = {"EX0", "T0", "EX1", "T1", "serial"};
    return names[source];
}

static void trace_step(const mcs51 *cpu, bool *was_idle)
{
    if (cpu->irq.pending >= 0) {
        fprintf(stderr, "[%10" PRIu64 "] interrupt %s -> %04Xh\n", cpu->cycles,
                irq_name(cpu->irq.pending), 8u * (unsigned)cpu->irq.pending + 3u);
        *was_idle = false;
        return;
    }
    if (cpu->idle) {
        if (!*was_idle)
            fprintf(stderr, "[%10" PRIu64 "] idle\n", cpu->cycles);
        *was_idle = true;
        return;
    }
    *was_idle = false;

    char text[48], bytes[16] = "";
    unsigned len = mcs51_disasm(cpu->code, cpu->pc, text, sizeof text);
    for (unsigned i = 0; i < len; i++)
        snprintf(bytes + 3 * i, sizeof bytes - 3 * i, "%02X ", cpu->code[(uint16_t)(cpu->pc + i)]);
    fprintf(stderr, "[%10" PRIu64 "] %04X  %-9s %-26s A=%02X PSW=%02X SP=%02X\n", cpu->cycles,
            cpu->pc, bytes, text, MCS51_SFR(cpu, MCS51_ACC), MCS51_SFR(cpu, MCS51_PSW),
            MCS51_SFR(cpu, MCS51_SP));
}

static void print_state(mcs51_board *board, mcs51_status status, const options *opt)
{
    mcs51 *cpu = &board->cpu;
    double seconds = (double)cpu->cycles * MCS51_CLOCKS_PER_CYCLE / opt->clock;
    fprintf(stderr, "mcs51: %s after %" PRIu64 " machine cycles (%.6f s at %.6f MHz)\n",
            mcs51_status_name(status), cpu->cycles, seconds, opt->clock / 1e6);
    fprintf(stderr, "PC=%04Xh A=%02Xh B=%02Xh PSW=%02Xh SP=%02Xh DPTR=%02X%02Xh\n", cpu->pc,
            MCS51_SFR(cpu, MCS51_ACC), MCS51_SFR(cpu, MCS51_B), MCS51_SFR(cpu, MCS51_PSW),
            MCS51_SFR(cpu, MCS51_SP), MCS51_SFR(cpu, MCS51_DPH), MCS51_SFR(cpu, MCS51_DPL));
    fprintf(stderr, "R0-R7:");
    for (unsigned i = 0; i < 8; i++)
        fprintf(stderr, " %02X", mcs51_reg(cpu, i));
    fprintf(stderr, "\nP0=%02Xh P1=%02Xh P2=%02Xh P3=%02Xh  PPI A=%02Xh B=%02Xh C=%02Xh  latch=%02Xh\n",
            MCS51_SFR(cpu, MCS51_P0), MCS51_SFR(cpu, MCS51_P1), MCS51_SFR(cpu, MCS51_P2),
            MCS51_SFR(cpu, MCS51_P3), mcs51_ppi_port(&board->ppi, MCS51_PPI_A),
            mcs51_ppi_port(&board->ppi, MCS51_PPI_B), mcs51_ppi_port(&board->ppi, MCS51_PPI_C),
            board->latch);
}

/* --------------------------------------------------------------- commands */

static int cmd_run(const options *opt)
{
    static mcs51_board board;
    if (!mcs51_board_init(&board)) {
        fputs("mcs51: invalid board memory map\n", stderr);
        return EXIT_ERROR;
    }
    mcs51 *cpu = &board.cpu;
    mcs51_ihex_info info;
    if (!load_program(cpu, opt->path, &info))
        return EXIT_ERROR;
    if (opt->rx && !inject_text(cpu, opt->rx)) {
        fputs("mcs51: --rx: invalid escape sequence or text too long\n", stderr);
        return EXIT_ERROR;
    }

    monitor mon = {&board, opt->trace, opt->ports, {0xFF, 0xFF, 0xFF, 0xFF}};
    cpu->io.serial_tx = on_serial;
    cpu->io.port_out = on_port;
    cpu->io.port_ctx = &mon;
    if (opt->trace || opt->ports) {
        board.ppi.port_out = on_ppi;
        board.ppi.ctx = &mon;
        board.bus.monitor = on_bus;
        board.bus.monitor_ctx = &mon;
    }

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY); /* serial bytes go out unchanged */
#endif
    mcs51_status status = MCS51_CYCLE_LIMIT;
    bool was_idle = false;
    while (cpu->cycles < opt->max_cycles) {
        if (opt->trace)
            trace_step(cpu, &was_idle);
        status = mcs51_step(cpu);
        if (status != MCS51_OK)
            break;
        status = MCS51_CYCLE_LIMIT;
    }
    fflush(stdout);
    if (status == MCS51_ILLEGAL)
        fprintf(stderr, "mcs51: reserved opcode %02Xh at %04Xh\n", cpu->code[cpu->pc], cpu->pc);
    print_state(&board, status, opt);
    return status == MCS51_ILLEGAL ? EXIT_ILLEGAL : EXIT_STOPPED;
}

static int cmd_disasm(const char *path)
{
    static mcs51 cpu;
    mcs51_init(&cpu);
    mcs51_ihex_info info;
    if (!load_program(&cpu, path, &info))
        return EXIT_ERROR;
    if (info.bytes == 0)
        return EXIT_STOPPED;
    uint32_t addr = info.lowest;
    while (addr <= info.highest) {
        char text[48], bytes[16] = "";
        unsigned len = mcs51_disasm(cpu.code, (uint16_t)addr, text, sizeof text);
        for (unsigned i = 0; i < len; i++)
            snprintf(bytes + 3 * i, sizeof bytes - 3 * i, "%02X ", cpu.code[(uint16_t)(addr + i)]);
        printf("%04X  %-9s %s\n", (unsigned)addr, bytes, text);
        addr += len;
    }
    return EXIT_STOPPED;
}

static int cmd_map(void)
{
    static mcs51_board board;
    if (!mcs51_board_init(&board)) {
        fputs("mcs51: invalid board memory map\n", stderr);
        return EXIT_ERROR;
    }
    printf("%-13s%-16s%-18s%s\n", "range", "device", "A15...........A0", "mirrors");
    for (unsigned i = 0; i < board.bus.count; i++) {
        const mcs51_xbus_device *d = &board.bus.dev[i];
        char lines[17];
        for (int bit = 15; bit >= 0; bit--) {
            uint16_t line = (uint16_t)(1u << bit);
            lines[15 - bit] = (d->mask & line) ? ((d->match & line) ? '1' : '0')
                              : (d->local_mask & line) ? 'a' : 'x';
        }
        lines[16] = '\0';
        printf("%04Xh-%04Xh  %-14s  %s  %" PRIu32 "\n", mcs51_xbus_first(d),
               mcs51_xbus_last(d), d->name, lines, mcs51_xbus_mirrors(d));
    }
    printf("1/0 = decoded address line, a = line wired to the device, x = not connected\n"
           "(each x doubles the number of mirror addresses); other addresses read FFh\n");
    return EXIT_STOPPED;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return EXIT_ERROR;
    }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help")) {
        usage(stdout);
        return EXIT_STOPPED;
    }
    if (!strcmp(cmd, "map"))
        return argc == 2 ? cmd_map() : (usage(stderr), EXIT_ERROR);
    if (!strcmp(cmd, "disasm"))
        return argc == 3 ? cmd_disasm(argv[2]) : (usage(stderr), EXIT_ERROR);
    if (strcmp(cmd, "run") != 0) {
        usage(stderr);
        return EXIT_ERROR;
    }

    options opt = {NULL, DEFAULT_MAX_CYCLES, DEFAULT_CLOCK, false, false, NULL};
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--trace")) {
            opt.trace = true;
        } else if (!strcmp(a, "--ports")) {
            opt.ports = true;
        } else if (!strcmp(a, "--max-cycles") && i + 1 < argc) {
            if (!parse_u64(argv[++i], &opt.max_cycles)) {
                fprintf(stderr, "mcs51: invalid --max-cycles value '%s'\n", argv[i]);
                return EXIT_ERROR;
            }
        } else if (!strcmp(a, "--clock") && i + 1 < argc) {
            uint64_t hz;
            if (!parse_u64(argv[++i], &hz) || hz == 0) {
                fprintf(stderr, "mcs51: invalid --clock value '%s'\n", argv[i]);
                return EXIT_ERROR;
            }
            opt.clock = (double)hz;
        } else if (!strcmp(a, "--rx") && i + 1 < argc) {
            opt.rx = argv[++i];
        } else if (a[0] != '-' && opt.path == NULL) {
            opt.path = a;
        } else {
            usage(stderr);
            return EXIT_ERROR;
        }
    }
    if (opt.path == NULL) {
        usage(stderr);
        return EXIT_ERROR;
    }
    return cmd_run(&opt);
}
