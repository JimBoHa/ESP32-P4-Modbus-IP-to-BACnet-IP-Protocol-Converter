#include "gateway_poll.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned calls;
static uint16_t offsets[512];
static bool wrong_type, wrong_version, new_map, timeout_next;
static mb_error_t error_next;
static gateway_poll_t *request_state;

mb_error_t mb_read_holding(const char *host, uint16_t port, uint8_t unit,
    uint16_t offset, uint16_t count, uint16_t tid, uint32_t timeout,
    uint16_t out[50], mb_result_t *result)
{
    (void)host; (void)port; (void)unit;
    assert(timeout == 1200 && count <= 50);
    /* A failure handler can change profile state. Capture its original request
     * metadata before entering the transport, not from the state afterward. */
    assert(request_state->last_offset == offset && request_state->last_quantity == count);
    assert(request_state->last_checking == !request_state->profile_valid);
    assert(request_state->transaction_id == tid);
    assert(calls < sizeof(offsets)/sizeof(offsets[0]));
    offsets[calls++] = offset;
    memset(result, 0, sizeof(*result));
    if (timeout_next) {
        timeout_next = false;
        result->error = MB_ERR_TIMEOUT;
        result->elapsed_ms = 1200;
        return result->error;
    }
    if (error_next != MB_OK) {
        result->error = error_next;
        if (error_next == MB_ERR_EXCEPTION) result->exception_code = 2;
        error_next = MB_OK;
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
    error_next=MB_OK;
    request_state=p;
    gateway_poll_init(p);
}

static void gate(gateway_poll_t *p)
{
    const uint16_t expected_offset[] = {9998,1199,1209,1201};
    const uint16_t expected_quantity[] = {1,1,3,1};
    for (unsigned i=0;i<4;++i) {
        assert(gateway_poll_step(p,&config,i*250));
        assert(p->last_checking && p->last_offset==expected_offset[i]);
        assert(p->last_quantity==expected_quantity[i]);
        assert(p->requests==i+1 && p->successes==i+1 && p->failures==0);
        if (i==2) assert(p->last_result.error==MB_ERR_EXCEPTION && p->last_result.exception_code==2);
    }
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
    assert(!p.last_checking && p.last_quantity==ats_scan_blocks[0].count);
    timeout_next=true;
    assert(gateway_poll_step(&p,&config,1250));
    assert(!p.profile_valid && p.model.blocks[0].failed);
    assert(p.failures==1 && p.next_request_ms>=7450);
    assert(!p.last_checking && p.last_offset==ats_scan_blocks[1].offset);
    assert(p.last_quantity==ats_scan_blocks[1].count && p.transaction_id==6);
    assert(p.last_result.error==MB_ERR_TIMEOUT && p.last_result.elapsed_ms==1200);
    assert(!gateway_poll_step(&p,&config,7449));
    assert(!p.last_checking && p.last_offset==ats_scan_blocks[1].offset && p.transaction_id==6);
    assert(gateway_poll_step(&p,&config,7450));
    assert(offsets[calls-1]==9998);
    assert(p.last_checking && p.last_quantity==1 && p.transaction_id==7 && p.failures==1);

    /* Actual transport, protocol, and data exceptions are failures; the same
     * exception 2 is successful only during the old-map identity probe. */
    const mb_error_t failures[] = {MB_ERR_CONNECT, MB_ERR_TRANSACTION, MB_ERR_EXCEPTION};
    for (size_t i=0;i<sizeof(failures)/sizeof(failures[0]);++i) {
        reset(&p); gate(&p);
        error_next=failures[i];
        assert(gateway_poll_step(&p,&config,1000));
        assert(p.requests==5 && p.successes==4 && p.failures==1);
        assert(p.last_offset==0 && p.last_quantity==ats_scan_blocks[0].count && !p.last_checking);
        assert(p.last_result.error==failures[i] && p.transaction_id==5);
    }
    reset(&p);
    assert(gateway_poll_step(&p,&config,0));
    assert(gateway_poll_step(&p,&config,250));
    error_next=MB_ERR_PROTOCOL;
    assert(gateway_poll_step(&p,&config,500));
    assert(p.failures==1 && p.successes==2 && p.last_result.error==MB_ERR_PROTOCOL);
    assert(p.last_offset==1209 && p.last_quantity==3 && p.last_checking && p.profile_stage==0);

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
    puts("poller: identity gates, request metadata, old/new map isolation, timeout faults, backoff and scan coverage passed");
    return 0;
}
