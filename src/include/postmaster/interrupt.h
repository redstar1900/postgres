/*-------------------------------------------------------------------------
 *
 * interrupt.h
 *    中断处理相关函数声明。
 *
 * 对中断的响应方式多种多样，许多类型的后端进程有自己的实现，
 * 但这里我们提供了一些通用的内容以便代码复用。
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/include/postmaster/interrupt.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef INTERRUPT_H
#define INTERRUPT_H

#include <signal.h>

// 标记是否有配置重载请求待处理
extern PGDLLIMPORT volatile sig_atomic_t ConfigReloadPending;
// 标记是否有关闭请求待处理
extern PGDLLIMPORT volatile sig_atomic_t ShutdownRequestPending;

// 处理主循环中的中断
extern void HandleMainLoopInterrupts(void);
// 配置重载信号处理函数
extern void SignalHandlerForConfigReload(SIGNAL_ARGS);
// 崩溃退出信号处理函数
extern void SignalHandlerForCrashExit(SIGNAL_ARGS);
// 关闭请求信号处理函数
extern void SignalHandlerForShutdownRequest(SIGNAL_ARGS);

#endif
