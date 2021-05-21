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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <semaphore.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "xraudio.h"
#include "xraudio_utils.h"

#define ERROR_SIZE (128)

typedef struct {
   xraudio_object_t                        object;
   char *                                  buffer;
   sem_t                                   sem_grant;
   sem_t                                   sem_first_frame;
   xraudio_utils_sound_play_end_callback_t callback;
   void *                                  param;
   char                                    error[ERROR_SIZE];
   xraudio_volume_step_t                   volume_step;
} xraudio_instance_t;

static void xraudio_playback_callback(audio_out_callback_event_t event, void *param);
static void resource_notification_callback(xraudio_resource_event_t event, void *param);
static void xraudio_utils_sound_playback_complete(xraudio_instance_t *instance, xraudio_utils_sound_end_reason_t reason);
static bool xraudio_utils_file_get_contents(const char *file, char **contents, off_t *size);

static xraudio_instance_t g_xraudio_utils_sounds;

int xraudio_utils_sound_play(const char *file, uint8_t timeout, xraudio_volume_step_t volume_step, char **error, xraudio_utils_sound_play_end_callback_t callback, void *param) {
   char *sink;
   if(error == NULL) {
      error = &sink;
   }
   *error = NULL;
   if(file == NULL) {
      snprintf(g_xraudio_utils_sounds.error, ERROR_SIZE, "NULL file param");
      *error = g_xraudio_utils_sounds.error;
      return(-1);
   }
   if(g_xraudio_utils_sounds.object != NULL) {
      snprintf(g_xraudio_utils_sounds.error, ERROR_SIZE, "playback in progress");
      *error = g_xraudio_utils_sounds.error;
      return(-1);
   }
   g_xraudio_utils_sounds.callback    = NULL;
   g_xraudio_utils_sounds.param       = NULL;
   g_xraudio_utils_sounds.buffer      = NULL;
   g_xraudio_utils_sounds.volume_step = volume_step;
   off_t size;
   
   if((0 != access(file, F_OK)) || !xraudio_utils_file_get_contents(file, &g_xraudio_utils_sounds.buffer, &size)) {
      snprintf(g_xraudio_utils_sounds.error, ERROR_SIZE, "Unable to open wave file <%s>", file);
      *error = g_xraudio_utils_sounds.error;
      return(-1);
   }
   
   uint32_t data_length = 0;
   xraudio_output_format_t format;
   int32_t data_offset = xraudio_container_header_parse_wave(NULL, (const uint8_t *)g_xraudio_utils_sounds.buffer, size, &format, &data_length);

   if(data_offset < 0 || data_length == 0) {
      snprintf(g_xraudio_utils_sounds.error, ERROR_SIZE, "Unable to parse wave header");
      *error = g_xraudio_utils_sounds.error;
      free(g_xraudio_utils_sounds.buffer);
      return(-1);
   }
   g_xraudio_utils_sounds.object = xraudio_object_create(NULL);
   if(g_xraudio_utils_sounds.object == NULL) {
      snprintf(g_xraudio_utils_sounds.error, ERROR_SIZE, "unable to create object");
      *error = g_xraudio_utils_sounds.error;
      free(g_xraudio_utils_sounds.buffer);
      return(-1);
   }
   
   sem_init(&g_xraudio_utils_sounds.sem_grant, 0, 0);
   sem_init(&g_xraudio_utils_sounds.sem_first_frame, 0, 0);

   g_xraudio_utils_sounds.callback = callback;
   g_xraudio_utils_sounds.param    = param;

   xraudio_result_t result = xraudio_resource_request(g_xraudio_utils_sounds.object, XRAUDIO_DEVICE_INPUT_NONE, XRAUDIO_DEVICE_OUTPUT_NORMAL, XRAUDIO_RESOURCE_PRIORITY_HIGH, resource_notification_callback, &g_xraudio_utils_sounds);
   if(result != XRAUDIO_RESULT_OK) {
      snprintf(g_xraudio_utils_sounds.error, ERROR_SIZE, "resource request error <%s>", xraudio_result_str(result));
      *error = g_xraudio_utils_sounds.error;
      xraudio_object_destroy(g_xraudio_utils_sounds.object);
      g_xraudio_utils_sounds.object = NULL;
      free(g_xraudio_utils_sounds.buffer);
      return(-1);
   }

   // Wait for resource grant
   //printf("%s: wait for resources...\n", __FUNCTION__);
   struct timespec end_time;
   
   int rc = -1;
   if(clock_gettime(CLOCK_REALTIME, &end_time) != 0) {
      //printf("%s: unable to get time\n", __FUNCTION__);
   } else {
      end_time.tv_sec += timeout;
      do {
         errno = 0;
         rc = sem_timedwait(&g_xraudio_utils_sounds.sem_grant, &end_time);
         if(rc == -1 && errno == EINTR) {
            //printf("%s: interrupted\n", __FUNCTION__);
         } else {
            break;
         }
      } while(1);
   }

   if(rc != 0) { // timeout
      snprintf(g_xraudio_utils_sounds.error, ERROR_SIZE, "resource timeout");
      *error = g_xraudio_utils_sounds.error;
      xraudio_resource_release(g_xraudio_utils_sounds.object);
      xraudio_object_destroy(g_xraudio_utils_sounds.object);
      g_xraudio_utils_sounds.object = NULL;
      free(g_xraudio_utils_sounds.buffer);
      return(-1);
   }
   
   result = xraudio_open(g_xraudio_utils_sounds.object, XRAUDIO_POWER_MODE_FULL, false, XRAUDIO_DEVICE_INPUT_NONE, XRAUDIO_DEVICE_OUTPUT_NORMAL, NULL);
   if(result != XRAUDIO_RESULT_OK) {
      snprintf(g_xraudio_utils_sounds.error, ERROR_SIZE, "open error <%s>", xraudio_result_str(result));
      *error = g_xraudio_utils_sounds.error;
      xraudio_resource_release(g_xraudio_utils_sounds.object);
      xraudio_object_destroy(g_xraudio_utils_sounds.object);
      g_xraudio_utils_sounds.object = NULL;
      free(g_xraudio_utils_sounds.buffer);
      return(-1);
   }

   format.container = XRAUDIO_CONTAINER_NONE;
   format.encoding  = XRAUDIO_ENCODING_PCM;
   
   xraudio_play_volume_set(g_xraudio_utils_sounds.object, g_xraudio_utils_sounds.volume_step, g_xraudio_utils_sounds.volume_step);
   result = xraudio_play_from_memory(g_xraudio_utils_sounds.object, &format, (const uint8_t *)&g_xraudio_utils_sounds.buffer[data_offset], data_length, xraudio_playback_callback, &g_xraudio_utils_sounds);
   if(result != XRAUDIO_RESULT_OK) {
      snprintf(g_xraudio_utils_sounds.error, ERROR_SIZE, "open error <%s>", xraudio_result_str(result));
      *error = g_xraudio_utils_sounds.error;
      xraudio_close(g_xraudio_utils_sounds.object);
      xraudio_resource_release(g_xraudio_utils_sounds.object);
      xraudio_object_destroy(g_xraudio_utils_sounds.object);
      g_xraudio_utils_sounds.object = NULL;
      free(g_xraudio_utils_sounds.buffer);
      return(-1);
   }

   sem_wait(&g_xraudio_utils_sounds.sem_first_frame);
   return(0);
}

void xraudio_playback_callback(audio_out_callback_event_t event, void *param) {
   xraudio_instance_t *instance = (xraudio_instance_t *)param;
   
   switch(event) {
      case AUDIO_OUT_CALLBACK_EVENT_OK: {
         break;
      }
      case AUDIO_OUT_CALLBACK_EVENT_FIRST_FRAME: {
         sem_post(&instance->sem_first_frame);
         break;
      }
      case AUDIO_OUT_CALLBACK_EVENT_EOF: {
         xraudio_utils_sound_playback_complete(instance, XRAUDIO_UTILS_SOUND_END_REASON_DONE);
         break;
      }
      case AUDIO_OUT_CALLBACK_EVENT_ERROR: {
         xraudio_utils_sound_playback_complete(instance, XRAUDIO_UTILS_SOUND_END_REASON_ERROR);
         break;
      }
      default: {
         xraudio_utils_sound_playback_complete(instance, XRAUDIO_UTILS_SOUND_END_REASON_ERROR);
         break;
      }
   }
}

void resource_notification_callback(xraudio_resource_event_t event, void *param) {
   xraudio_instance_t *instance = (xraudio_instance_t *)param;

   if(event == XRAUDIO_RESOURCE_EVENT_GRANTED) {
      // Signal grant to user thread
      sem_post(&instance->sem_grant);
   } else if(event == XRAUDIO_RESOURCE_EVENT_REVOKED) {
      sem_post(&instance->sem_first_frame);
      xraudio_utils_sound_playback_complete(instance, XRAUDIO_UTILS_SOUND_END_REASON_REVOKED);
   }
}

void xraudio_utils_sound_playback_complete(xraudio_instance_t *instance, xraudio_utils_sound_end_reason_t reason) {
   // Call the callback with parameter to notify the user of the completion and any error
   if(g_xraudio_utils_sounds.callback != NULL) {
      (*g_xraudio_utils_sounds.callback)(reason, g_xraudio_utils_sounds.param);
      g_xraudio_utils_sounds.callback = NULL;
      g_xraudio_utils_sounds.param    = NULL;
   }
}

void xraudio_utils_sound_stop(void) {
   if(g_xraudio_utils_sounds.object != NULL) {
      xraudio_close(g_xraudio_utils_sounds.object);
      xraudio_resource_release(g_xraudio_utils_sounds.object);
      xraudio_object_destroy(g_xraudio_utils_sounds.object);
      g_xraudio_utils_sounds.object = NULL;
      if(g_xraudio_utils_sounds.buffer != NULL) {
         free(g_xraudio_utils_sounds.buffer);
         g_xraudio_utils_sounds.buffer = NULL;
      }
   }
}

xraudio_volume_step_t xraudio_utils_sound_volume_up(void) {
   g_xraudio_utils_sounds.volume_step++;

   xraudio_volume_step_t step = g_xraudio_utils_sounds.volume_step;
   if(g_xraudio_utils_sounds.volume_step > XRAUDIO_VOLUME_MAX) {
      g_xraudio_utils_sounds.volume_step = XRAUDIO_VOLUME_MAX;
      step                               = XRAUDIO_VOLUME_MAX + 1;
   }

   if(g_xraudio_utils_sounds.object != NULL) { // Set updated volume
      xraudio_play_volume_ramp_set(g_xraudio_utils_sounds.object, step, step, 1);
   }
   return(g_xraudio_utils_sounds.volume_step);
}

xraudio_volume_step_t xraudio_utils_sound_volume_down(void) {
   g_xraudio_utils_sounds.volume_step--;

   xraudio_volume_step_t step = g_xraudio_utils_sounds.volume_step;
   if(g_xraudio_utils_sounds.volume_step < XRAUDIO_VOLUME_MIN) {
      g_xraudio_utils_sounds.volume_step = XRAUDIO_VOLUME_MIN;
      step                               = XRAUDIO_VOLUME_MIN - 1;
   }

   if(g_xraudio_utils_sounds.object != NULL) { // Set updated volume
      xraudio_play_volume_ramp_set(g_xraudio_utils_sounds.object, step, step, 1);
   }
   return(g_xraudio_utils_sounds.volume_step);
}

bool xraudio_utils_file_get_contents(const char *file, char **contents, off_t *size) {
   int fd = -1;
   if(file == NULL || contents == NULL || size == NULL) {
      return(false);
   }
   // Open file
   do {
      errno = 0;
      fd = open(file, O_RDONLY, 0444);
      if(fd < 0) {
         if(errno == EINTR) {
            continue;
         }
         return(false);
      }
      break;
   } while(1);

   // Get size
   off_t file_size = 0;
   do {
      file_size = lseek(fd, 0, SEEK_END);
      if(file_size < 0) {
         if(errno == EINTR) {
            continue;
         }
         close(fd);
         return(false);
      }
      if(file_size == 0) {
         close(fd);
         return(false);
      }
      break;
   } while(1);

   // Seek to beginning of file
   do {
      if(lseek(fd, 0, SEEK_SET) < 0) {
         if(errno == EINTR) {
            continue;
         }
         close(fd);
         return(false);
      }
      break;
   } while(1);

   // Allocate memory
   *contents = malloc(file_size);

   if(*contents == NULL) {
      close(fd);
      return(false);
   }

   // Read contents
   bool result = false;
   do {
      ssize_t bytes_read = read(fd, *contents, file_size);
      if(bytes_read < 0) {
         if(errno == EINTR) {
            continue;
         }
         break;
      }
      if(bytes_read != file_size) {
         break;
      }
      result = true;
      break;
   } while(1);

   close(fd);

   if(!result) {
      free(*contents);
      *contents = NULL;
      *size     = 0;
   } else {
      *size     = file_size;
   }

   return(result);
}
