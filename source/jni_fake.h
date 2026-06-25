/* jni_fake.h -- fake JNI environment for libfox.so (Sonic 4 Episode II)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

#include <stdint.h>

typedef int32_t jint;
typedef uint32_t juint;
typedef uint8_t jboolean;

extern void *fake_vm;  // JavaVM *
extern void *fake_env; // JNIEnv *

// set when the engine asks the activity to quit
extern volatile int jni_quit_requested;
extern volatile int g_continue_flag;

void jni_init(void);

// set the directory reported to the engine as the writable / save path
void jni_set_writable_path(const char *p);

// constructors for fake Java objects
void *jni_make_string(const char *utf);
void *jni_make_object(const char *label);
void *jni_new_byte_array(const void *data, int len);
void *jni_new_int_array(const int *data, int len);
void *jni_new_float_array(const float *data, int len);
void  jni_delete_ref(void *ref);

#endif
