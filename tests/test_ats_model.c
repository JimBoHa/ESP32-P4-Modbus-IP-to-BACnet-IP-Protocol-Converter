/* Independent decoder fixtures. No networking or equipment identifiers. */
#include "ats_model.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static ats_value_t values[ATS_POINT_COUNT];
#define P(instance) values[(instance)-1001u]
#define EQ(actual, expected) assert(fabs((actual)-(expected)) < 1e-9)

static void apply(ats_model_t *model, unsigned offset, unsigned count,
                  const uint16_t *words, uint64_t timestamp)
{
    assert(ats_model_apply_block(model, (uint16_t)offset, (uint16_t)count, words, timestamp));
}

static void metadata(void)
{
    size_t i, j;
    unsigned ai=0, bi=0, msi=0, csv=0;
    for (i=0; i<ATS_POINT_COUNT; ++i) {
        const ats_point_def_t *p = &ats_points[i];
        unsigned contained=0;
        assert(p->instance == 1001u+i);
        assert(p->name && p->description && p->word_count >= 1 && p->word_count <= 10);
        switch (p->object_type) {
        case 0: ++ai; break;
        case 3: ++bi; break;
        case 13: ++msi; assert(p->state_count >= 3 && p->state_text); break;
        case 40: ++csv; break;
        default: assert(0);
        }
        for (j=0; j<i; ++j) assert(strcmp(p->name, ats_points[j].name));
        for (j=0; j<ATS_BLOCK_COUNT; ++j) {
            const ats_scan_block_t *b = &ats_scan_blocks[j];
            assert(b->count <= 40 && b->stale_ms >= b->period_ms);
            if (p->offset >= b->offset && p->offset+p->word_count <= b->offset+b->count) ++contained;
        }
        assert(contained == 1);
    }
    assert(ai==109 && bi==26 && msi==15 && csv==14);
}

static void startup_failure_stale_recovery(void)
{
    ats_model_t model, before;
    uint16_t words[19]={0x2801,2741,2799,2965,0,0,0,4920,4905,4910,0,0,0,599,0,0,0,0,45};
    size_t i;
    ats_model_init(&model);
    ats_model_decode_all(&model, 0, values);
    for (i=0; i<ATS_POINT_COUNT; ++i) {
        assert(values[i].quality == ATS_QUALITY_COMM);
        assert(isnan(values[i].numeric));
        assert(!values[i].raw_count && !values[i].text[0]);
    }
    apply(&model, 0, 19, words, 1000);
    ats_model_decode_all(&model, 1000, values);
    EQ(P(1013).numeric,492.0); assert(P(1013).quality == ATS_QUALITY_GOOD);
    before=model;
    assert(!ats_model_apply_block(&model,0,18,words,1001));
    assert(!ats_model_apply_block(&model,0,19,words,999));
    assert(!ats_model_apply_block(&model,0,19,NULL,1001));
    assert(!memcmp(&model,&before,sizeof(model)));
    assert(ats_model_fail_block(&model,0,19,1100));
    ats_model_decode_all(&model,1100,values);
    EQ(P(1013).numeric,492.0); assert(P(1013).quality == ATS_QUALITY_COMM);
    assert(P(1013).last_good_ms==1000 && P(1013).raw_words[0]==4920);
    words[7]=4855; apply(&model,0,19,words,1200);
    ats_model_decode_all(&model,7200,values);
    EQ(P(1013).numeric,485.5); assert(P(1013).quality == ATS_QUALITY_GOOD);
    ats_model_decode_all(&model,7201,values);
    assert(P(1013).quality == ATS_QUALITY_COMM); EQ(P(1013).numeric,485.5);
    ats_model_decode_all(&model,1199,values);
    assert(P(1013).quality == ATS_QUALITY_COMM);
}

static void overview_current_and_angles(void)
{
    ats_model_t model;
    uint16_t words[19]={0x2801}, mode[1]={0}, angle[1]={10};
    ats_model_init(&model);
    words[15]=1234; /* Even nonzero does not prove current sensors are fitted. */
    apply(&model,0,19,words,0); apply(&model,56,1,mode,0); apply(&model,40,1,angle,0);
    ats_model_decode_all(&model,0,values);
    EQ(P(1002).numeric,2); /* raw1 Normal -> BACnet state2 */
    EQ(P(1003).numeric,1); EQ(P(1004).numeric,256);
    EQ(P(1005).numeric,1); EQ(P(1006).numeric,0);
    EQ(P(1021).numeric,123.4); assert(P(1021).quality==ATS_QUALITY_UNVERIFIED);
    assert(P(1022).quality==ATS_QUALITY_UNVERIFIED && P(1023).quality==ATS_QUALITY_UNVERIFIED);
    assert(P(1024).quality==ATS_QUALITY_NA && P(1025).quality==ATS_QUALITY_NA);
    words[0]=0x6007; mode[0]=2;
    apply(&model,0,19,words,1); apply(&model,56,1,mode,1);
    ats_model_decode_all(&model,1,values);
    EQ(P(1002).numeric,4); /* Old-map raw3 Fault, never both-connected. */
    EQ(P(1003).numeric,2); EQ(P(1005).numeric,1); EQ(P(1006).numeric,1);
    assert(P(1024).quality==ATS_QUALITY_UNVERIFIED && P(1025).quality==ATS_QUALITY_UNVERIFIED);
    words[0]|=0x8000; apply(&model,0,19,words,2);
    ats_model_decode_all(&model,2,values);
    assert(P(1001).quality==ATS_QUALITY_UNVERIFIED);
    assert(P(1002).quality==ATS_QUALITY_GOOD);
}

static void optional_board_and_software_io(void)
{
    ats_model_t model;
    uint16_t hardware[4]={0}, states[10]={3,2,0,0,0,0,0,0,0,0}, software[4]={0};
    ats_model_init(&model);
    apply(&model,193,10,states,100); apply(&model,249,4,software,100);
    ats_model_decode_all(&model,100,values);
    assert(P(1092).quality==ATS_QUALITY_COMM); /* Missing presence is not absent. */
    apply(&model,153,4,hardware,100);
    ats_model_decode_all(&model,100,values);
    assert(P(1092).quality==ATS_QUALITY_NA && P(1096).quality==ATS_QUALITY_NA);
    assert(P(1100).quality==ATS_QUALITY_NA && P(1102).quality==ATS_QUALITY_NA);
    assert(P(1101).quality==ATS_QUALITY_GOOD && P(1103).quality==ATS_QUALITY_GOOD);
    EQ(P(1088).numeric,1); EQ(P(1089).numeric,1); EQ(P(1090).numeric,0); EQ(P(1091).numeric,1);
    hardware[0]=1; hardware[1]=9; apply(&model,153,4,hardware,101);
    software[1]=1; software[3]=16; apply(&model,249,4,software,101);
    ats_model_decode_all(&model,101,values);
    assert(P(1092).quality==ATS_QUALITY_GOOD && P(1096).quality==ATS_QUALITY_GOOD);
    assert(P(1093).quality==ATS_QUALITY_UNVERIFIED);
    assert(P(1100).quality==ATS_QUALITY_GOOD && P(1102).quality==ATS_QUALITY_GOOD);
    assert(ats_model_fail_block(&model,153,4,102));
    ats_model_decode_all(&model,102,values);
    assert(P(1092).quality==ATS_QUALITY_COMM);
}

static void enums_boolean_and_scaling(void)
{
    ats_model_t model;
    uint16_t rotation[1]={0x0203}, nominal[8]={2,4800,4800,600,600,3,3,1600};
    uint16_t trip[26]={0}, delay[3]={5,123,456}, mode[1]={3};
    ats_model_init(&model);
    trip[4]=2; trip[7]=5; trip[12]=1;
    apply(&model,44,1,rotation,0); apply(&model,45,8,nominal,0);
    apply(&model,77,26,trip,0); apply(&model,74,3,delay,0); apply(&model,56,1,mode,0);
    ats_model_decode_all(&model,0,values);
    EQ(P(1026).numeric,3); assert(P(1026).quality==ATS_QUALITY_GOOD);
    EQ(P(1027).numeric,4); assert(P(1027).quality==ATS_QUALITY_UNVERIFIED);
    EQ(P(1029).numeric,480); EQ(P(1031).numeric,60); EQ(P(1035).numeric,1600);
    EQ(P(1040).numeric,12.3); EQ(P(1041).numeric,45.6); EQ(P(1063).numeric,0.5);
    assert(P(1060).quality==ATS_QUALITY_UNVERIFIED); EQ(P(1060).numeric,2);
    assert(P(1068).quality==ATS_QUALITY_GOOD); EQ(P(1068).numeric,1);
    assert(P(1036).quality==ATS_QUALITY_UNVERIFIED); /* Reserved transition raw3. */
}

static void strings_clock_dates_and_history(void)
{
    ats_model_t model;
    uint16_t text[40]={0x4241,0x0043}; /* Low-byte-first -> ABC. */
    uint16_t firmware[3]={0x0203,0x0403,0x0001};
    uint16_t clock[3]={1439,(uint16_t)((24u<<9)|(2u<<5)|29u),1};
    uint16_t history[26]={0};
    uint16_t outage[4]={60,(uint16_t)((24u<<9)|(2u<<5)|29u),0,69};
    ats_model_init(&model);
    history[2]=80; history[5]=0x1234; history[6]=0x5678;
    history[13]=0xffff; history[14]=0xffff; history[25]=0x0221;
    apply(&model,1109,40,text,100); apply(&model,1199,3,firmware,100);
    apply(&model,1249,3,clock,100); apply(&model,1294,26,history,100); apply(&model,1009,4,outage,100);
    ats_model_decode_all(&model,100,values);
    assert(!strcmp(P(1133).text,"ABC") && P(1133).quality==ATS_QUALITY_GOOD);
    assert(!strcmp(P(1137).text,"2.03"));
    assert(!strcmp(P(1140).text,"23:59") && !strcmp(P(1141).text,"2024-02-29"));
    assert(P(1141).quality==ATS_QUALITY_GOOD);
    EQ(P(1148).numeric,305419896.0); /* Explicit MSW-first, no text-style swapping. */
    EQ(P(1152).numeric,4294967295.0);
    assert(P(1143).quality==ATS_QUALITY_UNVERIFIED && P(1145).quality==ATS_QUALITY_GOOD);
    assert(P(1162).quality==ATS_QUALITY_UNVERIFIED && !strcmp(P(1162).text,"2001-01-01"));
    EQ(P(1132).numeric,69); assert(P(1132).quality==ATS_QUALITY_UNVERIFIED);
    assert(P(1132).raw_count==2 && P(1132).raw_words[1]==69);
    clock[0]=1440; clock[1]=(uint16_t)((23u<<9)|(2u<<5)|29u); text[0]=0xffff;
    apply(&model,1249,3,clock,101); apply(&model,1109,40,text,101);
    ats_model_decode_all(&model,101,values);
    assert(P(1140).quality==ATS_QUALITY_UNVERIFIED && P(1141).quality==ATS_QUALITY_UNVERIFIED);
    assert(P(1133).quality==ATS_QUALITY_UNVERIFIED && P(1133).raw_words[0]==0xffff);
}

static void all_blocks_raw_word_boundaries(void)
{
    ats_model_t model;
    uint16_t words[ATS_MAX_BLOCK_WORDS];
    size_t b, j, p;
    ats_model_init(&model);
    /* Unique synthetic raw values exercise every field offset, including
     * the final word of each string/counter and far-apart wire addresses.
     * Run under UBSan: forming a pointer using the absolute wire offset
     * before subtracting the block base is undefined even if it lands back.
     */
    for (b=0; b<ATS_BLOCK_COUNT; ++b) {
        for (j=0; j<ats_scan_blocks[b].count; ++j)
            words[j]=(uint16_t)(ats_scan_blocks[b].offset+j);
        apply(&model, ats_scan_blocks[b].offset, ats_scan_blocks[b].count, words, 1000);
        ats_model_decode_all(&model, 1000, values);
    }
    for (p=0; p<ATS_POINT_COUNT; ++p) {
        assert(values[p].raw_count==ats_points[p].word_count);
        assert(values[p].last_good_ms==1000);
        for (j=0; j<values[p].raw_count; ++j)
            assert(values[p].raw_words[j]==ats_points[p].offset+j);
    }
    EQ(P(1164).numeric,9998); /* Highest mapped wire address, one-word block. */
    EQ(P(1139).numeric,1201); /* Old-map fingerprint slot, block's final word. */
    assert(ats_model_fail_block(&model,1109,40,1001));
    ats_model_decode_all(&model,1001,values);
    assert(P(1136).quality==ATS_QUALITY_COMM && P(1136).raw_words[9]==1148);
}

int main(void)
{
    metadata();
    startup_failure_stale_recovery();
    overview_current_and_angles();
    optional_board_and_software_io();
    enums_boolean_and_scaling();
    strings_clock_dates_and_history();
    all_blocks_raw_word_boundaries();
    puts("ats_model: 7 fixture groups passed (164 points, all 23 blocks)");
    return 0;
}
