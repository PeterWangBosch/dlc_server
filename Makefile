PROG = dlc_server
MODULE_CFLAGS=-DMG_ENABLE_THREADS -DMG_ENABLE_HTTP_WEBSOCKET=0

CJSON_SRCS = cJSON/cJSON.c cJSON/cJSON_Utils.c bs_dlc_utils.c

SOURCES = $(PROG).c mongoose/mongoose.c $(CJSON_SRCS)
CFLAGS = -g -W -Wall -Werror -I../.. -Wno-unused-function $(CFLAGS_EXTRA) $(MODULE_CFLAGS)

ifneq ($(OS), Windows_NT)
CFLAGS += -pthread
endif

all: $(PROG)

ifeq ($(SSL_LIB),openssl)
CFLAGS += -DMG_ENABLE_SSL -lssl -lcrypto
endif
ifeq ($(SSL_LIB), krypton)
CFLAGS += -DMG_ENABLE_SSL ../../../krypton/krypton.c -I../../../krypton
endif
ifeq ($(SSL_LIB),mbedtls)
CFLAGS += -DMG_ENABLE_SSL -DMG_SSL_IF=MG_SSL_IF_MBEDTLS -DMG_SSL_MBED_DUMMY_RANDOM -lmbedcrypto -lmbedtls -lmbedx509
endif

$(PROG): $(SOURCES)
	$(CC) $(SOURCES) -o $@ $(CFLAGS)

clean:
	rm -rf *.gc* *.dSYM *.exe *.obj *.o a.out $(PROG)
