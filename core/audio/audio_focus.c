/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "include/audio_focus.h"
void rtcp_audio_focus_policy(const rtcp_audio_focus_t *f,rtcp_audio_focus_policy_t *p)
{
    if(!f || !p)return;
    p->main_gain=p->alternate_gain=p->media_gain=p->local_gain=32768;
    if(f->call_active || f->siri_active) p->media_gain=0;
    else if(f->navigation_active || f->local_active)p->media_gain=8192;
    if(f->local_active)p->main_gain=8192;
    if(f->critical_active){p->main_gain=0;p->alternate_gain=0;p->media_gain=0;}
    /* The protected local lane always remains at unity; remote volume/mute
     * lives separately in the mixer and cannot suppress a local alarm. */
}
