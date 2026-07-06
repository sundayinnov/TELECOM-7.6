/**************************************************************************
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
 **************************************************************************
 *
 * Description : uni_setting_session.h
 * Author      : shangjinlong@unisound.com
 * Date        : 2018.07.06
 *
 **************************************************************************/

#ifndef APP_INC_SESSIONS_UNI_STUDY_SESSION_H_
#define APP_INC_SESSIONS_UNI_STUDY_SESSION_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "uni_iot.h"
#include "uni_nlu_content_type.h"
#include "list_head.h"

#define COLLECT_STUDY_RESULT_NUM           3   ///<每个词重复学习的次数,不得超过10
#define STUDY_FAILED_EXIT_COUNT            3   ///<学习失败X次就退出学习模式
#define STUDY_GRAMMAR_SIMILARITY_THRESHOLD 30  ///<学习相似度阈值，值越大相似度越高，推荐30

typedef struct {
  uni_list_head link;
  uni_nlu_content_mapping_t nlu_map;
} StudyNLUContent;

/**
 * Usage:   Initialize study session and register it to session manager
 * Params:
 * Return:  Result of initialization
 */
Result StudySessionInit(void);

/**
 * Usage:   Finalize study session and unregister it from session manager
 * Params:
 * Return:  Result of finalization
 */
Result StudySessionFinal(void);

/**
 * Usage:   Get study cmd index by hashcode map
 * Params:
 * Return:  Result of get
 */
uni_bool StudySessionGetNluIndex(uni_u32 hashcode, uni_u8 *nlu_index);

/**
 * Usage:   Clear study grammars and data
 * Params:
 * Return:
 */
void StudySessionClearGrammar();

/**
 * Usage:   Remove specific study grammar by cmd index
 * Params:
 * Return:
 */
void StudySessionRemoveGrammar(uni_u8 rm_nlu_index, uni_bool remove_else);
#ifdef __cplusplus
}
#endif
#endif  //  APP_INC_SESSIONS_UNI_STUDY_SESSION_H_