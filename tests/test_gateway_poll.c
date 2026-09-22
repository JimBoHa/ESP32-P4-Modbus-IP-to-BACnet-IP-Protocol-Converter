#include "gateway_poll.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned calls;
static uint16_t offsets[512];
static bool wrong_type, wrong_version, new_map, timeout_next;

mb_error_t mb_read_holding(const char *host, uint16_t port, uint8_t unit,
    uint16_t offset, uint16_t count, uint16_t tid, uint32_t timeout,
    uint16_t out[50], mb_result_t *result)
{
    (void)host; (void)port; (void)unit; (void)tid;
    assert(timeout == 1200 && count <= 50);
    assert(calls < sizeof(offsets)/sizeof(offsets[0]));
    offsets[calls++] = offset;
    memset(result, 0, sizeof(*result));
    if (timeout_next) {
        timeout_next = false;
        result->error = MB_ERR_TIMEOUT;
        result->elapsed_ms = 1200;
        return result->error;
    }
    memset(out, 0, count*sizeof(*out));
    if (offset == 9998) out[0] = wrong_type ? 27 : 23;
    if (offset == 1199) out[0] = wrong_version ? 516 : 515;
    if (offset == 1201) out[0] = 0x3456;
    if (offset == 1209 && !new_map) {
        result->error = MB_ERR_EXCEPTION;
        result->exception_code = 2;
        return result->error;
    }
    if (offset == 0) { out[0] = 0x2801; out[7] = 4800; }
    return MB_OK;
}

static gateway_poll_config_t config = {
    .host="127.0.0.1", .port=15020, .unit=41,
    .expected_firmware=515, .expected_mac_fragment=0x3456,
};

static void reset(gateway_poll_t *p)
{
    calls=0; wrong_type=wrong_version=new_map=timeout_next=false;
    gateway_poll_init(p);
}

static void gate(gateway_poll_t *p)
{
    for (unsigned i=0;i<4;++i) assert(gateway_poll_step(p,&config,i*250));
    assert(p->profile_valid);
    assert(calls==4 && offsets[0]==9998 && offsets[1]==1199 && offsets[2]==1209 && offsets[3]==1201);
}

int main(void)
{
    gateway_poll_t p;
    reset(&p); gate(&p);
    assert(!gateway_poll_step(&p,&config,999));
    assert(gateway_poll_step(&p,&config,1000));
    assert(offsets[4]==0 && p.model.blocks[0].acquired);
    timeout_next=true;
    assert(gateway_poll_step(&p,&config,1250));
    assert(!p.profile_valid && p.model.blocks[0].failed);
    assert(p.failures==1 && p.next_request_ms>=7450);
    assert(!gateway_poll_step(&p,&config,7449));
    assert(gateway_poll_step(&p,&config,7450));
    assert(offsets[calls-1]==9998);

    reset(&p); wrong_type=true;
    assert(gateway_poll_step(&p,&config,0));
    assert(!p.profile_valid && calls==1 && p.next_request_ms>=30000);
    assert(!gateway_poll_step(&p,&config,2000));

    reset(&p); wrong_version=true;
    assert(gateway_poll_step(&p,&config,0));
    assert(gateway_poll_step(&p,&config,250));
    assert(!p.profile_valid && calls==2);

    reset(&p); new_map=true;
    for (unsigned i=0;i<3;++i) assert(gateway_poll_step(&p,&config,i*250));
    assert(!p.profile_valid && calls==3); /* Never read newer-map WO register1201. */

    reset(&p); config.expected_mac_fragment=1;
    for (unsigned i=0;i<4;++i) assert(gateway_poll_step(&p,&config,i*250));
    assert(!p.profile_valid && calls==4);
    config.expected_mac_fragment=0x3456;

    reset(&p); gate(&p);
    for (uint64_t t=1000;t<30000;t+=250) gateway_poll_step(&p,&config,t);
    bool seen[ATS_BLOCK_COUNT]={0};
    for(unsigned i=4;i<calls;++i)
        for(unsigned j=0;j<ATS_BLOCK_COUNT;++j)
            if(offsets[i]==ats_scan_blocks[j].offset) seen[j]=true;
    for(unsigned j=0;j<ATS_BLOCK_COUNT;++j) assert(seen[j]);
    puts("poller: identity gates, old/new map isolation, timeout faults, backoff and scan coverage passed");
    return 0;
}
