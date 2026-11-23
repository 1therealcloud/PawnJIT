#define YSI_TESTS
#define YSI_NO_TEST_WARNINGS
#define LIGHT_TEST_REPORT

#include <open.mp>
#include <YSI_Coding/y_inline>
#include <YSI_Coding/y_malloc>
#include <YSI_Coding/y_timers>
#include <YSI_Coding/y_hooks>

main()
{
    SetTimer("PawnJIT_YsiTimer", 100, false);
}

forward PawnJIT_YsiTimer();
public PawnJIT_YsiTimer()
{
    new start = GetTickCount();
    new checksum;
    for (new i; i < 2000000; ++i)
        checksum = checksum * 31 + i;

    printf("[PawnJIT YSI] checksum %d after %d ms (expected 1525334592)", checksum, GetTickCount() - start);
}
