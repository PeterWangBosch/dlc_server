#ifndef _BS_DLC_UTILS_H_
#define _BS_DLC_UTILS_H_

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef enum err_json_cfg_e {

    JCFG_ERR_OK = 0,
    JCFG_ERR_BAD_DOCUMENT,//invalid json document
    JCFG_ERR_SYSTEM_CALL_FAIL,//system call failed
    JCFG_ERR_ALLOC_MEM_FAIL,//mem alloc failed

}err_json_cfg_t;
//
typedef enum bs_dev_type_e {

    BS_DEV_TYPE_NONE = 0,//!not initized
    BS_DEV_TYPE_ETH,
    BS_DEV_TYPE_CAN,
    BS_DEV_TYPE_CAN_FD,

}bs_dev_type_e_t;
//
#define BS_MAX_MANI_PKG_NUM 4
#define BS_MAX_DEV_ID_LEN   32
#define BS_PKG_CHK_SUM_LEN  32
#define BS_MAX_PKG_URL_LEN  128
//
#define BS_CFG_MAX_APP_TXT_LEN 2048
typedef struct bs_device_app bs_device_app_t;

//
typedef struct bs_l1_manifest_pkg_s {
    int dev_type;//bs_dev_type_e_t
    char dev_id[BS_MAX_DEV_ID_LEN + 1];
    char chk_sum[BS_PKG_CHK_SUM_LEN + 1];
    char pkg_url[BS_MAX_PKG_URL_LEN + 1];
    int pkg_siz;
}bs_l1_manifest_pkg_t;
//
typedef struct bs_l1_manifest_s {

    int pkg_num;
    bs_l1_manifest_pkg_t packages[BS_MAX_MANI_PKG_NUM];
}bs_l1_manifest_t;


int bs_parse_l1_manifest(const char* json_txt, bs_l1_manifest_t* l1_mani);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* _BS_DLC_UTILS_H_ */

