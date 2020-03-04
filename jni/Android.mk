LOCAL_PATH := $(call my-dir)/..
include $(CLEAR_VARS)

LOCAL_CFLAGS    := -std=c99 -O2 -W -Wall -pthread -pipe $(COPT)
LOCAL_MODULE    := dlc
LOCAL_SRC_FILES := dlc_server.c src/mongoose.c

include $(BUILD_EXECUTABLE)
