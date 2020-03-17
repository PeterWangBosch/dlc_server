/*
 * Copyright (c) 2014 Cesanta Software Limited
 * All rights reserved
 */

#include "stdio.h"
#include "mongoose/mongoose.h"

/* reserved buffer to save memory */
static char g_cmd_buf[1024];
static char g_cmd_output[1024];

/**
 * CGW API Handler
**/
struct bs_cgw_api_handler {
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
struct bs_context {
  struct mg_connection * dmc;
  unsigned char cgw_thread_exit;
  unsigned char downloader_thread_exit;
  struct bs_cgw_api_handler cgw_api_pkg_new;
  struct bs_cgw_api_handler cgw_api_pkg_stat;
  struct bs_cgw_api_handler cgw_api_tdr_run;
  struct bs_cgw_api_handler cgw_api_tdr_stat;
  char * tftp_server;
  char * downloader;
  char * pkg_cdn_url;
  char * cmd_buf;
  char * cmd_output;
  
  void * data;
};
struct bs_context g_ctx;
static void core_state_handler(unsigned char);

//---------------------------------------------------------------------------
// Communication with DMC
//--------------------------------------------------------------------------- 
static int dmc_tftp_run(struct bs_context * p_ctx) {
  // TODO: 1. format message according convention with SocketWrapper
  // TODO: 2. need root right to listen port 69?
  char * succ = "{ \"DLC pkg new\": \"Downloader start to run\"}";
  char * fail = "{ \"DLC pkg new\": \"Donwloader failed to run\"}";

  FILE *fp;

  if ((fp = popen(p_ctx->cmd_buf, "r")) != NULL) {
    mg_send(p_ctx->dmc, succ, strlen(succ));
  } else {
    mg_send(p_ctx->dmc, fail, strlen(fail));
  }

  //TODO:  handle the cases of tftp failed and monitoring tftp server

  return 1;
}

static int dmc_downloader_stat(struct bs_context * p_ctx) {
  (void) p_ctx;
  return g_stat;
}

static void * dmc_downloader_thread(void * param) {
  // TODO: format message according convention with SocketWrapper
  char * succ = "{ \"DLC pkg new\": \"Downloader start to run\"}";
  char * fail = "{ \"DLC pkg new\": \"Donwloader failed to run\"}";

  FILE *fp;
  struct bs_context * p_ctx = (struct bs_context *) param;

  // TODO: memeset g_cmd_buf to zero and check the length of url and dlc_path
  strcpy(p_ctx->cmd_buf, p_ctx->downloader);
  strcat(p_ctx->cmd_buf, " ");
  strcat(p_ctx->cmd_buf, p_ctx->pkg_cdn_url);    

  if ((fp = popen(p_ctx->cmd_buf, "r")) != NULL) {
    mg_send(p_ctx->dmc, succ, strlen(succ));
  } else {
    mg_send(p_ctx->dmc, fail, strlen(fail));
    return NULL;
  }

  printf("--> curl started!\n");
  while (!p_ctx->downloader_thread_exit &&
         fgets(p_ctx->cmd_output, 1024, fp) != NULL) {
    // block until has lock
    while (g_stat_lock);

    g_stat_lock = 1;
    // TODO: need better way to check successful downloading  
    g_stat_lock = 0;
  }
  printf("--> curl ended!\n");

  while (g_stat_lock);
  g_stat_lock = 1;
  if (pclose(fp) == 0) {
    g_stat = DLC_PKG_READY;
    core_state_handler(STAT_INVALID);
  }
  g_stat_lock = 0;

  return NULL;
}

static int dmc_downloader_run(struct bs_context * p_ctx) {
  mg_start_thread(dmc_downloader_thread, (void *) p_ctx);
  return 1;
}

static void dmc_msg_handler(struct mg_connection *nc, int ev, void *p) {
  struct mbuf *io = &nc->recv_mbuf;
  unsigned int len = 0;
  (void) p;

  switch (ev) {
    case MG_EV_ACCEPT:
      g_ctx.dmc = nc;
      dmc_tftp_run(&g_ctx);
      core_state_handler(DLC_PKG_NEW);//??test
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
        g_ctx.cgw_thread_exit = 1;
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
      g_ctx.cgw_thread_exit = 1;
      break;
    case MG_EV_CLOSE:
      g_ctx.cgw_api_pkg_new.nc = NULL;
      if (g_ctx.cgw_thread_exit == 0) {
        printf("Orchestrator pkg new: connection closed\n");
        g_ctx.cgw_thread_exit = 1;
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
        g_ctx.cgw_thread_exit = 1;
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
      g_ctx.cgw_thread_exit = 1;
      break;
    case MG_EV_CLOSE:
      g_ctx.cgw_api_pkg_stat.nc = NULL;
      if (g_ctx.cgw_thread_exit == 0) {
        printf("Orchestrator pkg status: closed connection\n");
        g_ctx.cgw_thread_exit = 1;
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
        g_ctx.cgw_thread_exit = 1;
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
      g_ctx.cgw_thread_exit = 1;
      break;
    case MG_EV_CLOSE:
      g_ctx.cgw_api_tdr_run.nc = NULL;
      if (g_ctx.cgw_thread_exit == 0) {
        printf("Orchestrator tdr run: closed connection\n");
        g_ctx.cgw_thread_exit = 1;
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
        g_ctx.cgw_thread_exit = 1;
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
      g_ctx.cgw_thread_exit = 1;
      break;
    case MG_EV_CLOSE:
      g_ctx.cgw_api_tdr_stat.nc = NULL;
      if (g_ctx.cgw_thread_exit == 0) {
        printf("Orchestrator tdr status: closed connection\n");
        g_ctx.cgw_thread_exit = 1;
      }
      break;
    default:
      break;
  }
}

static void * cgw_msg_thread(void *param) {
  struct mg_mgr mgr;
  struct bs_cgw_api_handler * handler = (struct bs_cgw_api_handler *) param;

  mg_mgr_init(&mgr, NULL);
  mg_connect_http(&mgr, handler->fn, handler->api, NULL, NULL);

  while (g_ctx.cgw_thread_exit == 0) {
    if (g_stat_lock)
      continue;

    // run until one api request end
    g_stat_lock = 1;
    mg_mgr_poll(&mgr, 300);
    g_stat_lock = 0;
  }

  mg_mgr_free(&mgr);
  return NULL;
}

//---------------------------------------------------------------------------
// Core state machine
// Set g_stat_lock=1 before invoking this function
//--------------------------------------------------------------------------- 
static void core_state_handler(unsigned char reset) {

  // force to reset
  if (reset != STAT_INVALID) { 
    g_stat = reset;
  }

  switch (g_stat) {
    case STAT_IDLE:
      break;
    case DLC_PKG_NEW:
      // TODO: parse received JSON

      dmc_downloader_run(&g_ctx);
      g_stat = DLC_PKG_DOWNLOADING;
      break;
    case DLC_PKG_DOWNLOADING:
      // TODO: verify checksum of downloaded pkg
      break;
    case DLC_PKG_READY:
      printf("---> pgk ready\n");
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

  g_ctx.cgw_thread_exit = 0;
  g_ctx.downloader_thread_exit = 0;

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

  g_ctx.cmd_buf = g_cmd_buf;
  g_ctx.cmd_output = g_cmd_output;

  g_ctx.pkg_cdn_url = "ftp://speedtest.tele2.net/1KB.zip";
  
  g_ctx.downloader = "curl --output wpc.1.0.0"; 
//  g_ctx.downloader = "/data/duc/test_interface/dlc";

  g_ctx.tftp_server = "sudo ./tftpserver"; 
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
