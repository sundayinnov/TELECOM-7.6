/**********************************************************************
 * Copyright (C) 2017-2017  Unisound
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 **********************************************************************
 *
 * Description : uni_study_session.c
 * Author      : yuanshifeng@unisound.com
 * Date        : 2020.12.14
 *
 **********************************************************************/

#include "uni_study_session.h"

#include "uni_fsm.h"
#include "uni_session.h"
#include "uni_session_priority.h"
#include "uni_session_manage.h"
#include "uni_recog_service.h"
#include "uni_media_player.h"
#include "uni_nlu.h"
#include "uni_log.h"
#include "idle_detect.h"
#include "uni_black_board.h"
#include "uni_media_player.h"
#include "uni_event.h"
#include "uni_event_route.h"
#include "uni_config.h"
#include "uni_pcm_default.h"
#include "uni_user_meeting.h"
#include "aik-id.h"
#include "aik-id-args.h"
#include "user_flash.h"
#include "uni_hal_flash.h"
#include "uni_hal_watchdog.h"

#define TAG "study_session"

extern const unsigned char global_kws_lp_grammar[];
extern const uni_u32 global_kws_lp_grammar_size;
extern char g_mp3_ais_share_buf[];
extern const uni_u32 g_mp3_ais_share_buf_size;
extern const unsigned char global_kws_lp_study_grammar[];
extern const char *const g_nlu_content_str[];

#define MAX_STUDY_NODES_NUM  16
#define MAX_TMP_GRAMMAR_SIZE 356

static uni_bool g_clear_pending = FALSE;
static uni_bool g_clear_is_wakeup = FALSE;

/** 学习命令词序号表
 * 如:
 * eCMD_ac_power_on,
 * eCMD_ac_power_off,
 * eCMD_ac_speed_inc,
 * eCMD_ac_speed_dec,
 */
const uni_u8 g_study_cmd_list[] = {
  eCMD_come,
  eCMD_away,
};

/** 学习命令词播报pcm列表
 * 如:
 * "136",
 * "137",
 * "138",
 * "139",
 */
const char *g_study_cmd_reply_pcm_list[] = {
  "518",
  "517",
};

#define MAX_STUDY_CMD (sizeof(g_study_cmd_list) / sizeof(uni_u8))

#define BLOB_KEY_HAS_NEW_GRAMMAR "M_GRAM"
#define BLOB_KEY_NEW_GRAMMAR_LEN "M_GRAM_LEN"
#define BLOB_KEY_STUDY_NODE_NUM  "STUDY_NODE_NUM"
#define BLOB_TYPE_BOOL           uni_bool
#define BLOB_TYPE_U32            uni_u32

#define STUDY_PCM_SUCCESS      2  //学习成功
#define STUDY_PCM_FAIL_RETRY   3  //学习失败，请重试一次
#define STUDY_PCM_FINISH       4  //学习完成
#define STUDY_PCM_FAIL_EXIT    5  //学习异常，已退出学习模式
#define STUDY_PCM_ADVICE_RESET 1  //已有学习数据，请说重置学习进行重置
#define STUDY_PCM_START_WAKEUP 0  //开始学习唤醒词，请在安静环境下说话

#define STUDY_TIMEOUT_MS   (20000)   // 20秒
static uni_s64 g_study_start_time = 0;

static Result _idle__vui_app_study(void *event);
static Result _studying__vui_app_study_result(void *event);
static Result _start__audio_play_end(void *event);
static Result _clear__audio_play_end(void *event);
static Result _studying__audio_play_end(void *event);
static Result _finish__audio_play_end(void *event);
static Result _idle__audio_play_end(void *event);

typedef enum {
  STATE_IDLE = 0,
  STATE_START,
  STATE_CLEAR,
  STATE_FINISH,
  STATE_STUDYING,
} StudySessionState;

typedef struct {
  char tmp_grammar[MAX_TMP_GRAMMAR_SIZE];
  uni_u32 tmp_grammar_size;
  uni_u32 command_hash;
  uni_u8 nlu_cmd_index;
} StudyTmpGrammarNode;

typedef struct {
  Session *session;
  MicroFsmStruct *fsm;
  char *pcm_list;
  uni_bool enter_studying;
  uni_bool is_study_wakeup;
  uni_u8 succ_index;
  uni_u8 study_index;
  uni_u8 failed_count;
  uni_list_head head;
  StudyTmpGrammarNode node[COLLECT_STUDY_RESULT_NUM];
} StudySession;

static Scene g_study_scene = {UNI_LSTUDY_MODE,
                              DEFAULT_RECOGN_SCENE_TIMEOUT,
                              DEFAULT_WAKEUP_SCENE_LOW_THRESHOLD,
                              DEFAULT_WAKEUP_SCENE_STD_THRESHOLD,
                              320,
                              NULL};
static Scene g_wakeup_scene = {UNI_LP_WAKEUP,
                               0,
                               DEFAULT_WAKEUP_SCENE_LOW_THRESHOLD,
                               DEFAULT_WAKEUP_SCENE_STD_THRESHOLD,
                               320,
                               NULL};
static Scene g_asr_scene = {UNI_RASR_LASR_MODE,
                            DEFAULT_RECOGN_SCENE_TIMEOUT,
                            DEFAULT_RECOGN_SCENE_LOW_THRESHOLD,
                            DEFAULT_RECOGN_SCENE_STD_THRESHOLD,
                            320,
                            NULL};

static StudySession *g_study_session = NULL;
static MicroFsmTransition *g_study_session_transition = NULL;

static void _session_scene_init(void) {
  double tmp = 0.0;
  uni_s64 timeout = 0;
  if (0 == ConfigReadItemDouble("lasr.lasr_wkp.std_threshold", &tmp)) {
    g_wakeup_scene.std_threshold = tmp;
  }
  if (0 == ConfigReadItemDouble("lasr.lasr_wkp.lp_threshold", &tmp)) {
    g_wakeup_scene.low_threshold = tmp;
  }
  if (0 == ConfigReadItemDouble("lasr.lasr_asr.std_threshold", &tmp)) {
    g_asr_scene.std_threshold = tmp;
  }
  if (0 == ConfigReadItemDouble("lasr.lasr_asr.lp_threshold", &tmp)) {
    g_asr_scene.low_threshold = tmp;
  }
  if (0 == ConfigReadItemNumber("lasr.lasr_asr.timeout", &timeout)) {
    g_asr_scene.timeout = timeout * 1000;
  }
}

static int _session_transition_init(void) {
  const MicroFsmTransition session_transition[] = {
    {STATE_IDLE, ID(VUI_APP_STUDY_EVENT), _idle__vui_app_study},
    {STATE_IDLE, ID(AUDIO_PLAY_END_EVENT), _idle__audio_play_end},
    {STATE_START, ID(AUDIO_PLAY_END_EVENT), _start__audio_play_end},
    {STATE_CLEAR, ID(AUDIO_PLAY_END_EVENT), _clear__audio_play_end},
    {STATE_STUDYING, ID(VUI_APP_STUDY_RESULT_EVENT),
     _studying__vui_app_study_result},
    {STATE_STUDYING, ID(AUDIO_PLAY_END_EVENT), _studying__audio_play_end},
    {STATE_FINISH, ID(AUDIO_PLAY_END_EVENT), _finish__audio_play_end},
  };
  g_study_session_transition = uni_malloc(sizeof(session_transition));
  if (NULL == g_study_session_transition) {
    LOGE(TAG, "malloc failed !");
    return 0;
  }
  uni_memcpy(g_study_session_transition, session_transition,
             sizeof(session_transition));
  return (sizeof(session_transition) / sizeof(session_transition[0]));
}

static char *_creat_buf_save_str(const char *str) {
  char *buf = NULL;
  int str_len = 0;
  if (NULL == str) {
    return NULL;
  }
  str_len = uni_strlen(str);
  buf = (char *)uni_malloc(str_len + 1);
  if (buf) {
    uni_memset(buf, 0, str_len + 1);
    uni_strncpy(buf, str, str_len);
  }
  return buf;
}

// static void _stop_player(void) {
//  user_event_context_t context = {{0}};
//  MediaPlayerStop(PLAYER_PCM);
//  /* User event send from app layer, because of maybe need some process */
//  if (uni_user_meeting_is_subscribe(USER_AUDIO_PLAY_START)) {
//    context.audio_end.type = AUDIO_PLAY_REPLY;
//    context.audio_end.is_auto = false;
//    uni_user_meeting_send_event(USER_AUDIO_PLAY_END, &context);
//  }
//}

static Result _response_pcm(const char *pcm) {
  user_event_context_t context = {{0}};
  LOGT(TAG, "playing pcm: %s", pcm);
  /* User event send from app layer, because of maybe need some process */
  if (uni_user_meeting_is_subscribe(USER_AUDIO_PLAY_START)) {
    context.audio_play.type = AUDIO_PLAY_REPLY;
    context.audio_play.file_name = _creat_buf_save_str(pcm);
    uni_user_meeting_send_event(USER_AUDIO_PLAY_START, &context);
  }
  RecogStop();
  return MediaPlayerStart(PLAYER_PCM, pcm);
}

static const char *_study_session_state_2_str(uni_s32 state) {
  static const char *state_str[] = {
    [STATE_IDLE] = "STATE_IDLE",         [STATE_START] = "STATE_START",
    [STATE_STUDYING] = "STATE_STUDYING", [STATE_FINISH] = "STATE_FINISH",
    [STATE_CLEAR] = "STATE_CLEAR",
  };
  if (state < STATE_IDLE || STATE_STUDYING < state) {
    return "N/A";
  }
  return state_str[state];
}

static Result _filter_event(Event *event) {
  Result rc = E_OK;
  return rc;
}

static char *_get_cur_grammar() {
  BLOB_TYPE_BOOL has_new_grammar;
  int save_len;
  user_flash_get_env_blob(BLOB_KEY_HAS_NEW_GRAMMAR, &has_new_grammar,
                          sizeof(BLOB_TYPE_BOOL), &save_len);
  if (save_len == sizeof(BLOB_TYPE_BOOL)) {
    if (has_new_grammar) {
      return (char *)STUDY_GRAMMAR_ADDR;
    }
  }

  return (char *)global_kws_lp_grammar;
}

// static uni_u32 _get_cur_grammar_size() {
//  BLOB_TYPE_U32 new_len;
//  int save_len;
//  user_flash_get_env_blob(BLOB_KEY_NEW_GRAMMAR_LEN, &new_len,
//                            sizeof(BLOB_TYPE_U32), &save_len);
//  if (save_len == sizeof(BLOB_TYPE_U32)) {
//    if (new_len > 0) {
//      return new_len;
//    }
//  }
//
//  return global_kws_lp_grammar_size;
//}

extern void DataCacheRangeInvalid(uint32_t start, uint32_t size);
static Result _flash_write_and_check(uni_u32 start_addr, uni_u32 erase_len,
                                     uni_u8 *write_data, uni_u32 write_len) {
  Result rst = E_OK;
  if (uni_hal_flash_erase(SECTOR_ERASE, start_addr, erase_len) < 0) {
    LOGE(TAG, "erase flash failed");
    rst = E_FAILED;
    goto exit_flash_write;
  }

  SPI_FLASH_ERR_CODE err_code;
  err_code =
    uni_hal_flash_write(start_addr, (uint8_t *)write_data, write_len, 1000);
  if (err_code != FLASH_NONE_ERR) {
    LOGE(TAG, "write flash failed, err %d", err_code);
    rst = E_FAILED;
    goto exit_flash_write;
  }

  // invalid cache when direct read on flash
  DataCacheRangeInvalid(start_addr, erase_len);
  int i;
  uni_u8 read_data;
  for (i = 0; i < write_len; ++i) {
    read_data = *(uni_u8 *)(start_addr + i);
    if (read_data != *(write_data + i)) {
      LOGE(TAG, "check flash failed at [%d], read:[0x%02x] exp:[0x%02x]", i,
           read_data, *(write_data + i));
      rst = E_FAILED;
    }
  }

exit_flash_write:
  return rst;
}

static Result _save_study_grammar(char *grammar, int size) {
  if (_flash_write_and_check(STUDY_GRAMMAR_ADDR, 32 * 1024, (uni_u8 *)grammar,
                             size) != E_OK) {
    return E_FAILED;
  }

  BLOB_TYPE_BOOL tmp_bool = true;
  user_flash_set_env_blob(BLOB_KEY_HAS_NEW_GRAMMAR, &tmp_bool,
                          sizeof(BLOB_TYPE_BOOL));
  user_flash_set_env_blob(BLOB_KEY_NEW_GRAMMAR_LEN, &size,
                          sizeof(BLOB_TYPE_U32));
  return E_OK;
}

static Result _update_new_grammar(StudyTmpGrammarNode *node,
                                  uni_bool is_wakeup) {
  Result rc = E_FAILED;
  int i;
  int tmp_base_grammar_size = -1;
  UalAikKwsGetGrammarMergedArgs args;

  uni_hal_watchdog_disable();
  for (i = 0; i < COLLECT_STUDY_RESULT_NUM; ++i) {
    StudyTmpGrammarNode *cur_node = node + i;
    int merged_grammar_size = 0;

    uni_memset(&args, 0, sizeof(UalAikKwsGetGrammarMergedArgs));
    args.merged_grammar_size = (int32_t *)&merged_grammar_size;
    args.tmp_grammar = (int8_t *)(cur_node->tmp_grammar);
    args.is_wakeup = is_wakeup;
    args.merged_grammar = (int8_t *)g_mp3_ais_share_buf;
    args.max_merged_grammar_size = (int32_t)g_mp3_ais_share_buf_size;

    if (tmp_base_grammar_size <= 0) {
      args.base_grammar = (int8_t *)_get_cur_grammar();
    } else {
      args.base_grammar = (int8_t *)STUDY_MERGED_GRAMMAR_ADDR;
    }

    if (E_OK == VuiRecognGet(AIK_ID_KWS_LP_GET_GRAMMAR_MERGE, &args)) {
      LOGT(TAG, "grammar merged success, size %d", *(args.merged_grammar_size));
      if (*(args.merged_grammar_size) > 0) {
        tmp_base_grammar_size = *(args.merged_grammar_size);
        if (E_OK != _flash_write_and_check(STUDY_MERGED_GRAMMAR_ADDR, 32 * 1024,
                                           (uni_u8 *)args.merged_grammar,
                                           tmp_base_grammar_size)) {
          LOGE(TAG, "write merged grammar failed");
          rc = E_FAILED;
          goto exit_update_new_grammar;
        }
      } else {
        LOGE(TAG, "tmp_base_grammar merge error, size %d",
             *args.merged_grammar_size);
        rc = E_FAILED;
        goto exit_update_new_grammar;
      }
    } else {
      LOGE(TAG, "grammar merged failed");
      rc = E_FAILED;
      goto exit_update_new_grammar;
    }
  }
  rc = _save_study_grammar((char *)args.merged_grammar, tmp_base_grammar_size);

exit_update_new_grammar:
  uni_hal_watchdog_enable(WDG_STEP_4S);
  return rc;
}

static Result _save_study_node(StudyTmpGrammarNode *node) {
  BLOB_TYPE_U32 node_num = 0;
  int save_len;
  user_flash_get_env_blob(BLOB_KEY_STUDY_NODE_NUM, &node_num,
                          sizeof(BLOB_TYPE_U32), &save_len);
  if (save_len != sizeof(BLOB_TYPE_U32)) {
    node_num = 0;
  } else {
    if (node_num >= MAX_STUDY_NODES_NUM) {
      LOGE(TAG, "failed, study node over max num %d", MAX_STUDY_NODES_NUM);
      return E_FAILED;
    }
  }

  uni_u32 start_addr =
    STUDY_TMP_GRAMMAR_NODE_ADDR + (node_num * ERASE_SECTOR_SIZEE);
  uni_u32 save_size = sizeof(StudyTmpGrammarNode) * COLLECT_STUDY_RESULT_NUM;
  if (_flash_write_and_check(start_addr, ERASE_SECTOR_SIZEE, (uni_u8 *)node,
                             save_size) != E_OK) {
    return E_FAILED;
  }

  node_num++;
  user_flash_set_env_blob(BLOB_KEY_STUDY_NODE_NUM, &node_num,
                          sizeof(BLOB_TYPE_U32));
  return E_OK;
}

static Result _update_study_nlu_map(StudyTmpGrammarNode *node,
                                    uni_bool need_save) {
  int i;

  if (need_save && _save_study_node(node) != E_OK) {
    LOGE(TAG, "save study nodes failed");
    return E_FAILED;
  }

  for (i = 0; i < COLLECT_STUDY_RESULT_NUM; ++i) {
    StudyNLUContent *nlu_content = NULL;
    StudyTmpGrammarNode *cur_node = node + i;
    nlu_content = uni_malloc(sizeof(StudyNLUContent));
    if (nlu_content != NULL) {
      uni_memset(nlu_content, 0, sizeof(StudyNLUContent));
      nlu_content->nlu_map.key_word_hash_code = cur_node->command_hash;
      nlu_content->nlu_map.nlu_content_str_index = cur_node->nlu_cmd_index;
      uni_list_add_tail(&nlu_content->link, &g_study_session->head);
      LOGT(TAG, "add map hash:%u cmd:%d in list",
           nlu_content->nlu_map.key_word_hash_code,
           nlu_content->nlu_map.nlu_content_str_index);
    } else {
      LOGE(TAG, "malloc nlu_content %d failed", sizeof(StudyNLUContent));
      return E_FAILED;
    }
  }
  return E_OK;
}

static Result _action_finish() {
  if (E_OK != _update_new_grammar(g_study_session->node,
                                  g_study_session->is_study_wakeup)) {
    return E_FAILED;
  }

  // study wakeup grammar also merge into command
  if (g_study_session->is_study_wakeup) {
    if (E_OK != _update_new_grammar(g_study_session->node, 0)) {
      return E_FAILED;
    }
  }

  if (E_OK != _update_study_nlu_map(g_study_session->node, true)) {
    return E_FAILED;
  }
  return E_OK;
}

static void _action_success_get_result() {
  if (g_study_session->succ_index == (COLLECT_STUDY_RESULT_NUM - 1)) {
    if (E_OK == _action_finish()) {
      g_study_session->succ_index++;
      g_study_session->study_index++;
      FsmSetState(g_study_session->fsm, STATE_FINISH);

      user_event_context_t ctx = {{0}};
      ctx.study_end.success = TRUE;
      ctx.study_end.failed_count = g_study_session->failed_count;
      uni_user_meeting_send_event(USER_STUDY_SUCCESS_EVENT, &ctx);

      _response_pcm(
        uni_get_number_pcm(g_study_session->pcm_list, STUDY_PCM_FINISH));
    } else {
      g_study_session->failed_count = STUDY_FAILED_EXIT_COUNT;
      _response_pcm(
        uni_get_number_pcm(g_study_session->pcm_list, STUDY_PCM_FAIL_EXIT));
    }
  } else {
    g_study_session->succ_index++;
    _response_pcm(
      uni_get_number_pcm(g_study_session->pcm_list, STUDY_PCM_SUCCESS));
  }
}

static void _action_failed_get_result() {
  g_study_session->failed_count++;
  if (g_study_session->failed_count >= STUDY_FAILED_EXIT_COUNT) {

    user_event_context_t ctx = {{0}};
    ctx.study_end.success = FALSE;
    ctx.study_end.failed_count = g_study_session->failed_count;
    ctx.study_end.reason = "max_failures";
    uni_user_meeting_send_event(USER_STUDY_FAIL_EVENT, &ctx);

    _response_pcm(
      uni_get_number_pcm(g_study_session->pcm_list, STUDY_PCM_FAIL_EXIT));
  } else {
    _response_pcm(
      uni_get_number_pcm(g_study_session->pcm_list, STUDY_PCM_FAIL_RETRY));
  }
}

static uni_bool _check_has_study_data(uni_bool is_wakeup) {
  if (_get_cur_grammar() != (char *)STUDY_GRAMMAR_ADDR) {
    return false;
  }

  StudyNLUContent *content;

  uni_list_for_each_entry(content, &g_study_session->head, StudyNLUContent,
                          link) {
    if (is_wakeup &&
        content->nlu_map.nlu_content_str_index == eCMD_wakeup_uni) {
      return true;
    } else if (!is_wakeup &&
               content->nlu_map.nlu_content_str_index != eCMD_wakeup_uni) {
      return true;
    }
  }
  return false;
}

static uni_bool _check_tmp_grammar_valid(uni_u8 node_index) {
  if (node_index == 0) return true;
  UalAikKwsGetGrammarSimilarityArgs args;
  int tmp_score;
  args.score = (int32_t *)&tmp_score;
  args.tmp_grammar1 =
    (int8_t *)(g_study_session->node[node_index - 1].tmp_grammar);
  args.tmp_grammar2 = (int8_t *)(g_study_session->node[node_index].tmp_grammar);
  if (E_OK == VuiRecognGet(AIK_ID_KWS_LP_GET_GRAMMAR_COMPARE, &args)) {
    LOGT(TAG, "grammar compare result [%d]-[%d] score %d", node_index - 1,
         node_index, *(args.score));
    return *(args.score) >= STUDY_GRAMMAR_SIMILARITY_THRESHOLD;
  }
  return false;
}

static Result _exit_studying(void) {
   g_study_start_time = 0;
  g_study_session->enter_studying = false;
  FsmSetState(g_study_session->fsm, STATE_IDLE);
  if (VuiRecognUpdateGrammar(_get_cur_grammar()) != E_OK) {
    LOGE(TAG, "engine update grammar failed");
    return E_FAILED;
  }
  RecogLaunch(&g_wakeup_scene);
  return E_OK;
}

static Result _start__audio_play_end(void *event) {
  LOGT(TAG, "action called");
  if (VuiRecognUpdateGrammar((char *)global_kws_lp_study_grammar) != E_OK) {
    LOGE(TAG, "engine update loop grammar failed");
    return E_FAILED;
  }
  FsmSetState(g_study_session->fsm, STATE_STUDYING);
  g_study_start_time = uni_get_clock_time_ms();   // 记录开始时间
  RecogLaunch(&g_study_scene);
  return E_HOLD;
}

static Result _clear__audio_play_end(void *event) {
  LOGT(TAG, "action called");

  user_event_context_t ctx = {{0}};
  ctx.study_reset.is_wakeup = g_clear_is_wakeup;
  uni_user_meeting_send_event(USER_STUDY_RESET_EVENT, &ctx);

  FsmSetState(g_study_session->fsm, STATE_IDLE);
  RecogLaunch(&g_asr_scene);
  return E_OK;
}

static Result _idle__audio_play_end(void *event) {
  RecogLaunch(&g_asr_scene);
  return E_OK;
}

static Result _finish__audio_play_end(void *event) {
  Result rc;
  LOGT(TAG, "action called");
  if (g_study_session->study_index == MAX_STUDY_CMD) {
    rc = _exit_studying();
  } else {
    if (g_study_session->failed_count >= STUDY_FAILED_EXIT_COUNT) {
      rc = _exit_studying();
    } else {
      if (g_study_session->is_study_wakeup) {
        rc = _exit_studying();
      } else {
        FsmSetState(g_study_session->fsm, STATE_STUDYING);
        g_study_session->succ_index = 0;
        g_study_session->failed_count = 0;
        uni_memset(g_study_session->node, 0,
                   sizeof(StudyTmpGrammarNode) * COLLECT_STUDY_RESULT_NUM);
        _response_pcm(g_study_cmd_reply_pcm_list[g_study_session->study_index]);
        rc = E_HOLD;
      }
    }
  }
  return rc;
}

static Result _studying__audio_play_end(void *event) {
  Result rc;
  LOGT(TAG, "action called");
  if (g_study_session->succ_index == COLLECT_STUDY_RESULT_NUM) {
    rc = _exit_studying();
  } else {
    if (g_study_session->failed_count >= STUDY_FAILED_EXIT_COUNT) {
      rc = _exit_studying();
    } else {
      g_study_start_time = uni_get_clock_time_ms();   // 记录开始时间
      RecogLaunch(&g_study_scene);
      rc = E_HOLD;
    }
  }
  return rc;
}

static Result _idle__vui_app_study(void *event_info) {
  LOGT(TAG, "action called");
  uni_lasr_result_t *content = NULL;
  content = (uni_lasr_result_t *)(((Event *)event_info)->content.info);

  if (uni_strcmp(content->cmd, "startStudy") == 0 ||
      uni_strcmp(content->cmd, "startStuCmd") == 0) {
      return E_FAILED;  // 直接返回，不做任何处理
    if (_check_has_study_data(false)) {
      _response_pcm(
        uni_get_number_pcm(g_study_session->pcm_list, STUDY_PCM_ADVICE_RESET));
      return E_HOLD;
    }
    g_study_session->succ_index = 0;
    g_study_session->failed_count = 0;
    g_study_session->study_index = 0;
    g_study_session->is_study_wakeup = false;
    uni_memset(g_study_session->node, 0,
               sizeof(StudyTmpGrammarNode) * COLLECT_STUDY_RESULT_NUM);
    g_study_session->enter_studying = true;
    FsmSetState(g_study_session->fsm, STATE_START);
    if (E_OK == _response_pcm(
                  g_study_cmd_reply_pcm_list[g_study_session->study_index])) {
      return E_HOLD;
    }
  } else if (uni_strcmp(content->cmd, "clearStudy") == 0) {
    
    g_clear_pending = TRUE;
    g_clear_is_wakeup = FALSE;   // 清除所有

    StudySessionClearGrammar();

    FsmSetState(g_study_session->fsm, STATE_CLEAR);
    if (E_OK == _response_pcm(uni_get_random_pcm(content->pcm))) {
      return E_HOLD;
    }
  } else if (uni_strcmp(content->cmd, "startStuWakeup") == 0) {
    if (_check_has_study_data(true)) {
      _response_pcm(
        uni_get_number_pcm(g_study_session->pcm_list, STUDY_PCM_ADVICE_RESET));
      return E_HOLD;
    }
    g_study_session->succ_index = 0;
    g_study_session->failed_count = 0;
    g_study_session->study_index = 0;
    g_study_session->is_study_wakeup = true;
    uni_memset(g_study_session->node, 0,
               sizeof(StudyTmpGrammarNode) * COLLECT_STUDY_RESULT_NUM);
    g_study_session->enter_studying = true;

    user_event_context_t ctx = {{0}};
    ctx.study_start.is_wakeup_study = TRUE;
    ctx.study_start.cmd = "wakeup";
    uni_user_meeting_send_event(USER_STUDY_START_EVENT, &ctx);

    FsmSetState(g_study_session->fsm, STATE_START);
    if (E_OK == _response_pcm(uni_get_number_pcm(g_study_session->pcm_list,
                                                 STUDY_PCM_START_WAKEUP))) {
      return E_HOLD;
    }
  } else if (uni_strcmp(content->cmd, "clearStuWakeup") == 0) {
    if (_check_has_study_data(true)) {

      g_clear_pending = TRUE;
      g_clear_is_wakeup = TRUE;   // 清除唤醒词

      StudySessionRemoveGrammar(eCMD_wakeup_uni, false);
    }
    FsmSetState(g_study_session->fsm, STATE_CLEAR);
    if (E_OK == _response_pcm(uni_get_random_pcm(content->pcm))) {
      return E_HOLD;
    }
  } else if (uni_strcmp(content->cmd, "clearStuCmd") == 0) {
    return E_FAILED;  // 直接返回，不做任何处理
    if (_check_has_study_data(false)) {

      g_clear_pending = TRUE; 
      g_clear_is_wakeup = FALSE;

      StudySessionRemoveGrammar(eCMD_wakeup_uni, true);
    }
    FsmSetState(g_study_session->fsm, STATE_CLEAR);
    if (E_OK == _response_pcm(uni_get_random_pcm(content->pcm))) {
      return E_HOLD;
    }
  }

  return E_FAILED;
}

static Result _studying__vui_app_study_result(void *event_info) {
   g_study_start_time = 0;
  LOGT(TAG, "action called");
  Event *event = (Event *)event_info;
  UalAikKwsGetTmpGrammarArgs args;
  uni_u8 index = g_study_session->succ_index;

  g_study_session->node[index].command_hash = *(uni_u32 *)event->content.info;
  if (g_study_session->is_study_wakeup) {
    g_study_session->node[index].nlu_cmd_index = eCMD_wakeup_uni;
  } else {
    g_study_session->node[index].nlu_cmd_index =
      g_study_cmd_list[g_study_session->study_index];
  }
  args.tmp_grammar_size =
    (int32_t *)&g_study_session->node[index].tmp_grammar_size;
  args.max_grammar_size = (int32_t)MAX_TMP_GRAMMAR_SIZE;
  args.tmp_grammar = (int8_t *)g_study_session->node[index].tmp_grammar;
  RecogStop();
  if (E_OK == VuiRecognGet(AIK_ID_KWS_LP_GET_STUDY_GRAMMAR, &args)) {
    LOGT(TAG, "get tmp grammar size %d", *(args.tmp_grammar_size));
    if (*(args.tmp_grammar_size) < 0 || !_check_tmp_grammar_valid(index)) {
      LOGT(TAG, "check tmp grammar valid failed");
      _action_failed_get_result();
      return E_HOLD;
    } else {
      _action_success_get_result();
    }
  } else {
    _action_failed_get_result();
  }
  return E_HOLD;
}

static Result _query_handler(Event *event) {
    if (g_study_session && g_study_session->enter_studying && g_study_start_time != 0) {
        uni_s64 now = uni_get_clock_time_ms();
        if ((now - g_study_start_time) > STUDY_TIMEOUT_MS) {
              LOGW(TAG, "Study timeout, treat as failure");
            // 增加失败计数，播放重试提示音（必要时退出）
            _action_failed_get_result();       
            // 重置计时，避免在音频播放期间再次触发超时
            g_study_start_time = 0;
            return E_HOLD;   // 表示该事件已被处理
            
        }
    }

  Result ret = IsValidEventId(g_study_session->fsm, event->type);
  if (E_OK == ret) {
    ret = _filter_event(event);
    LOGT(TAG, "query handler ok, event=%d ret=%d", event->type, ret);
  }
  return ret;
}

static Result _start_handler(Event *event) {
  Result ret;
  if (NULL == event) {
    ret = ProcessNextEvent(g_study_session->fsm, NULL, ID(COMMON_RESUME_EVENT));
    LOGT(TAG, "start handler called, event=NULL ret=%d", ret);
  } else {
    ret = ProcessNextEvent(g_study_session->fsm, (void *)event, event->type);
    LOGT(TAG, "start handler called, event=%d ret=%d", event->type, ret);
  }
  return ret;
}

static Result _stop_handler(void) {
  Result ret =
    ProcessNextEvent(g_study_session->fsm, NULL, ID(COMMON_STOP_EVENT));
  LOGT(TAG, "stop handler called, ret=%d", ret);
  return ret;
}

static inline void _init_study_nlu_mapping_list(void) {
  BLOB_TYPE_U32 node_num = 0;
  int save_len;
  int i;

  uni_list_init(&g_study_session->head);
  user_flash_get_env_blob(BLOB_KEY_STUDY_NODE_NUM, &node_num,
                          sizeof(BLOB_TYPE_U32), &save_len);
  if (save_len != sizeof(BLOB_TYPE_U32)) {
    node_num = 0;
  }

  for (i = 0; i < node_num; ++i) {
    StudyTmpGrammarNode *node =
      (StudyTmpGrammarNode *)(STUDY_TMP_GRAMMAR_NODE_ADDR +
                              (i * ERASE_SECTOR_SIZEE));
    if (E_OK != _update_study_nlu_map(node, false)) {
      LOGE(TAG, "update study nlu map failed");
    }
  }
}

Result StudySessionInit(void) {
  uni_s32 session_transition_cnt = 0;

  g_study_session = (StudySession *)uni_malloc(sizeof(StudySession));
  if (NULL == g_study_session) {
    LOGE(TAG, "memory alloc failed");
    return E_FAILED;
  }
  uni_memset(g_study_session, 0, sizeof(StudySession));
  _session_scene_init();
  g_study_session->session = SessionCreate(
    SESSION_PRIORITY_STUDY, 2, _query_handler, _start_handler, _stop_handler);
  if (NULL == g_study_session->session) {
    LOGE(TAG, "create session failed");
    goto L_ERROR0;
  }
  g_study_session->fsm = (MicroFsmStruct *)uni_malloc(sizeof(MicroFsmStruct));
  if (NULL == g_study_session->fsm) {
    LOGE(TAG, "memory alloc failed");
    goto L_ERROR1;
  }

  cJSON *jstudy_content = NULL;
  jstudy_content = cJSON_Parse(g_nlu_content_str[eCMD_startStudy]);
  JsonReadItemString(jstudy_content, "pcm", &(g_study_session->pcm_list));
  cJSON_Delete(jstudy_content);

  g_study_session->succ_index = 0;
  g_study_session->failed_count = 0;
  g_study_session->study_index = 0;
  g_study_session->enter_studying = false;
  g_study_session->is_study_wakeup = false;
  _init_study_nlu_mapping_list();
  session_transition_cnt = _session_transition_init();
  FsmInit(g_study_session->fsm, "study", g_study_session_transition,
          session_transition_cnt, _study_session_state_2_str);
  FsmSetState(g_study_session->fsm, STATE_IDLE);
  SessionManageRegister(g_study_session->session);
  RecogStop();
  if (VuiRecognUpdateGrammar(_get_cur_grammar()) != E_OK) {
    LOGE(TAG, "engine update grammar failed");
    return E_FAILED;
  }
  RecogLaunch(&g_wakeup_scene);
  return E_OK;
L_ERROR1:
  SessionDestroy(g_study_session->session);
L_ERROR0:
  uni_free(g_study_session);
  g_study_session = NULL;
  return E_FAILED;
}

Result StudySessionFinal(void) {
  if (NULL != g_study_session) {
    SessionManageUnregister(g_study_session->session);
    SessionDestroy(g_study_session->session);
    if (NULL != g_study_session->fsm) {
      uni_free(g_study_session->fsm);
      g_study_session->fsm = NULL;
    }
    if (NULL != g_study_session->pcm_list) {
      uni_free(g_study_session->pcm_list);
      g_study_session->pcm_list = NULL;
    }
    uni_free(g_study_session);
    g_study_session = NULL;
  }
  if (NULL != g_study_session_transition) {
    uni_free(g_study_session_transition);
    g_study_session_transition = NULL;
  }
  StudyNLUContent *item, *tmp;
  uni_list_for_each_entry_safe(item, tmp, &g_study_session->head,
                               StudyNLUContent, link) {
    uni_list_del(&item->link);
    uni_free(item);
  }
  return E_OK;
}

uni_bool StudySessionGetNluIndex(uni_u32 hashcode, uni_u8 *nlu_index) {
  StudyNLUContent *content;

  uni_list_for_each_entry(content, &g_study_session->head, StudyNLUContent,
                          link) {
    if (content->nlu_map.key_word_hash_code == hashcode) {
      *nlu_index = content->nlu_map.nlu_content_str_index;
      return true;
    }
  }
  return false;
}

void StudySessionClearGrammar() {
  BLOB_TYPE_BOOL tmp_bool = false;
  BLOB_TYPE_U32 tmp_int = 0;

  user_flash_set_env_blob(BLOB_KEY_HAS_NEW_GRAMMAR, &tmp_bool,
                          sizeof(BLOB_TYPE_BOOL));
  user_flash_set_env_blob(BLOB_KEY_NEW_GRAMMAR_LEN, &tmp_int,
                          sizeof(BLOB_TYPE_U32));
  user_flash_set_env_blob(BLOB_KEY_STUDY_NODE_NUM, &tmp_int,
                          sizeof(BLOB_TYPE_U32));
  g_study_session->succ_index = 0;
  g_study_session->failed_count = 0;
  g_study_session->study_index = 0;
  uni_memset(g_study_session->node, 0,
             sizeof(StudyTmpGrammarNode) * COLLECT_STUDY_RESULT_NUM);

  StudyNLUContent *item, *tmp;
  uni_list_for_each_entry_safe(item, tmp, &g_study_session->head,
                               StudyNLUContent, link) {
    uni_list_del(&item->link);
    uni_free(item);
  }

  RecogStop();
  if (VuiRecognUpdateGrammar(_get_cur_grammar()) != E_OK) {
    LOGE(TAG, "engine update loop grammar failed");
  }
  RecogLaunch(&g_wakeup_scene);
}

void StudySessionRemoveGrammar(uni_u8 rm_nlu_index, uni_bool remove_else) {
  BLOB_TYPE_U32 node_num = 0;
  int save_len;
  int i, j, rm_index;
  uni_u8 *tmp_buff = NULL;

  tmp_buff = (uni_u8 *)uni_malloc(ERASE_SECTOR_SIZEE);
  if (tmp_buff == NULL) {
    LOGE(TAG, "malloc size %d failed", (ERASE_SECTOR_SIZEE));
    return;
  }

  user_flash_get_env_blob(BLOB_KEY_STUDY_NODE_NUM, &node_num,
                          sizeof(BLOB_TYPE_U32), &save_len);
  if (save_len != sizeof(BLOB_TYPE_U32)) {
    node_num = 0;
  }

  uni_hal_watchdog_disable();
  do {
    rm_index = -1;
    for (i = 0; i < node_num; ++i) {
      StudyTmpGrammarNode *node =
        (StudyTmpGrammarNode *)(STUDY_TMP_GRAMMAR_NODE_ADDR +
                                (i * ERASE_SECTOR_SIZEE));
      if (remove_else && node->nlu_cmd_index != rm_nlu_index) {
        rm_index = i;
        break;
      } else if (!remove_else && node->nlu_cmd_index == rm_nlu_index) {
        rm_index = i;
        break;
      }
    }

    if (rm_index != -1) {
      for (j = rm_index; (j + 1) < node_num; ++j) {
        uni_u32 start_addr =
          (STUDY_TMP_GRAMMAR_NODE_ADDR + (j * ERASE_SECTOR_SIZEE));
        uni_u8 *write_data = (uni_u8 *)(STUDY_TMP_GRAMMAR_NODE_ADDR +
                                        ((j + 1) * ERASE_SECTOR_SIZEE));
        uni_hal_flash_read((uint32_t)write_data, tmp_buff, ERASE_SECTOR_SIZEE,
                           1000);
        _flash_write_and_check(start_addr, ERASE_SECTOR_SIZEE, tmp_buff,
                               ERASE_SECTOR_SIZEE);
      }
      node_num--;
    }
  } while (rm_index != -1);
  uni_hal_watchdog_enable(WDG_STEP_4S);

  // refresh study nlu map and grammar
  BLOB_TYPE_BOOL tmp_bool = false;
  user_flash_set_env_blob(BLOB_KEY_HAS_NEW_GRAMMAR, &tmp_bool,
                          sizeof(BLOB_TYPE_BOOL));
  user_flash_set_env_blob(BLOB_KEY_STUDY_NODE_NUM, &node_num,
                          sizeof(BLOB_TYPE_U32));
  StudyNLUContent *item, *tmp;
  uni_list_for_each_entry_safe(item, tmp, &g_study_session->head,
                               StudyNLUContent, link) {
    uni_list_del(&item->link);
    uni_free(item);
  }

  RecogStop();
  if (node_num > 0) {
    for (i = 0; i < node_num; ++i) {
      StudyTmpGrammarNode *node =
        (StudyTmpGrammarNode *)(STUDY_TMP_GRAMMAR_NODE_ADDR +
                                (i * ERASE_SECTOR_SIZEE));
      _update_new_grammar(node, (node->nlu_cmd_index == eCMD_wakeup_uni));
      if (node->nlu_cmd_index == eCMD_wakeup_uni) {
        _update_new_grammar(node, 0);
      }
      _update_study_nlu_map(node, false);
    }
  }
  if (VuiRecognUpdateGrammar(_get_cur_grammar()) != E_OK) {
    LOGE(TAG, "engine update loop grammar failed");
  }
  RecogLaunch(&g_wakeup_scene);
  uni_free(tmp_buff);
}
