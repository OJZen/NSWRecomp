#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "shader_key.h"

typedef struct { uint64_t data; const void *control; } NVNshaderData;
typedef struct { uint64_t serial; unsigned nstage; NVNshaderData sd[6]; } prog_t;
static NSMutableDictionary *g_shmap, *manifest;
static NSString *g_msl_dir;
static unsigned batches;
static NSArray *last_batch;
#define MLOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define MDIE(...) do { MLOG(__VA_ARGS__); exit(78); } while (0)
static uint64_t bp_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static void shmap_load(void) { g_shmap = manifest; }
static id serial_get(uint64_t serial) { return serial == UINT64_MAX ? manifest : nil; }
static void zelda_compile_shaders(NSArray *keys) {
    batches++; last_batch = keys;
    for (NSString *key in keys) {
        NSString *capture = [NSString stringWithFormat:@"work/shaders/source/runtime-%@.nvn", key];
        assert([NSFileManager.defaultManager fileExistsAtPath:capture]);
        assert([@"synthetic" writeToFile:[g_msl_dir stringByAppendingPathComponent:[key stringByAppendingString:@"_b.metallib"]]
            atomically:YES encoding:NSUTF8StringEncoding error:nil]);
        manifest[key] = @"compute";
    }
}
#include "nvn_program_prepare.inc"
enum { OP_BIND_PROGRAM, OP_DRAW, OP_DRAW_IDX, OP_OTHER };
typedef struct { unsigned op; } op_hdr;
typedef struct { const prog_t *prog; } p_bind_program;
typedef struct { op_hdr header; p_bind_program binding; } operation;
typedef struct { const operation *ops; unsigned at, count; } fz_cursor;
static int fz_next(fz_cursor *cursor, const uint8_t **position, op_hdr *header, const void **arguments) {
    if (cursor->at == cursor->count) return 0;
    const operation *op = &cursor->ops[cursor->at++];
    *position = (const uint8_t *)op; *header = op->header; *arguments = &op->binding; return 1;
}
#include "nvn_program_prefetch.inc"

int main(int argc, const char **argv) { @autoreleasepool {
    assert(argc >= 2 && !chdir(argv[1]));
    manifest = [NSMutableDictionary dictionary]; g_msl_dir = @"work/shaders/b-synthetic";
    for (NSString *path in @[@"work/shaders/source", g_msl_dir])
        assert([NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil]);
    uint32_t control[0x758 / 4] = {0}; control[0] = 0x98761234; control[1] = 1; control[2] = 16; control[8] = sizeof control;
    control[0x6f8/4] = 16; control[0x6fc/4] = 16; control[0x700/4] = 0x110; control[0x704/4] = 0x120;
    control[0x708/4] = 0x100; control[0x714/4] = 5; control[0x748/4] = control[0x74c/4] = control[0x750/4] = 1;
    uint32_t code[70][0x120/4] = {{0}}; prog_t programs[70] = {0};
    for (unsigned i = 0; i < 70; i++) {
        code[i][0] = 0x12345679; code[i][0x100/4] = i;
        programs[i].serial = i + 1; programs[i].nstage = 1;
        programs[i].sd[0] = (NVNshaderData){(uintptr_t)code[i], control};
    }
    if (argc == 3) { programs[0].nstage = 7; prog_collect_missing(&programs[0], [NSMutableOrderedSet new]); return 1; }
    programs[0].serial = UINT64_MAX;
    programs[3].sd[0] = programs[2].sd[0];
    operation small[] = {{{OP_BIND_PROGRAM},{&programs[0]}},{{OP_DRAW},{0}},{{OP_BIND_PROGRAM},{&programs[1]}},
                         {{OP_BIND_PROGRAM},{&programs[2]}},{{OP_DRAW},{0}},{{OP_BIND_PROGRAM},{&programs[3]}},{{OP_DRAW_IDX},{0}}};
    fz_cursor cursor = {small, 0, sizeof small / sizeof *small};
    prog_prepare_rest(cursor, NULL);
    assert(!cursor.at && batches == 1 && last_batch.count == 1);
    NSString *unused = [NSString stringWithFormat:@"work/shaders/source/runtime-%@.nvn", prog_stage_key(&programs[1].sd[0])];
    assert(![NSFileManager.defaultManager fileExistsAtPath:unused]);
    prog_prepare_rest(cursor, NULL); assert(batches == 1);
    operation limit[162] = {0};
    for (unsigned i = 0; i < 160; i++) limit[i].header.op = OP_DRAW;
    limit[160] = (operation){{OP_BIND_PROGRAM},{&programs[4]}}; limit[161].header.op = OP_DRAW;
    prog_prepare_rest((fz_cursor){limit,0,162}, &programs[2]); assert(batches == 1);
    operation many[140];
    for (unsigned i = 0; i < 70; i++) {
        programs[i].serial = i + 1; programs[i].sd[0].data = (uintptr_t)code[i];
        many[i*2] = (operation){{OP_BIND_PROGRAM},{&programs[i]}}; many[i*2+1] = (operation){{OP_DRAW},{0}};
    }
    [manifest removeAllObjects];
    prog_prepare_rest((fz_cursor){many,0,140}, NULL); assert(batches == 2 && last_batch.count == 64);
    puts("Program batch deduplication, unused binds and bounds passed");
    return 0;
} }
