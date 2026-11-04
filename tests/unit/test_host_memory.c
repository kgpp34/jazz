#include "platform/host_memory.h"
#include "check.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void run(const char *maps, uintptr_t addr, size_t length, size_t limit,
    int expected, size_t expected_count)
{
    FILE *stream=fmemopen((void *)maps,strlen(maps),"r"); CHECK(stream);
    struct jz_memory_buffer buffer={(void *)addr,length};
    struct jz_host_chunk *chunks=NULL; size_t count=0;
    CHECK(jz_host_plan_stream(stream,&buffer,limit,&chunks,&count,NULL)==expected);
    if (!expected) {
        CHECK(count==expected_count);
        size_t total=0;
        for(size_t i=0;i<count;++i) {
            CHECK(chunks[i].offset==total && chunks[i].length && chunks[i].length<=limit);
            total+=chunks[i].length;
        }
        CHECK(total==length);
    } else CHECK(!chunks && !count);
    free(chunks); CHECK(fclose(stream)==0);
}
int main(void)
{
    const char *normal="1000-9000 rw-p 0000 00:00 0\nKernelPageSize: 4 kB\nVmFlags: rd wr hg\n";
    run(normal,0x1001,0x7001,10000,0,3);
    const char *huge="200000-800000 rw-p 0000 00:00 0\nKernelPageSize: 2048 kB\nVmFlags: rd wr ht\n";
    run(huge,0x200000,0x600000,0x300000,0,3);
    run(huge,0x200001,31,32,0,1); /* No blanket alignment rejection. */
    run(huge,0x200000,0x600000,0x100000,-E2BIG,0);
    const char *mixed="100000-200000 rw-p 0 00:00 0\nKernelPageSize: 64 kB\nVmFlags: rd wr\n"
        "200000-400000 rw-p 0 00:00 0\nKernelPageSize: 2048 kB\nVmFlags: rd wr ht\n";
    run(mixed,0x100000,0x300000,0x300000,0,2);
    run(normal,0x8000,0x2000,4096,-EFAULT,0);
    run("1000-9000 rw-p 0 00:00 0\n",0x1000,32,4096,-EOPNOTSUPP,0);
    char bytes[64]; struct jz_memory_buffer buffer={bytes,sizeof(bytes)};
    struct jz_host_chunk *chunks=NULL; size_t count=0;
    CHECK(jz_host_plan(&buffer,16,&chunks,&count,NULL)==0 && count==4);
    char location[JZ_MEMORY_LOCATION_MAX];
    jz_host_location(&buffer,chunks,count,"cpu:7",location);
    CHECK(!strcmp(location,"cpu:7"));
    jz_host_location(&buffer,chunks,count,"*",location);
    CHECK(location[0]); /* Unknown is valid when move_pages is denied. */
    free(chunks);
    return 0;
}
