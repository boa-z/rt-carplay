/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int fail_commit, fail_restore;
static int fat_rename(const char *old, const char *newpath) {
    struct stat st;
    if (stat(newpath,&st)==0) {errno=EEXIST; return -1;}
    if ((fail_commit && strstr(old,".tmp")) || (fail_restore && strstr(old,".bak"))) {errno=EIO; return -1;}
    return rename(old,newpath);
}
#define rename fat_rename
#include "../../examples/rtthread_app/pairing_files.c"
#undef rename
#include "../../examples/rtthread_app/usb_pair_cache.c"
#include "../../examples/rtthread_app/pairing_manager.c"
int main(void) {
    char directory[]="/tmp/carplay-persistence-XXXXXX";
    assert(mkdtemp(directory));
    char path[256], backup[256];
    snprintf(path,sizeof(path),"%s/store",directory);
    snprintf(backup,sizeof(backup),"%s/store.bak",directory);
    uint8_t data[32]; size_t n;
    assert(carplay_file_save(path,(const uint8_t *)"old",3)==0);
    assert(carplay_file_save(path,(const uint8_t *)"new",3)==0); /* FatFS cannot overwrite */
    assert(carplay_file_load(path,data,sizeof(data),&n)==0 && n==3 && !memcmp(data,"new",3));
    fail_commit=1; fail_restore=1;
    assert(carplay_file_save(path,(const uint8_t *)"bad",3)!=0);
    assert(carplay_file_load(path,data,sizeof(data),&n)==0 && n==3 && !memcmp(data,"new",3));
    fail_commit=fail_restore=0;
    assert(carplay_file_save(path,(const uint8_t *)"restored",8)==0);
    assert(carplay_file_load(path,data,sizeof(data),&n)==0 && n==8);
    static rtcp_pair_record_t record, loaded;
    memset(&record,0,sizeof(record));
    strcpy(record.host_id,"11111111-1111-4111-8111-111111111111");
    strcpy(record.system_buid,"22222222-2222-4222-8222-222222222222");
#define SET(name) record.name[0]=1; record.name##_length=1
    SET(device_public_key); SET(device_certificate); SET(host_private_key);
    SET(host_certificate); SET(root_private_key); SET(root_certificate);
#undef SET
    assert(carplay_usb_pair_cache_save(path,&record)==0);
    assert(carplay_usb_pair_cache_load(path,&loaded)==0 && !memcmp(&record,&loaded,sizeof(record)));
    FILE *f=fopen(path,"r+b"); assert(f); fputc('X',f); fclose(f);
    assert(carplay_usb_pair_cache_load(path,&loaded)!=0 && loaded.root_private_key_length==0);
    record.root_private_key_length=sizeof(record.root_private_key)+1;
    assert(!rtcp_pair_record_valid(&record));
    remove(path); remove(backup);
    const char *usb = "rt-carplay-usb-0123456789abcdef.bin";
    snprintf(path,sizeof(path),"%s/%s",directory,usb);
    assert(carplay_usb_pair_cache_save(path,&loaded)!=0); /* corrupt/zero record */
    assert(carplay_file_save(path,(const uint8_t *)"trust",5)==0);
    assert(carplay_file_save(path,(const uint8_t *)"trust2",6)==0);
    char identity[256]; snprintf(identity,sizeof(identity),"%s/rt-carplay.seed",directory);
    assert(carplay_file_save(identity,(const uint8_t *)"identity",8)==0);
    carplay_pair_file_t files[4]; unsigned count; bool more;
    assert(carplay_pair_files_scan(directory,files,4,&count,&more)==0);
    assert(count==1 && !more && !strcmp(files[0].name,usb));
    assert(carplay_pair_file_forget(directory,"rt-carplay.seed")==RTCP_EINVAL);
    assert(carplay_pair_file_forget(directory,"../rt-carplay-pairings.bin")==RTCP_EINVAL);
    assert(carplay_pair_file_forget(directory,usb)==0);
    assert(carplay_file_load(path,data,sizeof(data),&n)==0 && n==0); /* .bak cannot resurrect */
    assert(carplay_file_load(identity,data,sizeof(data),&n)==0 && n==8);
    snprintf(path,sizeof(path),"%s/rt-carplay-pairings.bin.bak",directory);
    assert(carplay_file_save(path,(const uint8_t *)"recover",7)==0);
    assert(carplay_pair_files_scan(directory,files,4,&count,&more)==0 && count==1);
    assert(!strcmp(files[0].name,"rt-carplay-pairings.bin"));
    assert(carplay_pair_file_forget(directory,files[0].name)==0);
    remove(identity); assert(rmdir(directory)==0);
    puts("FatFS-style replacement, interrupted commit recovery and pair cache bounds/integrity passed");
}
