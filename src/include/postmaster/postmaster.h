/*-------------------------------------------------------------------------
 *
 * postmaster.h
 *    postmaster/postmaster.c 导出的接口声明。
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/postmaster.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _POSTMASTER_H
#define _POSTMASTER_H

#include "miscadmin.h"

/* GUC 配置选项 */
// 是否启用 SSL
extern PGDLLIMPORT bool EnableSSL;
// 超级用户保留连接数
extern PGDLLIMPORT int SuperuserReservedConnections;
// 保留连接数
extern PGDLLIMPORT int ReservedConnections;
// 监听端口号
extern PGDLLIMPORT int PostPortNumber;
// Unix socket 权限
extern PGDLLIMPORT int Unix_socket_permissions;
// Unix socket 所属用户组
extern PGDLLIMPORT char *Unix_socket_group;
// Unix socket 目录
extern PGDLLIMPORT char *Unix_socket_directories;
// 监听地址
extern PGDLLIMPORT char *ListenAddresses;
// 是否正在进行客户端认证
extern PGDLLIMPORT bool ClientAuthInProgress;
// 认证前延迟
extern PGDLLIMPORT int PreAuthDelay;
// 认证超时时间
extern PGDLLIMPORT int AuthenticationTimeout;
// 是否记录连接日志
extern PGDLLIMPORT bool Log_connections;
// 是否记录主机名
extern PGDLLIMPORT bool log_hostname;
// 是否启用 Bonjour 服务
extern PGDLLIMPORT bool enable_bonjour;
// Bonjour 服务名称
extern PGDLLIMPORT char *bonjour_name;
// 崩溃后是否重启
extern PGDLLIMPORT bool restart_after_crash;
// 崩溃后是否移除临时文件
extern PGDLLIMPORT bool remove_temp_files_after_crash;
// 崩溃时是否发送中止信号
extern PGDLLIMPORT bool send_abort_for_crash;
// 被 kill 时是否发送中止信号
extern PGDLLIMPORT bool send_abort_for_kill;

#ifdef WIN32
// Windows 下 postmaster 句柄
extern PGDLLIMPORT HANDLE PostmasterHandle;
#else
// 非 Windows 下用于检测 postmaster 存活的文件描述符
extern PGDLLIMPORT int postmaster_alive_fds[2];

/*
 * 常量：postmaster_alive_fds 的索引
 * POSTMASTER_FD_WATCH：子进程用于检测 postmaster 是否存活
 * POSTMASTER_FD_OWN：仅由 postmaster 保持打开
 */
#define POSTMASTER_FD_WATCH		0	/* 子进程检测 postmaster 死亡 */
#define POSTMASTER_FD_OWN		1	/* 仅 postmaster 保持打开 */
#endif

// 程序名
extern PGDLLIMPORT const char *progname;

// 是否已加载 SSL
extern PGDLLIMPORT bool LoadedSSL;

// postmaster 主函数（不会返回）
extern void PostmasterMain(int argc, char *argv[]) pg_attribute_noreturn();
// 关闭 postmaster 监听端口
extern void ClosePostmasterPorts(bool am_syslogger);
// 初始化全局进程变量
extern void InitProcessGlobals(void);

// 获取最大允许的 postmaster 子进程数
extern int	MaxLivePostmasterChildren(void);

// 标记某个 PID 以便后台工作进程通知
extern bool PostmasterMarkPIDForWorkerNotify(int);

// 处理取消请求
extern void processCancelRequest(int backendPID, int32 cancelAuthCode);

#ifdef EXEC_BACKEND
// 共享内存后端数组大小
extern Size ShmemBackendArraySize(void);
// 分配共享内存后端数组
extern void ShmemBackendArrayAllocation(void);

#ifdef WIN32
// Windows 下注册子进程退出回调
extern void pgwin32_register_deadchild_callback(HANDLE procHandle, DWORD procId);
#endif
#endif

/* 在 globals.c 中定义 */
// 当前客户端 socket
extern PGDLLIMPORT struct ClientSocket *MyClientSocket;

/* launch_backend.c 中的函数声明 */
// 启动 postmaster 子进程
extern pid_t postmaster_child_launch(BackendType child_type,
                                     char *startup_data,
                                     size_t startup_data_len,
                                     struct ClientSocket *client_sock);
// 获取子进程类型名称
const char *PostmasterChildName(BackendType child_type);
#ifdef EXEC_BACKEND
// 子 postmaster 主函数（不会返回）
extern void SubPostmasterMain(int argc, char *argv[]) pg_attribute_noreturn();
#endif

/*
 * 注意：MAX_BACKENDS 最大为 2^18-1，因为 buf_internals.h 中为缓冲区引用保留了这么多位。
 * 这个限制可以通过使用 64 位状态来解除，但目前意义不大，因为 2^18-1 已远超实际需求。
 * 即使解除该限制，仍然不能超过 2^23-1，因为 inval.c 中 ProcNumber 只用 3 字节有符号整数存储；
 * 也不能超过 INT_MAX/4，因为有些地方会计算 4*MaxBackends 而不做溢出检查。
 * 这些限制会在相关的 GUC 检查钩子和 RegisterBackgroundWorker() 中重新检查。
 */
#define MAX_BACKENDS	0x3FFFF

#endif							/* _POSTMASTER_H */
