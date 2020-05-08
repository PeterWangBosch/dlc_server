/*
 * Copyright (c) 2014 Cesanta Software Limited
 * All rights reserved
 */

#include "stdio.h"
#include "cJSON/cJSON.h"
#include "log/idcm_log.h"
#include "mongoose/mongoose.h"

/* reserved buffer to save memory */
static char g_cmd_buf[3096];
static char g_cmd_output[1024];

//static char * g_stub_report = "{\"fotaProtocolVersion\":\"HHFOTA-0.1\",\"vehicleVersion\":{\"orchestrator\":\"0.100\",\"dlc\":\"0.100\"},\"upgradeResults\":{\"servicePack\":\"VDCM\",\"campaign\":\"VDCM\",\"downloadStartTime\":\"20200501 240000\",\"downloadFinishTime\":\"20200501 240000\",\"userConfirmationTime\":\"20200501 240000\",\"startTime\":\"20200501 240000\",\"finishTime\":\"20200501 240000\",\"result\":\"success\",\"dlcReports\":[{\"timestamp\":\"20200501 240000\",\"errorCode\":-1,\"trace\":\"\"}],\"deviceReports\":[{\"ecu\":\"VDCM\",\"softwareId\":\"VDCM2.0.0\",\"startTime\":\"20200501 240000\",\"finishTime\":\"20200501 240000\",\"previousVersion\":\"1.0.0\",\"result\":\"success\",\"targetVersion\":\"2.0.0\",\"currentVersion\":\"1.0.0\",\"logs\":[{\"timestamp\":\"20200501 240000\",\"progress\":0,\"errorCode\":-1,\"trace\":\"\"}]}]}}";

/**
 * CGW API Handler
**/
typedef char * (* api_payload_gener) ();
struct bs_cgw_api_handler {
  bool cgw_thread_exit;
  char * api;
  mg_event_handler_t fn;
  api_payload_gener payload_gener;
  struct mg_connection * nc;
};

//-----------------------------------------------------------------
// HMI Message  
//-----------------------------------------------------------------
#define CHECK_NEW_PACKAGE   1
#define START_UPGRADE       2
#define UPGRADE_PROGRESS    3

#define REQUEST         "1"
#define RESPONSE        "2"

/**
 * Upgrading pkg info of single ECU 
**/
struct bs_pkg_info {
  // “yes” or “no”
  char door_module[8];
  char dev_id[32];
  char soft_id[32];
  char release_notes[256];
};

/**
 * Upgrading status of single ECU
**/
struct bs_ecu_upgrade_stat {
  char dev_id[32];
  char soft_id[32];
  char esti_time[64];
  char start_time[32];
  char time_stamp[32];
  // "yes" or "no"
  char door_module[8];
  // "pending", "in progress", "failed", "success"
  char status[15];
  // raw percentage data, e.g., 0, 55, or 100
  float progress_percent; 
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
struct bs_l1_manifest {
  char dev_id[32];
  char pkg_cdn_url[128];
};
struct bs_context {
  struct mg_connection * dmc;
  struct mg_connection * hmi;
  unsigned char downloader_thread_exit;
  unsigned char hmi_thread_exit;
  struct bs_cgw_api_handler cgw_api_pkg_new;
  struct bs_cgw_api_handler cgw_api_pkg_stat;
  struct bs_cgw_api_handler cgw_api_tdr_run;
  struct bs_cgw_api_handler cgw_api_tdr_stat;
  struct bs_cgw_api_handler cgw_api_pkg_upload;
  struct bs_pkg_info * hmi_pkg_info;
  struct bs_ecu_upgrade_stat * hmi_upgrade_stat;
  char * tftp_server;
  char * downloader;
  char * pkg_url;
  char * cmd_buf;
  struct bs_l1_manifest cur_manifest;
  char * cmd_output;
  
  void * data;
};
struct bs_context g_ctx;
static void core_state_handler(unsigned char);

// Block thread until g_stat unlocked or time out.
static int wait_stat_unlocked(unsigned int timeout) {
  // TODO: lock for context
  unsigned int t = 1;
  while (g_stat_lock && t < timeout) {
    sleep(1);
    t += 100;
  }

  return !g_stat_lock;
}

static void lock() {
  g_stat_lock = 1;
}

static void unlock() {
  g_stat_lock = 0;
}
//---------------------------------------------------------------------------
// Communication with HMI
// 
//--------------------------------------------------------------------------- 
static void init_hmi_objs(struct bs_context * p_ctx) {

  // TODO: take values from p_ctx to init these values

  // To make private
  static struct bs_pkg_info _hmi_pkg_info;
  static struct bs_ecu_upgrade_stat _hmi_upgrade_stat;

  p_ctx->hmi_pkg_info = &_hmi_pkg_info;
  p_ctx->hmi_upgrade_stat = &_hmi_upgrade_stat;

  strcpy(_hmi_pkg_info.door_module, "no");
  strcpy(_hmi_pkg_info.dev_id, "xxx");
  strcpy(_hmi_pkg_info.soft_id, "wpc.1.0.0.0");
  strcpy(_hmi_pkg_info.release_notes, "N/A");

  strcpy(_hmi_upgrade_stat.dev_id, "xxx");
  strcpy(_hmi_upgrade_stat.soft_id, "wpc.1.0.0.0");
  strcpy(_hmi_upgrade_stat.esti_time, "00:00:00 01-01-1900");
  strcpy(_hmi_upgrade_stat.start_time, "00:00:00 01-01-1900");
  strcpy(_hmi_upgrade_stat.time_stamp, "00:00:00 01-01-1900");
  // "yes" or "no"
  strcpy(_hmi_upgrade_stat.door_module, "no");
  // "pending", "in progress", "failed", "success"
  strcpy(_hmi_upgrade_stat.status, "in progress");
  // raw percentage data, e.g., 0, 55, or 100
  _hmi_upgrade_stat.progress_percent = 0; 
}

static unsigned int hmi_resp_check_new_pkg(struct bs_context *p_ctx, char *msg, const char *uuid) {
  unsigned int pc = 0;
  char buf[512];
  static char *resp_header = "\"func-id\":1,\"category\":2,\"response\":{";
  // first 4 bytes for length
  pc += 4;

  // start {
  msg[pc] = '{';
  pc += 1;

  // header
  strcpy(msg + pc, resp_header);
  pc += strlen(resp_header);

  // door_module
  sprintf(buf, "\"door_module\":\"%s\",", p_ctx->hmi_pkg_info->door_module);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // dev_id
  sprintf(buf, "\"dev_id\":\"%s\",", p_ctx->hmi_pkg_info->dev_id);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // soft_id
  sprintf(buf, "\"soft_id\":\"%s\",", p_ctx->hmi_pkg_info->soft_id);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // release_notes
  sprintf(buf, "\"release_notes\":\"%s\"", p_ctx->hmi_pkg_info->release_notes);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // end '}' for pkg info
  msg[pc] = '}';
  pc += 1;

  msg[pc] = ',';
  pc += 1;

  // uuid
  sprintf(buf, "\"uuid\":\"%s\"", uuid);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // end '}'
  msg[pc] = '}';
  pc += 1;

  // add string end to keep safe
  msg[++pc] = 0;

  // the value of pc is the length
  msg[0] = (char) (pc<< 24);
  msg[1] = (char) (pc<< 16);
  msg[2] = (char) (pc<< 8); 
  msg[3] = (char) (pc);

  // debug
  LOG_PRINT(IDCM_LOG_LEVEL_INFO,"To HMI Msg: new pkg: %s\n", msg+4);

  return pc;
}

static unsigned int hmi_resp_start_upgrade(struct bs_context *p_ctx, char *msg, const char *uuid) {
  unsigned int pc = 0;
  char buf[128];
  static char *resp_header = "\"func-id\":2,\"category\":2,\"response\":{";
  // first 4 bytes for length
  pc += 4;

  // start {
  msg[pc] = '{';
  pc += 1;

  // header
  strcpy(msg + pc, resp_header);
  pc += strlen(resp_header);

  // dev_id
  sprintf(buf, "\"dev_id\":\"%s\",", p_ctx->hmi_pkg_info->dev_id);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // soft_id
  sprintf(buf, "\"soft_id\":\"%s\"", p_ctx->hmi_pkg_info->soft_id);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // end '}' for pkg info
  msg[pc] = '}';
  pc += 1;

  msg[pc] = ',';
  pc += 1;

  // uuid
  sprintf(buf, "\"uuid\":\"%s\"", uuid);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // end '}'
  msg[pc] = '}';
  pc += 1;

  // add string end to keep safe
  msg[++pc] = 0;

  // the value of pc is the length
  msg[0] = (char) (pc<< 24);
  msg[1] = (char) (pc<< 16);
  msg[2] = (char) (pc<< 8);
  msg[3] = (char) (pc);

  // debug
  LOG_PRINT(IDCM_LOG_LEVEL_INFO,"To HMI Msg: start upgrade: %s\n", msg+4);

  return pc;
}

// -1: TDR error | 1: progress 100% | 0: in progress
static unsigned int hmi_resp_upgrade_stat_update(struct bs_context *p_ctx, cJSON * json) {
  cJSON * iterator = NULL;

  iterator = json->child;
  while(iterator) {

    if (strcmp(iterator->string, "error") == 0) {
      //TODO response error
      LOG_PRINT(IDCM_LOG_LEVEL_INFO,"upgrade_stat from Orchestrator: tdr error: %s\n", iterator->valuestring);
      return -1;
    } else if (strcmp(iterator->string, "esti_time") == 0) {
      strcpy(p_ctx->hmi_upgrade_stat->esti_time, iterator->valuestring);
    } else if (strcmp(iterator->string, "start_time") == 0) {
      strcpy(p_ctx->hmi_upgrade_stat->start_time, iterator->valuestring);
    } else if (strcmp(iterator->string, "time_stamp") == 0) {
      strcpy(p_ctx->hmi_upgrade_stat->time_stamp, iterator->valuestring);
    } else if (strcmp(iterator->string, "status") == 0) {
      strcpy(p_ctx->hmi_upgrade_stat->status, iterator->valuestring);
    } else if (strcmp(iterator->string, "progress_percent") == 0) {
      p_ctx->hmi_upgrade_stat->progress_percent = iterator->valueint;
    }

    if (p_ctx->hmi_upgrade_stat->progress_percent >= 100) {
      return 1;
    }
    iterator = iterator->next;
  }

  LOG_PRINT(IDCM_LOG_LEVEL_INFO,"upgrade_stat from Orchstator: %s\n", cJSON_Print(json));
  return 0;
}


static unsigned int hmi_resp_upgrade_stat(struct bs_context *p_ctx, char *msg, const char* uuid) {
  unsigned int pc = 0; 
  static char *resp_header = "\"func-id\":3,\"category\":2,\"id\":\"xxx\",\"version\":\"1.0.0\",\"response\":[";
  char buf[128];
 
  // first 4 bytes for length
  pc += 4;

  // start {
  msg[pc] = '{';
  pc += 1;

  // header
  strcpy(msg + pc, resp_header);
  pc += strlen(resp_header);

  // start {
  msg[pc] = '{';
  pc += 1;

  // dev_id
  sprintf(buf, "\"dev_id\":\"%s\",", p_ctx->hmi_upgrade_stat->dev_id);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // soft_id
  sprintf(buf, "\"soft_id\":\"%s\",", p_ctx->hmi_upgrade_stat->soft_id);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // esti_time
  sprintf(buf, "\"esti_time\":\"%s\",", p_ctx->hmi_upgrade_stat->esti_time);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // start_time
  sprintf(buf, "\"start_time\":\"%s\",", p_ctx->hmi_upgrade_stat->start_time);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // time_stamp
  sprintf(buf, "\"time_stamp\":\"%s\",", p_ctx->hmi_upgrade_stat->time_stamp);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // door_module
  sprintf(buf, "\"door_module\":\"%s\",", p_ctx->hmi_upgrade_stat->door_module);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // status
  sprintf(buf, "\"status\":\"%s\",", p_ctx->hmi_upgrade_stat->status);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // progress
  sprintf(buf, "\"progress_percent\":%d", (int)(p_ctx->hmi_upgrade_stat->progress_percent));
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // end '}' for pkg status
  msg[pc] = '}';
  pc += 1;

  // end ']' for pkg status
  msg[pc] = ']';
  pc += 1;
  msg[pc] = ',';
  pc += 1;

  // uuid
  sprintf(buf, "\"uuid\":\"%s\"", uuid);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // end '}'
  msg[pc] = '}';
  pc += 1;

  // add string end to keep safe
  msg[++pc] = 0;

  // the value of pc is the length
  msg[0] = (char) (pc<< 24);
  msg[1] = (char) (pc<< 16);
  msg[2] = (char) (pc<< 8); 
  msg[3] = (char) (pc);

  // debug
  LOG_PRINT(IDCM_LOG_LEVEL_INFO,"HMI Msg: send upgrade_stat: %s\n", msg+4);

  return pc;  
}

// parse JSON format payload
static int hmi_payload_parser(struct bs_context *p_ctx, char* payload, unsigned int len) { 
  int result = 1;
  int resp_len = 0;
  static char response[1024];
  struct cJSON * root = NULL;
  struct cJSON * iterator = NULL;
  int func_id_code = 0;
  static char uuid[64];

  (void) len;

  LOG_PRINT(IDCM_LOG_LEVEL_INFO,"HMI Msg: recv raw: %s\n", payload);

  root = cJSON_Parse(payload);

  if (root == NULL)
      return 0;

  // TODO: validate whole JSON
  iterator = root->child;
  while(iterator) {
    if (strcmp(iterator->string, "func-id") == 0) { 
      if (!cJSON_IsNumber(iterator))
        goto last_step;
      func_id_code = iterator->valueint;
    }

    if (strcmp(iterator->string, "uuid") == 0) { 
      strcpy(uuid, iterator->valuestring);
    }

    iterator = iterator->next;
  }
  // TODO: synthesize response acoording to coming in value
  switch(func_id_code) {
    case START_UPGRADE:
      LOG_PRINT(IDCM_LOG_LEVEL_INFO, "HMI Msg: recv START_UPGRADE\n");
      
      resp_len = hmi_resp_start_upgrade(p_ctx, response, uuid);
      if (g_stat == ORCH_PKG_READY) {
        wait_stat_unlocked(1000);
        lock();
        core_state_handler(ORCH_PKG_READY);
        unlock();
        mg_send(p_ctx->hmi, response, resp_len);
      }

      // TODO: handle if g_stat id not ORCH_PKG_READY 
      break;
    case UPGRADE_PROGRESS:
      resp_len = hmi_resp_upgrade_stat(p_ctx, response, "7950f8e2-6cd4-11ea-a9a9-571f576d565b"); // TODO: generate uuid
      mg_send(p_ctx->hmi, response, resp_len);
      break;
    case CHECK_NEW_PACKAGE:
      LOG_PRINT(IDCM_LOG_LEVEL_INFO,"HMI msg: recv CHECK_NEW_PACKAGE\n");
      resp_len = hmi_resp_check_new_pkg(p_ctx, response, uuid);
      mg_send(p_ctx->hmi, response, resp_len);
      break;
    default:
      // unknown item
      result = 0;
      goto last_step;
      break;
  }

last_step:
  // release memory
  cJSON_Delete(root);
  return result; 
}

static void hmi_msg_handler(struct mg_connection *nc, int ev, void *p) {
  unsigned int len = 0;
  struct mbuf *io = &nc->recv_mbuf;
  (void) p;

  switch (ev) {
    case MG_EV_ACCEPT:
      g_ctx.hmi = nc;
      LOG_PRINT(IDCM_LOG_LEVEL_INFO,"HMI Msg: socket connected\n");
      break;
    case MG_EV_RECV:
      // first 4 bytes for length
      len = io->buf[3] + (io->buf[2] << 8) + (io->buf[1] << 16) + (io->buf[0] << 24);
      hmi_payload_parser(&g_ctx, io->buf+4, len);
      break;
    default:
      break;
  }
}

static void * hmi_thread(void * param) {
  struct mg_mgr mgr;
  struct bs_context * p_ctx = (struct bs_context *) param;

  mg_mgr_init(&mgr, NULL);

  LOG_PRINT(IDCM_LOG_LEVEL_INFO,"===Start socket server for HMI ===\n");
    // by default, listen to 3001
    mg_bind(&mgr, "3001", hmi_msg_handler);
    LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Listen on port 3001 for HMI Msg\n");

  while (!p_ctx->hmi_thread_exit) {
    mg_mgr_poll(&mgr, 1000);
  }
  mg_mgr_free(&mgr);

  return 0;
}

static int hmi_thread_run(struct bs_context * p_ctx) {
  mg_start_thread(hmi_thread, (void *) p_ctx);
  return 1;
}

//---------------------------------------------------------------------------
// Communication with DMC
//--------------------------------------------------------------------------- 
static int dmc_tftp_run(struct bs_context * p_ctx) {
  // TODO: 1. format message according convention with SocketWrapper
  // TODO: 2. need root right to listen port 69?
  char * succ = "{ \"DLC pkg new\": \"Downloader start to run\"}";
  char * fail = "{ \"DLC pkg new\": \"Downloader failed to run\"}";

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
  strcat(p_ctx->cmd_buf, p_ctx->pkg_url);    

  if ((fp = popen(p_ctx->cmd_buf, "r")) != NULL) {
//    mg_send(p_ctx->dmc, succ, strlen(succ));
    (void) succ;
  } else {
//    mg_send(p_ctx->dmc, fail, strlen(fail));
    (void) fail;
    return NULL;
  }

  LOG_PRINT(IDCM_LOG_LEVEL_INFO,"TLC downloading: curl start downling!\n");
  while (!p_ctx->downloader_thread_exit &&
         fgets(p_ctx->cmd_output, 1024, fp) != NULL) {
    // block until has lock
    if (!wait_stat_unlocked(10000))
      LOG_PRINT(IDCM_LOG_LEVEL_INFO,"TLC downloading: lock timeout in curl downling!\n"); 
   // TODO: need better way to check successful downloading  
  }
  LOG_PRINT(IDCM_LOG_LEVEL_INFO,"TLC downloading: curl finished downloading!\n");

  // block until has lock
  if (!wait_stat_unlocked(1000))
    LOG_PRINT(IDCM_LOG_LEVEL_INFO,"TLC downloading: lock timeout!\n");

  g_stat_lock = 1;
  if (pclose(fp) == 0) {
    core_state_handler(DLC_PKG_READY);
  }
  g_stat_lock = 0;

  return NULL;
}

static int dmc_downloader_run(struct bs_context * p_ctx) {
  mg_start_thread(dmc_downloader_thread, (void *) p_ctx);
  return 1;
}

static struct cJSON * find_json_child(struct cJSON * root, char * label)
{
  struct cJSON * iterator = NULL;

  iterator = root->child;
  while(iterator) {
    if (strcmp(iterator->string, label) == 0) {
      return iterator;
    }
    iterator = iterator->next;
  }
  return iterator;
}


static int dmc_msg_parse(const char *json) {
  unsigned char next_stat = STAT_INVALID;
  struct cJSON * root = NULL;
  struct cJSON * iterator = NULL;

  // parse L1 Menifest
  root = cJSON_Parse(json);
  if (root == NULL) {
    return next_stat;
  }

  iterator = find_json_child(root, "fotaCertUrl");
  if (iterator == NULL) {
    return next_stat;
  }
  // save downloading url. TODO: in parallel downloaing
  strcpy(g_ctx.cur_manifest.pkg_cdn_url, iterator->valuestring);
  g_ctx.pkg_url = g_ctx.cur_manifest.pkg_cdn_url;
  // TODO: parse out each part
  // parse devid 
  strcpy(g_ctx.cur_manifest.dev_id, "VDCM");  

//finish_parse:
  cJSON_Delete(root);
  return next_stat;
}

static void dmc_msg_handler(struct mg_connection *nc, int ev, void *p) {
  struct mbuf *io = &nc->recv_mbuf;
  unsigned int len = 0;
  (void) p;

  switch (ev) {
    case MG_EV_ACCEPT:
      g_ctx.dmc = nc;
//      dmc_tftp_run(&g_ctx);
      hmi_thread_run(&g_ctx);
      LOG_PRINT(IDCM_LOG_LEVEL_INFO,"DMC Socket Wrapper connected!\n");
//      core_state_handler(DLC_PKG_NEW);//TODO: it's only for test, drop it
      break;
    case MG_EV_RECV:
      LOG_PRINT(IDCM_LOG_LEVEL_INFO,"-----Received Raw Message from DMC----\n");
      LOG_PRINT(IDCM_LOG_LEVEL_INFO,"%s \n", &(io->buf[4]));
      // first 4 bytes for length
      len = io->buf[3] + (io->buf[2] << 8) + (io->buf[1] << 16) + (io->buf[0] << 24);   
      //TODO: parse JSON
      (void) len;
      core_state_handler(dmc_msg_parse(io->buf+4));
      mbuf_remove(io, io->len);       // Discard message from recv buffer
      break;
    default:
      break;
  }
}

//---------------------------------------------------------------------------
// Communication with Orchestrator on CGW
//--------------------------------------------------------------------------- 
static void * cgw_pkg_upload_thread(void *param) {
  struct bs_context *p_ctx = (struct bs_context *) param;
  FILE *fp;
  static char cmd[128];
  char *curl = "curl -F 'data=@./wpc.1.0.0' ";
  int i;

  (void) param;
 
  for(i = 0; i < 128; i++) {
    cmd[i] = 0;
  }
  // TODO: obtain the package name from g_ctx
  strcpy(cmd, curl);
  strcat(cmd, p_ctx->cgw_api_pkg_upload.api);

  if ((fp = popen(cmd, "r")) != NULL) {
    LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Uploading pkg to Orchestrator: curl start to upload.\n");
  } else {
    LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Uploading pkg to Orchestrator: curl failed to upload.\n");
  }

  //TODO: check output
  while (!p_ctx->downloader_thread_exit &&
         fgets(p_ctx->cmd_output, 1024, fp) != NULL) {
  }

  lock();
  g_stat = ORCH_PKG_READY;//HMI will triger orchestrator to do upgrading
  unlock();  
  LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Uploading pkg to Orchestrator: uploading finished!\n");

  return NULL;
}

static void cgw_handler_pkg_new(struct mg_connection *nc, int ev, void *ev_data) {
  struct http_message *hm = (struct http_message *) ev_data;
  (void) hm;

  switch (ev) {
    case MG_EV_CONNECT:
      g_ctx.cgw_api_pkg_new.nc = nc;
      g_stat = ORCH_PKG_DOWNLOADING;
      LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Request Orchestrator pkg new: connected\n");
      break;
    case MG_EV_HTTP_REPLY:
      //TODO: parse JSON, if error then g_stat = ORCH_NET_ERR;
      g_stat = ORCH_PKG_DOWNLOADING;
      mg_start_thread(cgw_pkg_upload_thread, (void *) &g_ctx);
      nc->flags |= MG_F_CLOSE_IMMEDIATELY;
      g_ctx.cgw_api_pkg_new.cgw_thread_exit = 1;
      break;
    case MG_EV_CLOSE:
      g_ctx.cgw_api_pkg_new.nc = NULL;
      if (g_ctx.cgw_api_pkg_new.cgw_thread_exit == 0) {
        LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Request Orchestrator pkg new: connection closed\n");
        g_ctx.cgw_api_pkg_new.cgw_thread_exit = 1;
      }
      break;
    default:
      break;
  }
}

static void cgw_handler_pkg_stat(struct mg_connection *nc, int ev, void *ev_data) {
  struct http_message *hm = (struct http_message *) ev_data;

  switch (ev) {
    case MG_EV_CONNECT:
      if (*(int *) ev_data != 0) {
        LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Request Orchestrator pkg status: connect failed\n");
        g_stat = ORCH_PKG_BAD;
        g_ctx.cgw_api_pkg_stat.cgw_thread_exit = 1;
      } else {
        g_ctx.cgw_api_pkg_stat.nc = nc;
        g_stat = ORCH_PKG_DOWNLOADING;
        LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Request Orchestrator pkg status: connected\n");
      }
      break;
    case MG_EV_HTTP_REPLY:
      fwrite(hm->body.p, 1, hm->body.len, stdout);
      //TODO: parse JSON, if error then g_stat = ORCH_NET_ERR;
      //g_stat = ORCH_PKG_DOWNLOADING;
      if (strstr(hm->body.p, "succ") != NULL) {
        //g_stat = ORCH_PKG_READY;
        LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Request Orchestrator pkg status: pkg ready\n");
        nc->flags |= MG_F_CLOSE_IMMEDIATELY;
        g_ctx.cgw_api_pkg_stat.cgw_thread_exit = 1;
      } else {
        g_stat = ORCH_PKG_DOWNLOADING;
        LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Request Orchestrator pkg status: downloading\n");
      }

      break;
    case MG_EV_CLOSE:
      g_ctx.cgw_api_pkg_stat.nc = NULL;
      if (g_ctx.cgw_api_pkg_stat.cgw_thread_exit == 0) {
        LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Request Orchestrator pkg status: closed connection\n");
        g_ctx.cgw_api_pkg_stat.cgw_thread_exit = 1;
      }
      break;
    default:
      break;
  }
}

static void cgw_handler_tdr_run(struct mg_connection *nc, int ev, void *ev_data) {
  struct http_message *hm = (struct http_message *) ev_data;
  struct cJSON * res = NULL;
  struct cJSON * iterator = NULL;
  char * info = NULL;

  (void) info;

  switch (ev) {
    case MG_EV_CONNECT:
      g_ctx.cgw_api_tdr_run.nc = nc;
      core_state_handler(ORCH_TDR_RUN);
      LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Request Orchestrator tdr run: connected\n");
      break;
    case MG_EV_HTTP_REPLY:
      //TODO: parse JSON, if error then g_stat = ORCH_NET_ERR;
      // g_stat = ORCH_TDR_FAIL;
      res = cJSON_Parse(hm->body.p);
      iterator = res->child;
      while(iterator) {
        if (strcmp(iterator->string, "error") == 0) {
          info = iterator->valuestring;
        }
        iterator = iterator->next;
      }
      LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Request Orchestrator tdr run: %s\n", info);
      
      //if (strstr(info, "succ") != NULL) {
      //  core_state_handler(ORCH_TDR_SUCC);
      //  nc->flags |= MG_F_CLOSE_IMMEDIATELY;
      //  g_ctx.cgw_api_tdr_run.cgw_thread_exit = 1;
      //} else if (strstr(info, "fail") != NULL) {
      //  g_stat = ORCH_TDR_FAIL;
      //  nc->flags |= MG_F_CLOSE_IMMEDIATELY;
      //  g_ctx.cgw_api_tdr_run.cgw_thread_exit = 1;
      //} else {
        //TODO: report update progress  
      //  g_stat = ORCH_TDR_RUN;
      //}

      break;
    case MG_EV_CLOSE:
      g_ctx.cgw_api_tdr_run.nc = NULL;
      if (g_ctx.cgw_api_tdr_run.cgw_thread_exit == 0) {
        LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Request Orchestrator tdr run: closed connection\n");
        g_ctx.cgw_api_tdr_run.cgw_thread_exit = 1;
      }
      break;
    default:
      break;
  }
}

static void cgw_handler_tdr_stat(struct mg_connection *nc, int ev, void *ev_data) {
  struct http_message *hm = (struct http_message *) ev_data;
  struct cJSON * res = NULL;
  struct cJSON * result = NULL;
  int upgrade_stat = 0;
  // TODO: move to hmi thread
  int resp_len = 0;
  static char response[512];

  switch (ev) {
    case MG_EV_CONNECT:
      g_ctx.cgw_api_tdr_stat.nc = nc;
      if (*(int *) ev_data != 0) {
        LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Request Orchestrator tdr status: connect failed\n");
        g_stat = ORCH_CON_ERR;
        g_ctx.cgw_api_tdr_stat.cgw_thread_exit = 1;
      } else {
        g_ctx.cgw_api_tdr_stat.nc = nc;
        g_stat = ORCH_TDR_RUN;
        LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Request Orchestrator tdr stat: connected to send msg\n");
      }
      break;
    case MG_EV_HTTP_REPLY:
      LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Request Orchestrator tdr stat: response: %s\n", hm->body.p);
      //TODO: parse JSON, if error then g_stat = ORCH_TDR_FAIL;
      res = cJSON_Parse(hm->body.p);
      if (!res || !(result = cJSON_GetObjectItem(res, "result"))) {
        LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Request Orchestrator tdr status: bad json\n");
        break;
      }

      nc->flags |= MG_F_CLOSE_IMMEDIATELY;
      g_ctx.cgw_api_tdr_stat.cgw_thread_exit = 1;

      upgrade_stat = hmi_resp_upgrade_stat_update(&g_ctx, result);
      if (upgrade_stat < 0) {
        core_state_handler(ORCH_TDR_FAIL);
      } else if (upgrade_stat == 0) {
        // TODO: move response to hmi thread
        resp_len = hmi_resp_upgrade_stat(&g_ctx, response, "7950f8e2-6cd4-11ea-a9a9-571f576d565b"); // TODO: generate uuid
        mg_send(g_ctx.hmi, response, resp_len);
        core_state_handler(ORCH_TDR_RUN);
      } else if (upgrade_stat > 0) {
        core_state_handler(ORCH_TDR_SUCC);
      }

      cJSON_Delete(res);
      break;
    case MG_EV_CLOSE:
      g_ctx.cgw_api_tdr_stat.nc = NULL;
      if (g_ctx.cgw_api_tdr_stat.cgw_thread_exit == 0) {
        LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Request Orchestrator tdr status: closed connection\n");
        g_ctx.cgw_api_tdr_stat.cgw_thread_exit = 1;
      }
      break;
    default:
      break;
  }
}

static char * cgw_api_payload_pkg_new() {
  // TODO: gen payload based on g_ctx
  static char buf[256];
  sprintf(buf, "{\"dev_id\":\"%s\",\"payload\":{\"url\":\"%s\"}}",
          g_ctx.cur_manifest.dev_id, g_ctx.cur_manifest.pkg_cdn_url);
  return buf;
}

static char * cgw_api_payload_default() {
  // TODO: gen payload based on g_ctx 
  return "{\"dev_id\":\"VDCM\"}}";
}

static void * cgw_msg_thread(void *param) {
  struct mg_mgr mgr;
  struct bs_cgw_api_handler * handler = (struct bs_cgw_api_handler *) param;
  char *post_data = handler->payload_gener();// TODO: the payload should be generated dynamically

  mg_mgr_init(&mgr, NULL);
  mg_connect_http(&mgr, handler->fn, handler->api,
                  "Content-Type: application/json\r\n",
                  post_data);

  while (handler->cgw_thread_exit == 0) {
    if (!wait_stat_unlocked(1000)) {
      LOG_PRINT(IDCM_LOG_LEVEL_INFO,"CGW Msg thread: lock timeout\n");
    }

    // run until one api request end
    g_stat_lock = 1;
    mg_mgr_poll(&mgr, 500);
    g_stat_lock = 0;
  }

  handler->cgw_thread_exit = 0;
  mg_mgr_free(&mgr);
  return NULL;
}

//---------------------------------------------------------------------------
// Core state machine
// Set g_stat_lock=1 before invoking this function, then reset it to 0
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
      LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Core stat: dlc pgk ready\n");
      mg_start_thread(cgw_msg_thread, (void *) &(g_ctx.cgw_api_pkg_new)); 
      break;
    case ORCH_CON_ERR:
      break;
    case ORCH_PKG_DOWNLOADING:
      if (g_ctx.cgw_api_pkg_stat.nc == NULL)
        mg_start_thread(cgw_msg_thread, (void *) &(g_ctx.cgw_api_pkg_stat)); 
      break;
    case ORCH_PKG_BAD:
      break;
    case ORCH_PKG_READY:
      mg_start_thread(cgw_msg_thread, (void *) &(g_ctx.cgw_api_tdr_run)); 
      break;
    case ORCH_TDR_RUN:
      // TODO: protect two instances running in parallel
      mg_start_thread(cgw_msg_thread, (void *) &(g_ctx.cgw_api_tdr_stat)); 
      break;
    case ORCH_TDR_FAIL:
      mg_send(g_ctx.dmc, "{\"result\":\"TDR run failed\"}\n", 31);
      break;
    case ORCH_TDR_SUCC:
      mg_send(g_ctx.dmc, "{\"result\":\"TDR run succesful\"}\n", 31);
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
  g_ctx.hmi = NULL;

  g_ctx.hmi_thread_exit = 0;
  g_ctx.downloader_thread_exit = 0;

  init_hmi_objs(&g_ctx);

  // CGW http API
  g_ctx.cgw_api_pkg_new.api = "http://127.0.0.1:8018/pkg/new";
  g_ctx.cgw_api_pkg_new.cgw_thread_exit = 0;
  g_ctx.cgw_api_pkg_new.fn = cgw_handler_pkg_new;
  g_ctx.cgw_api_pkg_new.payload_gener = cgw_api_payload_pkg_new;
  g_ctx.cgw_api_pkg_new.nc = NULL;
  g_ctx.cgw_api_pkg_stat.api = "http://127.0.0.1:8018/pkg/sta";
  g_ctx.cgw_api_pkg_stat.cgw_thread_exit = 0;
  g_ctx.cgw_api_pkg_stat.fn = cgw_handler_pkg_stat;
  g_ctx.cgw_api_pkg_new.payload_gener = cgw_api_payload_default;
  g_ctx.cgw_api_pkg_stat.nc = NULL;
  g_ctx.cgw_api_tdr_run.api = "http://127.0.0.1:8018/tdr/run";
  g_ctx.cgw_api_tdr_run.cgw_thread_exit = 0;
  g_ctx.cgw_api_tdr_run.fn = cgw_handler_tdr_run;
  g_ctx.cgw_api_pkg_new.payload_gener = cgw_api_payload_default;
  g_ctx.cgw_api_tdr_run.nc = NULL;
  g_ctx.cgw_api_tdr_stat.api = "http://127.0.0.1:8018/tdr/stat";
  g_ctx.cgw_api_tdr_stat.cgw_thread_exit = 0;
  g_ctx.cgw_api_tdr_stat.fn = cgw_handler_tdr_stat;
  g_ctx.cgw_api_pkg_new.payload_gener = cgw_api_payload_default;
  g_ctx.cgw_api_tdr_stat.nc = NULL;
  // TODO: so far use curl to upload
  g_ctx.cgw_api_pkg_upload.api = "http://127.0.0.1:8018/upload";
  g_ctx.cgw_api_pkg_upload.fn = NULL;
  g_ctx.cgw_api_pkg_upload.nc = NULL;
  g_ctx.cgw_api_pkg_new.payload_gener = NULL;

  g_ctx.cmd_buf = g_cmd_buf;
  g_ctx.cmd_output = g_cmd_output;

  g_ctx.pkg_url = "ftp://speedtest.tele2.net/1KB.zip";

  memset(g_ctx.cur_manifest.dev_id, 0, 32);
  memset(g_ctx.cur_manifest.pkg_cdn_url, 0, 128);
  
  g_ctx.downloader = "curl --output wpc.1.0.0"; 
//  g_ctx.downloader = "/data/duc/test_interface/dlc";

  // Change to FTPS
  g_ctx.tftp_server = "sudo ./tftpserver"; 
}

int main(int argc, char *argv[]) {
  struct mg_mgr mgr;

  init_context();

  mg_mgr_init(&mgr, NULL);

  LOG_PRINT(IDCM_LOG_LEVEL_INFO,"=== Start socket server ===\n");
  if (argc >= 2 && strcmp(argv[1], "-o") == 0) {
    // Listen to specidied port
    mg_bind(&mgr, argv[2], dmc_msg_handler);
    LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Listen on port %s\n", argv[2]);
  } else {
    // by default, listen to 3000
    mg_bind(&mgr, "3000", dmc_msg_handler);
    LOG_PRINT(IDCM_LOG_LEVEL_INFO,"Listen on port 3000\n");
  }

  for (;;) {
    if (g_stat_lock) {
      sleep(1);
      continue;
    }

    g_stat_lock = 1;
    mg_mgr_poll(&mgr, 1000);
    g_stat_lock = 0;
  }
  mg_mgr_free(&mgr);

  return 0;
}
