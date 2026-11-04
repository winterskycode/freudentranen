/* Darwin quality-of-service classes mapped to Linux thread niceness (Android lets apps raise their own threads to -10). */
#pragma once
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>
typedef enum { QOS_CLASS_USER_INTERACTIVE = 0x21, QOS_CLASS_USER_INITIATED = 0x19, QOS_CLASS_DEFAULT = 0x15,
               QOS_CLASS_UTILITY = 0x11, QOS_CLASS_BACKGROUND = 0x09, QOS_CLASS_UNSPECIFIED = 0x00 } qos_class_t;
static __thread qos_class_t compat_thread_qos;
static inline int pthread_set_qos_class_self_np(qos_class_t q, int relative) {
    (void)relative; compat_thread_qos = q;
    int nice = q == QOS_CLASS_USER_INTERACTIVE ? -10 : q == QOS_CLASS_USER_INITIATED ? -4 : q == QOS_CLASS_UTILITY ? 4 : q == QOS_CLASS_BACKGROUND ? 10 : 0;
    setpriority(PRIO_PROCESS, (id_t)gettid(), nice);
    return 0;
}
static inline int pthread_get_qos_class_np(pthread_t t, qos_class_t *q, int *relative) {
    (void)t; if (q) *q = compat_thread_qos; if (relative) *relative = 0; return 0;
}
static inline qos_class_t qos_class_self(void) { return compat_thread_qos; }
/* Thread attributes cannot carry a QoS class on Linux: the thread sets it on itself (ignored here). */
static inline int pthread_attr_set_qos_class_np(pthread_attr_t *a, qos_class_t q, int relative) { (void)a; (void)q; (void)relative; return 0; }
