#pragma once

/* Usage and banner */
#define IDS_USAGE               300 /* 300..309: consecutive parts */
#define IDS_USAGE1              301
#define IDS_USAGE2              302
#define IDS_USAGE3              303
#define IDS_USAGE4              304
#define IDS_SIMPLE_USAGE        101
#define IDS_BANNER              102

/* Job header */
#define IDS_HDR_STARTED         110
#define IDS_HDR_SOURCE          111
#define IDS_HDR_DEST            112
#define IDS_HDR_FILES           113
#define IDS_HDR_EXC_FILES       114
#define IDS_HDR_EXC_DIRS        115
#define IDS_HDR_OPTIONS         116
#define IDS_HDR_SOURCE_ERR      117
#define IDS_HDR_DEST_ERR        118

/* Job summary */
#define IDS_SUM_COLUMNS         130
#define IDS_SUM_DIRS            131
#define IDS_SUM_FILES           132
#define IDS_SUM_BYTES           133
#define IDS_SUM_TIMES           134
#define IDS_SUM_SPEED_BYTES     135
#define IDS_SUM_SPEED_MB        136
#define IDS_SUM_ENDED           137

/* File classes (padded, shown in file lines) */
#define IDS_CLS_NEWFILE         150
#define IDS_CLS_NEWER           151
#define IDS_CLS_OLDER           152
#define IDS_CLS_CHANGED         153
#define IDS_CLS_SAME            154
#define IDS_CLS_TWEAKED         155
#define IDS_CLS_EXTRAFILE       156
#define IDS_CLS_MISMATCH        157
#define IDS_CLS_V_SAME          158
#define IDS_CLS_V_NEWER         159
#define IDS_CLS_V_OLDER         160
#define IDS_CLS_V_CHANGED       161
#define IDS_CLS_V_TWEAKED       162
#define IDS_CLS_V_NAMED         163
#define IDS_CLS_V_ATTRIB        164
#define IDS_CLS_V_LARGE         165
#define IDS_CLS_V_SMALL         166
#define IDS_CLS_V_TOOOLD        167
#define IDS_CLS_V_TOONEW        168
#define IDS_CLS_V_LONELY        169
#define IDS_CLS_V_JUNCTION      170

/* Directory classes (padded, shown in directory lines) */
#define IDS_DCLS_NEWDIR         180
#define IDS_DCLS_EXTRADIR       181
#define IDS_DCLS_MISMATCH       182
#define IDS_DCLS_EXCLUDED       183

/* Errors */
#define IDS_ERR_INVALID_PARAM   200
#define IDS_ERR_NO_SOURCE       201
#define IDS_ERR_NO_DEST         202
#define IDS_ERR_FORMAT          203
#define IDS_ERR_RETRY_WAIT      204
#define IDS_ERR_RETRYING        205
#define IDS_ERR_RETRY_LIMIT     206
#define IDS_ERR_JOB_FILE        207
#define IDS_ERR_LOG_FILE        208
#define IDS_ERR_OUTOFMEMORY     209

/* Error contexts ("ERROR n (0x...) <context> <path>") */
#define IDS_CTX_ACCESS_SRC      220
#define IDS_CTX_ACCESS_DST      221
#define IDS_CTX_CREATE_DST      222
#define IDS_CTX_COPY_FILE       223
#define IDS_CTX_DELETE_EXTRA    224
#define IDS_CTX_DELETE_EXTRADIR 225
#define IDS_CTX_DELETE_SRC      226
#define IDS_CTX_SCAN_SRC        227
#define IDS_CTX_SCAN_DST        228
#define IDS_CTX_SET_ATTR        229
#define IDS_CTX_SET_TIME        230
#define IDS_CTX_REMOVE_SRCDIR   231
#define IDS_CTX_CREATE_DIR      232

#define IDS_SAVED_JOB           250
