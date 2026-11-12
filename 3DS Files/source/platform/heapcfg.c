/* Heap split for the memory modes (meta/app.rsf: 64MB on Old 3DS, 178MB on New 3DS).
 *
 * Stock libctru gives the linear (GPU-visible, physically contiguous) heap at most 32MB and
 * pours everything else into the malloc heap. With a 178MB grant that is a ~146MB malloc heap,
 * which does not fit the 128MB heap window at 0x08000000: svcControlMemory fails and libctru
 * panics before main(), with no log and no picture. The game's models, textures and the GL
 * vertex ring all live in linear memory (the iOS allocator is backed by linearAlloc), so the
 * extra memory belongs there anyway.
 *
 * So this replaces libctru's weak __system_allocateHeaps():
 *   - the malloc heap gets a quarter of the grant, between HEAP_MIN_WANT and HEAP_WANT (a
 *     race peaks at about 8MB of malloc and 21MB of linear memory, so on an Old 3DS's ~60MB
 *     the linear heap is the one that needs the room),
 *   - the linear heap gets the rest, capped at one 128MB FCRAM bank (a physically contiguous
 *     block cannot cross the bank boundary on New 3DS), asked for first while the region is
 *     least fragmented,
 *   - both step down and retry instead of panicking, so a console that gives less costs a few
 *     megabytes and a log line instead of a dead boot.
 * The numbers asked for and granted are kept in g_heap_info and written to the debug log.
 * Same approach as the NFSU2 port, which found the contiguity limit on a New 3DS XL. */
#include <3ds.h>
#include <3ds/allocator/mappable.h>
#include "sys.h"

/* libctru's heap globals: the sizes are weak, the pointers plain .bss. Whatever is allocated
 * here must be recorded in them exactly (linearAlloc builds its pool from them, and libctru
 * frees them on exit). */
extern u32 __ctru_heap, __ctru_heap_size, __ctru_linear_heap, __ctru_linear_heap_size;
/* newlib's sbrk window; libctru carves the main thread stack off its front after this. */
extern char *fake_heap_start, *fake_heap_end;

HeapInfo g_heap_info;

#define MB (1024u * 1024u)
#define HEAP_WANT (40u * MB)
#define HEAP_MIN_WANT (12u * MB)
#define HEAP_MIN (4u * MB)
#define FCRAM_BANK (128u * MB)
#define LINEAR_MIN (4u * MB)
#define STEP (4u * MB)
#define ALIGN4K(x) ((u32)(x) & ~0xFFFu)

void __system_allocateHeaps(void) {
    s64 max = 0, used = 0;
    ResourceLimitType kind = RESLIMIT_COMMIT;
    Handle rl = 0;
    if (R_SUCCEEDED(svcGetResourceLimit(&rl, CUR_PROCESS_HANDLE))) {
        svcGetResourceLimitLimitValues(&max, rl, &kind, 1);
        svcGetResourceLimitCurrentValues(&used, rl, &kind, 1);
        svcCloseHandle(rl);
    }
    u32 avail = (used >= 0 && max > used) ? ALIGN4K(max - used) : 60u * MB;
    g_heap_info.avail = avail;

    u32 heap = ALIGN4K(avail / 4);
    if (heap > HEAP_WANT) heap = HEAP_WANT;
    if (heap < HEAP_MIN_WANT) heap = avail / 2 < HEAP_MIN_WANT ? ALIGN4K(avail / 2) : HEAP_MIN_WANT;
    u32 want = avail - heap;
    if (want > FCRAM_BANK) want = FCRAM_BANK;
    want = ALIGN4K(want);

    for (;;) {
        __ctru_linear_heap = 0;
        g_heap_info.linear_tries++;
        if (R_SUCCEEDED(svcControlMemory(&__ctru_linear_heap, 0, 0, want, MEMOP_ALLOC_LINEAR,
                                         MEMPERM_READWRITE))) {
            __ctru_linear_heap_size = want;
            break;
        }
        if (want <= LINEAR_MIN) {
            __ctru_linear_heap = __ctru_linear_heap_size = 0;
            break;
        }
        want = want > LINEAR_MIN + STEP ? want - STEP : LINEAR_MIN;
    }
    g_heap_info.linear = __ctru_linear_heap_size;

    for (;;) {
        __ctru_heap = 0;
        if (R_SUCCEEDED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0, heap, MEMOP_ALLOC,
                                         MEMPERM_READWRITE))) {
            __ctru_heap_size = heap;
            break;
        }
        if (heap <= HEAP_MIN) svcBreak(USERBREAK_PANIC); /* nothing can run without a heap */
        heap = heap > HEAP_MIN + STEP ? heap - STEP : HEAP_MIN;
    }
    g_heap_info.heap = __ctru_heap_size;

    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
    fake_heap_start = (char *)__ctru_heap;
    fake_heap_end = fake_heap_start + __ctru_heap_size;
}
