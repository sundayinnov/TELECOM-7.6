#ifndef INC_UNI_CMD_CODE_H_
#define INC_UNI_CMD_CODE_H_

typedef struct {
  uni_u8      cmd_code; /* cmd code fro send base on SUCP */
  const char  *cmd_str; /* action string on UDP */;
} cmd_code_map_t;

const cmd_code_map_t g_cmd_code_arry[] = {
  {0x0, "wakeup_uni"},
  {0x1, "exitUni"},
  {0x2, "startStudy"},
  {0x3, "clearStudy"},
  {0x4, "startStuCmd"},
  {0x5, "startStuWakeup"},
  {0x6, "clearStuCmd"},
  {0x7, "clearStuWakeup"},
  {0x8, "come"},
  {0x9, "away"},
  {0xa, "gohome"},
  {0xb, "opencover"},
  {0xc, "closecover"},
  {0xd, "stop"},
  {0xe, "volumeUpUni"},
  {0xf, "volumeDownUni"},
  {0x10, "fowrad"},
  {0x11, "backward"},
  {0x12, "left"},
  {0x13, "right"},
  {0x14, "back_up"},
};

#endif
