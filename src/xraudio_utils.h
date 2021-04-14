/*
##########################################################################
# If not stated otherwise in this file or this component's LICENSE
# file the following copyright and licenses apply:
#
# Copyright 2019 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
##########################################################################
*/
#ifndef __XRAUDIO_UTILS__
#define __XRAUDIO_UTILS__

typedef enum {
   XRAUDIO_UTILS_SOUND_END_REASON_DONE    = 0,
   XRAUDIO_UTILS_SOUND_END_REASON_ERROR   = 1,
   XRAUDIO_UTILS_SOUND_END_REASON_REVOKED = 2,
   XRAUDIO_UTILS_SOUND_END_REASON_INVALID = 3
} xraudio_utils_sound_end_reason_t;

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*xraudio_utils_sound_play_end_callback_t)(xraudio_utils_sound_end_reason_t reason, void *param);

int                   xraudio_utils_sound_play(const char *file, uint8_t timeout, xraudio_volume_step_t volume_step, char **error, xraudio_utils_sound_play_end_callback_t callback, void *param);
void                  xraudio_utils_sound_stop(void); // Please note that this function MUST be called at the end of the playback and CANNOT be called in the context of the callback
xraudio_volume_step_t xraudio_utils_sound_volume_up(void);
xraudio_volume_step_t xraudio_utils_sound_volume_down(void);

#ifdef __cplusplus
}
#endif

#endif
