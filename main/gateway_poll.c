#include "gateway_poll.h"
#include <string.h>

void gateway_poll_init(gateway_poll_t *s)
{
    memset(s, 0, sizeof(*s));
    ats_model_init(&s->model);
    s->profile_status = "Awaiting controller identity";
}

void gateway_poll_offline(gateway_poll_t *s, uint64_t now)
{
    for (size_t i = 0; i < ATS_BLOCK_COUNT; ++i) {
        ats_model_fail_block(&s->model, ats_scan_blocks[i].offset,
                             ats_scan_blocks[i].count, now);
    }
    s->profile_valid = false;
    s->profile_stage = 0;
    s->profile_status = "Network unavailable; identity check required";
}

static void reject_profile(gateway_poll_t *s, uint64_t now, const char *why)
{
    gateway_poll_offline(s, now);
    s->profile_status = why;
    s->next_request_ms = now + 30000;
}

bool gateway_poll_step(gateway_poll_t *s, const gateway_poll_config_t *c, uint64_t now)
{
    if (!s || !c || now < s->next_request_ms) return false;
    if (s->profile_valid && now >= s->profile_check_ms) {
        s->profile_valid = false;
        s->profile_stage = 0;
        s->profile_status = "Rechecking controller identity";
    }
    uint16_t offset = 0, count = 1, words[50];
    size_t block = ATS_BLOCK_COUNT;
    bool checking = !s->profile_valid;
    if (checking) {
        /* Check the new-map MAC BEFORE touching the old-map MAC register.
         * Register1201 is not an old-map read on newer MPAC controllers. */
        switch (s->profile_stage) {
        case 0: offset = 9998; break;
        case 1: offset = 1199; break;
        case 2: offset = 1209; count = 3; break;
        default: offset = 1201; break;
        }
    } else {
        uint64_t earliest = UINT64_MAX;
        for (size_t i = 0; i < ATS_BLOCK_COUNT; ++i) {
            if (s->next_due[i] <= now && s->next_due[i] < earliest) {
                earliest = s->next_due[i];
                block = i;
            }
        }
        if (block == ATS_BLOCK_COUNT) return false;
        offset = ats_scan_blocks[block].offset;
        count = ats_scan_blocks[block].count;
    }
    s->last_offset = offset;
    ++s->requests;
    mb_error_t error = mb_read_holding(c->host, c->port, c->unit, offset, count,
        ++s->transaction_id, 1200, words, &s->last_result);
    uint64_t completed = now + s->last_result.elapsed_ms;
    s->next_request_ms = completed + 250;
    bool expected_old_map = checking && s->profile_stage == 2 && error == MB_ERR_EXCEPTION &&
        s->last_result.exception_code == 2;
    if (error != MB_OK && !expected_old_map) {
        ++s->failures;
        ++s->consecutive_failures;
        if (block < ATS_BLOCK_COUNT) {
            ats_model_fail_block(&s->model, offset, count, completed);
            s->next_due[block] = completed + 5000;
        }
        /* A transport failure is global: immediately fault all last values
         * and restart identity checks after bounded backoff. */
        if (s->last_result.exception_code == 0) {
            gateway_poll_offline(s, completed);
        } else if (checking) {
            reject_profile(s, completed, "Identity read rejected");
        }
        uint32_t wait = s->consecutive_failures < 6 ?
            s->consecutive_failures * 5000 : 30000;
        if (s->next_request_ms < completed + wait) s->next_request_ms = completed + wait;
        return true;
    }
    ++s->successes;
    s->consecutive_failures = 0;
    s->last_success_ms = completed;
    if (checking) {
        if (s->profile_stage == 0 && words[0] != 23) {
            reject_profile(s, completed, "Wrong controller type: expected MPAC1500 ID23");
        } else if (s->profile_stage == 1 && words[0] != c->expected_firmware) {
            reject_profile(s, completed, "Firmware does not match commissioned old-map profile");
        } else if (s->profile_stage == 2 && !expected_old_map) {
            reject_profile(s, completed, "Newer MPAC map detected; old map disabled");
        } else if (s->profile_stage == 3) {
            if (c->expected_mac_fragment && (words[0] & 0x7fff) != c->expected_mac_fragment) {
                reject_profile(s, completed, "Controller MAC fingerprint mismatch");
            } else {
                s->profile_valid = true;
                s->profile_check_ms = completed + 300000;
                s->profile_status = "Old MPAC1500 profile verified";
            }
        } else {
            ++s->profile_stage;
        }
    } else {
        ats_model_apply_block(&s->model, offset, count, words, completed);
        s->next_due[block] = completed + ats_scan_blocks[block].period_ms;
    }
    return true;
}
