#ifndef OSAL_DEFS_H
#define OSAL_DEFS_H

#include <string.h>

#define OSAL_THREAD_HANDLE void*

#ifndef PACKED
  #if defined(__ICCARM__)
    #define PACKED_BEGIN  _Pragma("pack(push, 1)")
    #define PACKED        /* IAR: packing via PACKED_BEGIN/END pragma */
    #define PACKED_END    _Pragma("pack(pop)")
  #elif defined(__GNUC__)
    #define PACKED_BEGIN
    #define PACKED  __attribute__((__packed__))
    #define PACKED_END
  #else
    #define PACKED_BEGIN
    #define PACKED
    #define PACKED_END
  #endif
#endif

// define if debug printf is needed
//#define EC_DEBUG

#ifdef EC_DEBUG
#include <stdio.h>
#define EC_PRINT printf
#else
#define EC_PRINT(...) do {} while (0)
#endif

static inline int osal_getsysinfo(char *sysinfo) {
    strcpy(sysinfo, "FreeRTOS_RZN2L");
    return 1;
}

#endif /* OSAL_DEFS_H */
