/*
 * Copyright (c) 2014 Cesanta Software Limited
 * All rights reserved
 */

#include "stdio.h"
#include "mongoose/mongoose.h"

static char s_tdr_stat[512] = { 0 };
static unsigned int i_tdr_stat_len = 0;

/**
 * CGW API Handler
**/
struct cgw_api_handler {
  char * api;
  mg_event_handler_t fn;
  struct mg_connection * nc;
};

/**
 * core state machine
**/
#define STAT_INVALID  0xFF
#define STAT_IDLE  0x00
#define DLC_PKG_NEW 0x01
#define DLC_PKG_DOWNLOADING 0x02
#define DLC_PKG_READY 0x03
#define DLC_PKG_BAD 0x04

#define ORCH_CON_ERR 0x10
#define ORCH_PKG_DOWNLOADING 0x20
#define ORCH_PKG_READY 0x30
#define ORCH_PKG_BAD 0x40
#define ORCH_TDR_RUN 0x50
#define ORCH_TDR_FAIL 0x60
#define ORCH_TDR_SUCC 0x70
static unsigned char g_stat = 0;
static unsigned char g_stat_lock = 0;
struct context {
  struct mg_connection * dmc;
  struct cgw_api_handler cgw_api_pkg_new;
  struct cgw_api_handler cgw_api_pkg_stat;
  struct cgw_api_handler cgw_api_tdr_run;
  struct cgw_api_handler cgw_api_tdr_stat;
  char * downloader;
  char * pkg_cdn_url;
  void * data;
};
struct context g_ctx;

static unsigned char g_cgw_thread_exit_flag = 0;

static void core_state_handler(unsigned char);

static int dmc_downloader_run(struct mg_connection *nc, char* url, char* dlc_path) {
    (void) nc;
    (void) url;
    (void) dlc_path;
    return 1;
}

static int dmc_downloader_stat() {
    return 1;
}

static void socket_run_dlc_command(struct mg_connection *nc, char* url, char* dlc_path) {
    FILE *fp;
    char cmd[256] = {0};
    char output[300];

    // DLC command
    sprintf(cmd, "echo \"%s 2000000\" >> %s", url, dlc_path );

    if ((fp = popen(cmd, "r")) != NULL) {
        mg_send(nc, "{ \"DLC status\": \"Going to run\"}", 31);
    } else {
        mg_send(nc, "{ \"DLC status\": \"Failed to run\"}", 32);
        return;
    }

    while (fgets(output, sizeof(output)-1, fp) != NULL){
        if ((i_tdr_stat_len + sizeof(output)) >= 512) {
          sprintf(s_tdr_stat, "%s", output);
          i_tdr_stat_len = strlen(s_tdr_stat);
        } else {
          sprintf(s_tdr_stat, "%s\n %s", s_tdr_stat, output);
          i_tdr_stat_len = strlen(s_tdr_stat);
        }
    }

   pclose(fp);
}

static void dmc_msg_handler(struct mg_connection *nc, int ev, void *p) {
  struct mbuf *io = &nc->recv_mbuf;
  unsigned int len = 0;
  (void) p;

  switch (ev) {
    case MG_EV_ACCEPT:
      break;
    case MG_EV_RECV:
      // first 4 bytes for length
      len = io->buf[3] + (io->buf[2] << 8) + (io->buf[1] << 16) + (io->buf[0] << 24);   
      //TODO: parse JSON
      (void) len;
      core_state_handler(STAT_INVALID);
      mbuf_remove(io, io->len);       // Discard message from recv buffer
      break;
    default:
      break;
  }
}

//---------------------------------------------------------------------------
// Communication with Orchestrator on CGW
//--------------------------------------------------------------------------- 

static void cgw_handler_pkg_new(struct mg_connection *nc, int ev, void *ev_data) {
  struct http_message *hm = (struct http_message *) ev_data;
  (void) hm;

  switch (ev) {
    case MG_EV_CONNECT:
      if (*(int *) ev_data != 0) {
        printf("Orchestrator pkg new: connect failed\n");
        g_stat = ORCH_CON_ERR;
        g_cgw_thread_exit_flag = 1;
      } else {
        g_ctx.cgw_api_pkg_new.nc = nc;
        g_stat = ORCH_PKG_DOWNLOADING;
        printf("Orchestrator pkg new: connected to send msg\n");
      }
      break;
    case MG_EV_HTTP_REPLY:
      //TODO: parse JSON, if error then g_stat = ORCH_NET_ERR;
      g_stat = ORCH_PKG_DOWNLOADING;
      nc->flags |= MG_F_CLOSE_IMMEDIATELY;
      g_cgw_thread_exit_flag = 1;
      break;
    case MG_EV_CLOSE:
      g_ctx.cgw_api_pkg_new.nc = NULL;
      if (g_cgw_thread_exit_flag == 0) {
        printf("Orchestrator pkg new: connection closed\n");
        g_cgw_thread_exit_flag = 1;
      }
      break;
    default:
      break;
  }
}

static void cgw_handler_pkg_stat(struct mg_connection *nc, int ev, void *ev_data) {
  struct http_message *hm = (struct http_message *) ev_data;
  (void) hm;

  switch (ev) {
    case MG_EV_CONNECT:
      if (*(int *) ev_data != 0) {
        printf("Orchestrator pkg status: connect failed\n");
        g_stat = ORCH_PKG_BAD;
        g_cgw_thread_exit_flag = 1;
      } else {
        g_ctx.cgw_api_pkg_stat.nc = nc;
        g_stat = ORCH_PKG_DOWNLOADING;
        printf("Orchestrator pkg status: connected to send msg\n");
      }
      break;
    case MG_EV_HTTP_REPLY:
      //TODO: parse JSON, if error then g_stat = ORCH_NET_ERR;
      //g_stat = ORCH_PKG_DOWNLOADING;
      g_stat = ORCH_PKG_READY;
      nc->flags |= MG_F_CLOSE_IMMEDIATELY;
      g_cgw_thread_exit_flag = 1;
      break;
    case MG_EV_CLOSE:
      g_ctx.cgw_api_pkg_stat.nc = NULL;
      if (g_cgw_thread_exit_flag == 0) {
        printf("Orchestrator pkg status: closed connection\n");
        g_cgw_thread_exit_flag = 1;
      }
      break;
    default:
      break;
  }
}

static void cgw_handler_tdr_run(struct mg_connection *nc, int ev, void *ev_data) {
  struct http_message *hm = (struct http_message *) ev_data;
  (void) hm;

  switch (ev) {
    case MG_EV_CONNECT:
      if (*(int *) ev_data != 0) {
        printf("Orchestrator tdr run: connect failed\n");
        g_stat = ORCH_CON_ERR;
        g_cgw_thread_exit_flag = 1;
      } else {
        g_ctx.cgw_api_tdr_run.nc = nc;
        g_stat = ORCH_TDR_RUN;
        printf("Orchestrator tdr run: connected to send msg\n");
      }
      break;
    case MG_EV_HTTP_REPLY:
      //TODO: parse JSON, if error then g_stat = ORCH_NET_ERR;
      // g_stat = ORCH_TDR_FAIL;
      g_stat = ORCH_TDR_RUN;
      nc->flags |= MG_F_CLOSE_IMMEDIATELY;
      g_cgw_thread_exit_flag = 1;
      break;
    case MG_EV_CLOSE:
      g_ctx.cgw_api_tdr_run.nc = NULL;
      if (g_cgw_thread_exit_flag == 0) {
        printf("Orchestrator tdr run: closed connection\n");
        g_cgw_thread_exit_flag = 1;
      }
      break;
    default:
      break;
  }
}

static void cgw_handler_tdr_stat(struct mg_connection *nc, int ev, void *ev_data) {
  struct http_message *hm = (struct http_message *) ev_data;
  (void)hm;

  switch (ev) {
    case MG_EV_CONNECT:
      g_ctx.cgw_api_tdr_stat.nc = nc;
      if (*(int *) ev_data != 0) {
        printf("Orchestrator tdr status: connect failed\n");
        g_stat = ORCH_CON_ERR;
        g_cgw_thread_exit_flag = 1;
      } else {
        g_ctx.cgw_api_tdr_stat.nc = nc;
        g_stat = ORCH_TDR_RUN;
        printf("Orchestrator tdr status: connected to send msg\n");
      }
      break;
    case MG_EV_HTTP_REPLY:
      //TODO: parse JSON, if error then g_stat = ORCH_TDR_FAIL;
      // g_stat = ORCH_TDR_SUCC;
      g_stat = ORCH_TDR_RUN;
      nc->flags |= MG_F_CLOSE_IMMEDIATELY;
      g_cgw_thread_exit_flag = 1;
      break;
    case MG_EV_CLOSE:
      g_ctx.cgw_api_tdr_stat.nc = NULL;
      if (g_cgw_thread_exit_flag == 0) {
        printf("Orchestrator tdr status: closed connection\n");
        g_cgw_thread_exit_flag = 1;
      }
      break;
    default:
      break;
  }
}

static void * cgw_msg_thread(void *param) {
  struct mg_mgr mgr;
  struct cgw_api_handler * handler = (struct cgw_api_handler *) param;

  mg_mgr_init(&mgr, NULL);
  mg_connect_http(&mgr, handler->fn, handler->api, NULL, NULL);

  while (g_cgw_thread_exit_flag == 0) {
    if (g_stat_lock)
      continue;

    // run until one api request end
    g_stat_lock = 1;
    mg_mgr_poll(&mgr, 300);
  }
  g_stat_lock = 0;

  mg_mgr_free(&mgr);
  return NULL;
}

//---------------------------------------------------------------------------
// Core state machine
//--------------------------------------------------------------------------- 
static void core_state_handler(unsigned char reset) {
  struct mbuf *io = NULL;

  // force to reset
  if (reset != STAT_INVALID) { 
    g_stat = reset;
    return;
  }

  switch (g_stat) {
    case STAT_IDLE:
      break;
    case DLC_PKG_NEW:
      // TODO: parse received JSON
      io = &g_ctx.dmc->recv_mbuf;
      if (dmc_downloader_run(g_ctx.dmc, (char*)(io->buf+8), g_ctx.downloader)) {
        g_stat = DLC_PKG_DOWNLOADING; 
      } else {
        g_stat = DLC_PKG_BAD;
        mg_send(g_ctx.dmc, "{\"result\":\"Start downloader failed\n\"}", 40);
      }
      break;
    case DLC_PKG_DOWNLOADING:
      if (dmc_downloader_stat() < 0) {
        g_stat = DLC_PKG_BAD;
        mg_send(g_ctx.dmc, "{\"result\":\"Start downloader failed\n\"}", 40);
      } else if (dmc_downloader_stat() > 0) {
        // TODO: verify checksum of downloaded pkg
        g_stat = DLC_PKG_READY;
        mg_send(g_ctx.dmc, "{\"result\":\"IDCM downloading finished\n\"}", 40);
      } else {
        mg_send(g_ctx.dmc, "{\"result\":\"Start downloader failed\n\"}", 40);
      }
      break;
    case DLC_PKG_READY:
      mg_start_thread(cgw_msg_thread, (void *) &(g_ctx.cgw_api_pkg_new)); 
      break;
    case ORCH_CON_ERR:
      break;
    case ORCH_PKG_DOWNLOADING:
      mg_start_thread(cgw_msg_thread, (void *) &(g_ctx.cgw_api_pkg_stat)); 
      break;
    case ORCH_PKG_BAD:
      break;
    case ORCH_PKG_READY:
      mg_start_thread(cgw_msg_thread, (void *) &(g_ctx.cgw_api_tdr_run)); 
      break;
    case ORCH_TDR_RUN:
      mg_start_thread(cgw_msg_thread, (void *) &(g_ctx.cgw_api_tdr_stat)); 
      break;
    case ORCH_TDR_FAIL:
      break;
    case ORCH_TDR_SUCC:
      mg_send(g_ctx.dmc, "{\"result\":\"TDR run succesful\n\"}", 31);
      break;
    default:
      break;
  } 
}
//---------------------------------------------------------------------------
// Main
//---------------------------------------------------------------------------
static void init_context() {
  g_ctx.dmc = NULL;

  g_ctx.cgw_api_pkg_new.api = "http://127.0.0.1:8018/pkg/new";
  g_ctx.cgw_api_pkg_new.fn = cgw_handler_pkg_new;
  g_ctx.cgw_api_pkg_new.nc = NULL;
  g_ctx.cgw_api_pkg_stat.api = "http://127.0.0.1:8018/pkg/sta";
  g_ctx.cgw_api_pkg_stat.fn = cgw_handler_pkg_stat;
  g_ctx.cgw_api_pkg_stat.nc = NULL;
  g_ctx.cgw_api_tdr_run.api = "http://127.0.0.1:8018/tdr/run";
  g_ctx.cgw_api_tdr_run.fn = cgw_handler_tdr_run;
  g_ctx.cgw_api_tdr_run.nc = NULL;
  g_ctx.cgw_api_tdr_stat.api = "http://127.0.0.1:8018/tdr/stat";
  g_ctx.cgw_api_tdr_stat.fn = cgw_handler_tdr_stat;
  g_ctx.cgw_api_tdr_stat.nc = NULL;
  
  g_ctx.downloader = "/data/duc/test_interface/dlc"; 
}

int main(int argc, char *argv[]) {
  struct mg_mgr mgr;

  init_context();

  mg_mgr_init(&mgr, NULL);

  printf("==Start socket server==\n");
  if (argc >= 2 && strcmp(argv[1], "-o") == 0) {
    // Listen to specidied port
    mg_bind(&mgr, argv[2], dmc_msg_handler);
    printf("Listen on port %s\n", argv[2]);
  } else {
    // by default, listen to 3000
    mg_bind(&mgr, "3000", dmc_msg_handler);
    printf("Listen on port 3000\n");
  }

  for (;;) {
    if (g_stat_lock)
      continue;

    g_stat_lock = 1;

    mg_mgr_poll(&mgr, 1000);
    g_stat_lock = 0;
  }
  mg_mgr_free(&mgr);

  return 0;
}
