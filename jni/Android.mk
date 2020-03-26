LOCAL_PATH := $(call my-dir)/..
include $(CLEAR_VARS)

LOCAL_CFLAGS    := -std=c99 -O2 -W -Wall -pthread -pipe -DMG_ENABLE_THREADS -DMG_ENABLE_HTTP_WEBSOCKET=0 $(COPT)
LOCAL_MODULE    := dlc
LOCAL_SRC_FILES := dlc_server.c mongoose/mongoose.c cJSON/cJSON.c

include $(BUILD_EXECUTABLE)
