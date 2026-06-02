/*
 * smb2d - shared definitions between service.c and daemon.c.
 */
#ifndef _SMB2D_H_
#define _SMB2D_H_

#define SMB2D_SERVICE_NAME_W  L"smb2d"
#define SMB2D_SERVICE_NAME_A   "smb2d"
#define SMB2D_LOG_PATH_A      "\\SystemRoot\\Temp\\smb2d.log"

DWORD Smb2dDaemonLoop(HANDLE hStopEvent);

#endif /* _SMB2D_H_ */
