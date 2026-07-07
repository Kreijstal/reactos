/*
 * PROJECT:     ReactOS Wireless Network Selector
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Resource identifiers for the wlanui GUI front-end.
 */

#ifndef _WLANUI_RESOURCE_H_
#define _WLANUI_RESOURCE_H_

#define IDI_WLANUI              100

/* Main dialog */
#define IDD_MAIN                101
#define IDC_NETLIST             1001
#define IDC_REFRESH             1002
#define IDC_CONNECT             1003
#define IDC_DISCONNECT          1004
#define IDC_STATUS              1005
#define IDC_IFACE               1006

/* Security-key prompt dialog */
#define IDD_KEY                 102
#define IDC_KEYPROMPT           1101
#define IDC_KEY                 1102
#define IDC_SHOWKEY             1103

/* Strings */
#define IDS_APPTITLE            200
#define IDS_COL_SSID            201
#define IDS_COL_SIGNAL          202
#define IDS_COL_SECURITY        203
#define IDS_COL_AUTH            204
#define IDS_COL_STATUS          205
#define IDS_NOSERVICE           206
#define IDS_NOIFACE             207
#define IDS_SCANNING            208
#define IDS_CONNECTING          209
#define IDS_CONNECTED           210
#define IDS_DISCONNECTED        211
#define IDS_CONNECTFAIL         212
#define IDS_SECURED             213
#define IDS_OPEN                214
#define IDS_KEYPROMPTFMT        215
#define IDS_SELECTNET           216

#endif /* _WLANUI_RESOURCE_H_ */
