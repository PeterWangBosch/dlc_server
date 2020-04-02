#pragma once

#ifndef IDCM_LOG_LEVEL_DEFAULT
  #define IDCM_LOG_LEVEL_DEFAULT IDCM_LOG_LEVEL_VERBOSE
#endif

#ifndef IDCM_LOG_TAG
  #define IDCM_LOG_TAG FOTA
#endif

#ifdef ANDROID_FLATORM

#include <android/log.h>
#define IDCM_LOG_LEVEL_VERBOSE  ANDROID_LOG_VERBOSE
#define IDCM_LOG_LEVEL_DEBUG  ANDROID_LOG_DEBUG
#define IDCM_LOG_LEVEL_INFO    ANDROID_LOG_INFO
#define IDCM_LOG_LEVEL_WARN    ANDROID_LOG_WARN
#define IDCM_LOG_LEVEL_ERROR  ANDROID_LOG_ERROR
#define IDCM_LOG_LEVEL_FATAL  ANDROID_LOG_FATAL

#define LOG_PRINT(level, fmt,...)  \
  do {              \
    __android_log_print(level, IDCM_LOG_TAG, "(%s:%u) %s: " fmt,  \
    __FILE__, __LINE__, __PRETTY_FUNCTION__, ##__VA_ARGS__);      \
  } while(0)

#else

#include <stdio.h>

#define IDCM_LOG_LEVEL_VERBOSE  1
#define IDCM_LOG_LEVEL_DEBUG  2
#define IDCM_LOG_LEVEL_INFO    3
#define IDCM_LOG_LEVEL_WARN    4
#define IDCM_LOG_LEVEL_ERROR  5
#define IDCM_LOG_LEVEL_FATAL  6

#define LOG_PRINT(level, fmt,...)          \
  do {                      \
    if (level >= IDCM_LOG_LEVEL_DEFAULT) {    \
      printf("(%s:%u) %s: " fmt, __FILE__, __LINE__, __PRETTY_FUNCTION__, ##__VA_ARGS__);  \
      printf("\n");              \
    }                      \
  } while(0)

#endif

