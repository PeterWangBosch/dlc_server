LOCAL_PATH := $(call my-dir)/..
include $(CLEAR_VARS)

LOCAL_CFLAGS    :=  -O2 -W -Wall -pthread -pipe -lstdc++ -Iinclude -DANDROID
LOCAL_MODULE    := server
LOCAL_SRC_FILES := main.cpp src/tftp_server.cpp

include $(BUILD_EXECUTABLE)
