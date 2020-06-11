#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bs_dlc_utils.h"
#include "log/idcm_log.h"
#include "cJSON/cJSON.h"


int bs_parse_l1_manifest(const char* json_txt, bs_l1_manifest_t* l1_mani)
{
    assert(json_txt);
    assert(l1_mani);

    LOG_PRINT(IDCM_LOG_LEVEL_INFO, "--- parse l1_manifest enter -----\n");
    //LOG_PRINT(IDCM_LOG_LEVEL_INFO, "%s", json_txt);
    //LOG_PRINT(IDCM_LOG_LEVEL_INFO, "---------------------------------\n");

    int rc = JCFG_ERR_OK;
    memset(l1_mani, 0, sizeof(bs_l1_manifest_t));
    l1_mani->pkg_num = 0;

    struct cJSON* root = NULL;
    struct cJSON* manifest = NULL;
    struct cJSON* packages = NULL;

    root = cJSON_Parse(json_txt);
    if (NULL == root) {
        LOG_PRINT(IDCM_LOG_LEVEL_ERROR, "parse l1_mainfest document failed");
        rc = JCFG_ERR_BAD_DOCUMENT;
        goto DONE;
    }
    manifest = cJSON_GetObjectItem(root, "manifest");
    if (!cJSON_IsObject(manifest)) {
        LOG_PRINT(IDCM_LOG_LEVEL_ERROR, "[/manifest] object not find");        
        rc = JCFG_ERR_BAD_DOCUMENT;
        goto DONE;
    }
    packages = cJSON_GetObjectItem(manifest, "packages");
    if (!cJSON_IsArray(packages)) {
        LOG_PRINT(IDCM_LOG_LEVEL_ERROR, "[/manifest/packages] array not find");
        rc = JCFG_ERR_BAD_DOCUMENT;
        goto DONE;
    }

    if (cJSON_GetArraySize(packages) > BS_MAX_MANI_PKG_NUM) {
        LOG_PRINT(IDCM_LOG_LEVEL_ERROR, 
            "[/manifest/packages].size=%d larger then limit=%d", 
            cJSON_GetArraySize(packages) , BS_MAX_MANI_PKG_NUM);
        rc = JCFG_ERR_BAD_DOCUMENT;
        goto DONE;
    }
    for (int i = 0; i < cJSON_GetArraySize(packages); ++i) {
        struct cJSON* jpkg = cJSON_GetArrayItem(packages, i);
        assert(jpkg);

        struct cJSON* ecu = cJSON_GetObjectItem(jpkg, "ecu");
        if (!cJSON_IsString(ecu)) {
            LOG_PRINT(IDCM_LOG_LEVEL_ERROR, 
                "[/manifest/packages/[%d]/ecu]] string element not find", i + 1);
            rc = JCFG_ERR_BAD_DOCUMENT;
            goto DONE;
        }
        struct cJSON* typ = cJSON_GetObjectItem(jpkg, "deviceType");
        if (!cJSON_IsString(typ)) {
            LOG_PRINT(IDCM_LOG_LEVEL_ERROR, 
                "[/manifest/packages/[%d]/deviceType]] string element not find", i + 1);
            rc = JCFG_ERR_BAD_DOCUMENT;
            goto DONE;
        }
        struct cJSON* res = cJSON_GetObjectItem(jpkg, "resources");
        if (!cJSON_IsObject(res)) {
            LOG_PRINT(IDCM_LOG_LEVEL_ERROR,
                "[/manifest/packages/[%d]/resources]] object not find", i + 1);
            rc = JCFG_ERR_BAD_DOCUMENT;
            goto DONE;
        }
        struct cJSON* sum = cJSON_GetObjectItem(res, "fullDownloadChecksum");
        if (!cJSON_IsString(sum)) {
            LOG_PRINT(IDCM_LOG_LEVEL_ERROR,
                "[/manifest/packages/[%d]/resources/fullDownloadChecksum]] string element not find", i + 1);
            rc = JCFG_ERR_BAD_DOCUMENT;
            goto DONE;
        }
        struct cJSON* url = cJSON_GetObjectItem(res, "fullDownloadUrl");
        if (!cJSON_IsString(url)) {
            LOG_PRINT(IDCM_LOG_LEVEL_ERROR,
                "[/manifest/packages/[%d]/resources/fullDownloadUrl]] string element not find", i + 1);
            rc = JCFG_ERR_BAD_DOCUMENT;
            goto DONE;
        }
        struct cJSON* siz = cJSON_GetObjectItem(res, "fullSize");
        if (!cJSON_IsNumber(siz)) {
            LOG_PRINT(IDCM_LOG_LEVEL_ERROR,
                "[/manifest/packages/[%d]/resources/fullSize]] number element not find", i + 1);
            rc = JCFG_ERR_BAD_DOCUMENT;
            goto DONE;
        }


        bs_l1_manifest_pkg_t* pkg = &l1_mani->packages[i];
        if (!strcmp("eth", typ->valuestring)) {
            pkg->dev_type = BS_DEV_TYPE_ETH;
        }
        else if (!strcmp("can", typ->valuestring)) {
            pkg->dev_type = BS_DEV_TYPE_CAN;
        }
        else if (!strcmp("can-fd", typ->valuestring)) {
            pkg->dev_type = BS_DEV_TYPE_CAN_FD;
        }
        else {
            LOG_PRINT(IDCM_LOG_LEVEL_ERROR,
                "[/manifest/packages/[%d]/deviceType=%s]] can be recognized", 
                i + 1, typ->valuestring);
            rc = JCFG_ERR_BAD_DOCUMENT;
            goto DONE;
        }

        strncpy(pkg->dev_id, ecu->valuestring, BS_MAX_DEV_ID_LEN);
        strncpy(pkg->pkg_url, url->valuestring, BS_MAX_PKG_URL_LEN);
        strncpy(pkg->chk_sum, sum->valuestring, BS_PKG_CHK_SUM_LEN);
        pkg->pkg_siz = siz->valueint;

        //
        l1_mani->pkg_num++;

    }//foreach pkg in packages

DONE:
    if (root)
        cJSON_Delete(root);

    LOG_PRINT(IDCM_LOG_LEVEL_INFO,
        "--- parse l1_manifest %s----\n"
        "---------------------------------\n", JCFG_ERR_OK==rc ? "succ" : "fail");

    return (rc);
}

