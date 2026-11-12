/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_txt.h"
#include "airplay_info_plist.h"
#include "airplay_bplist.h"
#include "airplay_vendor_ie.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void capability_consistency(void)
{
    airplay_info_plist_config_t info={.device_id="AA:BB:CC:DD:EE:FF",.width_pixels=1024,.height_pixels=600};
    for(unsigned mode=0;mode<3;++mode) {
        info.audio_output=mode!=0;info.audio_compressed=true;
        info.features=mode==2 ? UINT64_C(0x800000015653aee2) : 0;
        char words[32];uint64_t features=airplay_info_plist_features(&info);
        assert(airplay_txt_format_features(features,words,sizeof(words)));
        unsigned low,high;assert(sscanf(words,"0x%x,0x%x",&low,&high)==2);
        uint64_t discovered=((uint64_t)high<<32)|low;
        uint8_t plist[AIRPLAY_INFO_PLIST_MAX_BYTES];size_t length;
        assert(airplay_info_plist_build(&info,plist,sizeof(plist),&length));
        airplay_bplist_doc_t doc;airplay_bplist_object_t root,value;uint64_t served;
        assert(airplay_bplist_open(plist,length,&doc)==AIRPLAY_BPLIST_OK);
        assert(airplay_bplist_root(&doc,&root)==AIRPLAY_BPLIST_OK);
        assert(airplay_bplist_dict_get(&doc,root,"features",&value)==AIRPLAY_BPLIST_OK);
        assert(airplay_bplist_get_uint(&doc,value,&served)==AIRPLAY_BPLIST_OK);
        assert(served==discovered && served==features);
        if(mode==1) {
            assert(served & (UINT64_C(1) << 18)); /* actual PCM capability */
#ifdef RTCP_AUDIO_CODECS
            assert(!strcmp(words,"0x5657aee2,0x61"));
#else
            assert(!strcmp(words,"0x5647aee2,0x61"));
#endif
        }
        if(!mode)assert(!strcmp(words,AIRPLAY_TXT_DEFAULT_FEATURES));
    }
    char tiny[2]={'x','x'};
    assert(!airplay_txt_format_features(UINT64_MAX,tiny,sizeof(tiny)) && !tiny[0]);
    assert(!airplay_txt_format_features(1,NULL,0));
    assert(airplay_info_plist_features(NULL)==AIRPLAY_INFO_DEFAULT_FEATURES);
}

int main(void)
{
    capability_consistency();
    size_t ie_len = 0U;
    const uint8_t *ie = airplay_vendor_ie(&ie_len);
    static const uint8_t expected_ie[] = {
        0xDD, 0x08, 0x00, 0xA0, 0x40, 0x00, 0x00, 0x02, 0x00, 0x22,
    };
    assert(ie_len == sizeof(expected_ie));
    assert(memcmp(ie, expected_ie, sizeof(expected_ie)) == 0);
    assert(airplay_vendor_ie_valid(ie, ie_len));
    assert(!airplay_vendor_ie_valid(ie, ie_len - 1U));
    assert(!airplay_vendor_ie_valid(NULL, ie_len));

    airplay_txt_config_t config = {
        .device_id = "AA:BB:CC:DD:EE:FF",
        .model = "AudioAccessory5,1",
        .source_version = "950.7.1",
        .protocol_version = "1.1",
        .pairing_id = "11111111-2222-3333-4444-555555555555",
        .public_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    };
    airplay_txt_record_t records[AIRPLAY_TXT_MAX_RECORDS];
    size_t count = airplay_txt_build(&config, records, AIRPLAY_TXT_MAX_RECORDS);
    assert(count == 8U);
    assert(strcmp(records[0].key, "deviceid") == 0);
    assert(strcmp(records[0].value, config.device_id) == 0);
    assert(strcmp(records[1].key, "features") == 0);
    assert(strcmp(records[1].value, AIRPLAY_TXT_DEFAULT_FEATURES) == 0);
    assert(strcmp(records[2].key, "flags") == 0);
    assert(strcmp(records[2].value, AIRPLAY_TXT_DEFAULT_FLAGS) == 0);
    assert(strcmp(records[6].key, "pi") == 0);
    assert(strcmp(records[7].key, "pk") == 0);

    config.features = "0x1";
    config.flags = "0x0";
    assert(airplay_txt_build(&config, records, AIRPLAY_TXT_MAX_RECORDS) == 8U);
    assert(strcmp(records[1].value, "0x1") == 0);
    assert(strcmp(records[2].value, "0x0") == 0);

    config.model = NULL;
    assert(airplay_txt_build(&config, records, AIRPLAY_TXT_MAX_RECORDS) == 0U);
    config.model = "AudioAccessory5,1";
    assert(airplay_txt_build(&config, records, 7U) == 0U);

    uint8_t wire[512];
    size_t wire_len = airplay_txt_encode(records, count, wire, sizeof(wire));
    assert(wire_len > 0U);
    assert(wire_len == 1U + strlen("deviceid") + 1U + strlen(config.device_id) +
                            1U + strlen("features") + 1U + strlen("0x1") +
                            1U + strlen("flags") + 1U + strlen("0x0") +
                            1U + strlen("model") + 1U + strlen(config.model) +
                            1U + strlen("srcvers") + 1U + strlen(config.source_version) +
                            1U + strlen("protovers") + 1U + strlen(config.protocol_version) +
                            1U + strlen("pi") + 1U + strlen(config.pairing_id) +
                            1U + strlen("pk") + 1U + strlen(config.public_key));
    assert(wire[0] == strlen("deviceid") + 1U + strlen(config.device_id));
    assert(memcmp(wire + 1, "deviceid=", 9) == 0);

    /* Truncation must fail rather than write a partial entry. */
    assert(airplay_txt_encode(records, count, wire, wire_len - 1U) == 0U);

    puts("discovery host test passed");
    return 0;
}
