/*-------------------------------------------------------------------------
 *
 * fd.h
 *	  Virtual file descriptor definitions.
 *    虚拟文件描述符定义。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/fd.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * calls:
 *
 *	File {Close, Read, Write, Size, Sync}
 *	{Path Name Open, Allocate, Free} File
 *
 * These are NOT JUST RENAMINGS OF THE UNIX ROUTINES.
 * Use them for all file activity...
 *
 *	File fd;
 *	fd = PathNameOpenFile("foo", O_RDONLY);
 *
 *	AllocateFile();
 *	FreeFile();
 *
 * Use AllocateFile, not fopen, if you need a stdio file (FILE*); then
 * use FreeFile, not fclose, to close it.  AVOID using stdio for files
 * that you intend to hold open for any length of time, since there is
 * no way for them to share kernel file descriptors with other files.
 *
 * Likewise, use AllocateDir/FreeDir, not opendir/closedir, to allocate
 * open directories (DIR*), and OpenTransientFile/CloseTransientFile for an
 * unbuffered file descriptor.
 *
 * If you really can't use any of the above, at least call AcquireExternalFD
 * or ReserveExternalFD to report any file descriptors that are held for any
 * length of time.  Failure to do so risks unnecessary EMFILE errors.
 */
/*
 *调用：文件 {关闭， 读取， 写， 大小， 同步}
 *{路径名 Open， Allocate， Free} 文件
 *这些不仅仅是 UNIX 例程的重命名。
 *所有文件活动都用它们......
 *文件fd;
 *fd = PathNameOpenFile（“foo”，O_RDONLY）;
 *AllocateFile（）;
 *FreeFile（）;
 *如果你需要 stdio 文件（FILE），请使用 AllocateFile，而非 fopen;
 *然后用FreeFile（非Fclose）关闭它。避免用 stdio 来记录你打算长时间开启的文件，因为它们无法与其他文件共享内核文件描述符。
 *
 *同样，使用AllocateDirFreeDir（而非opendirclosedir）来分配开放目录（DIR），而OpenTransientFileCloseTransientFile用于无缓冲的文件描述符。
 *
 *如果你真的无法使用上述任何一项，至少调用AcquireExternalFD或ReserveExternalFD，报告任何被保留了一段时间的文件描述符。若未如此，可能导致不必要的EMFILE错误。
 */
#ifndef FD_H
#define FD_H

#include <dirent.h>
#include <fcntl.h>

typedef enum RecoveryInitSyncMethod
{
	RECOVERY_INIT_SYNC_METHOD_FSYNC,
	RECOVERY_INIT_SYNC_METHOD_SYNCFS
}			RecoveryInitSyncMethod;

typedef int File;


#define IO_DIRECT_DATA			0x01
#define IO_DIRECT_WAL			0x02
#define IO_DIRECT_WAL_INIT		0x04


/* GUC parameter */
extern PGDLLIMPORT int max_files_per_process;
extern PGDLLIMPORT bool data_sync_retry;
extern PGDLLIMPORT int recovery_init_sync_method;
extern PGDLLIMPORT int io_direct_flags;

/*
 * This is private to fd.c, but exported for save/restore_backend_variables()
 */
/*
 *这是fd.c私有的，但导出为save/restore_backend_variables（）
 */
extern PGDLLIMPORT int max_safe_fds;

/*
 * On Windows, we have to interpret EACCES as possibly meaning the same as
 * ENOENT, because if a file is unlinked-but-not-yet-gone on that platform,
 * that's what you get.  Ugh.  This code is designed so that we don't
 * actually believe these cases are okay without further evidence (namely,
 * a pending fsync request getting canceled ... see ProcessSyncRequests).
 */
/*
 *在Windows上，我们必须理解EACCES可能和ENOENT的含义相同，因为如果文件在该平台上是解绑但还没消失的，那就是你得到的。
 *呃。这段代码的设计目的是让我们在没有进一步证据的情况下（比如一个待处理的fsync请求被取消......参见 ProcessSyncRequests 。
 */
#ifndef WIN32
#define FILE_POSSIBLY_DELETED(err)	((err) == ENOENT)
#else
#define FILE_POSSIBLY_DELETED(err)	((err) == ENOENT || (err) == EACCES)
#endif

/*
 * O_DIRECT is not standard, but almost every Unix has it.  We translate it
 * to the appropriate Windows flag in src/port/open.c.  We simulate it with
 * fcntl(F_NOCACHE) on macOS inside fd.c's open() wrapper.  We use the name
 * PG_O_DIRECT rather than defining O_DIRECT in that case (probably not a good
 * idea on a Unix).  We can only use it if the compiler will correctly align
 * PGIOAlignedBlock for us, though.
 */
/*
 * O_DIRECT不是标准，但几乎所有Unix都有。 我们翻译它
 * 映射到src/port/open.c中相应的Windows标志。 我们用 来模拟
 * FCNTL（F_NOCACHE） 在 macOS 上的 FD.C Open（） 封装内。 我们用这个名字
 * PG_O_DIRECT 而不是定义O_DIRECT（可能不好）
 * Unix 上的创意）。 只有编译器能够正确对齐时，我们才能使用它
 * 不过我们用的是PGIOAlignedBlock。
 */
#if defined(O_DIRECT) && defined(pg_attribute_aligned)
#define		PG_O_DIRECT O_DIRECT
#elif defined(F_NOCACHE)
#define		PG_O_DIRECT 0x80000000
#define		PG_O_DIRECT_USE_F_NOCACHE
#else
#define		PG_O_DIRECT 0
#endif

/*
 * prototypes for functions in fd.c
 */

/* Operations on virtual Files --- equivalent to Unix kernel file ops */
extern File PathNameOpenFile(const char *fileName, int fileFlags);
extern File PathNameOpenFilePerm(const char *fileName, int fileFlags, mode_t fileMode);
extern File OpenTemporaryFile(bool interXact);
extern void FileClose(File file);
extern int	FilePrefetch(File file, off_t offset, off_t amount, uint32 wait_event_info);
extern int	FileRead(File file, void *buffer, size_t amount, off_t offset, uint32 wait_event_info);
extern int	FileWrite(File file, const void *buffer, size_t amount, off_t offset, uint32 wait_event_info);
extern int	FileSync(File file, uint32 wait_event_info);
extern int	FileZero(File file, off_t offset, off_t amount, uint32 wait_event_info);
extern int	FileFallocate(File file, off_t offset, off_t amount, uint32 wait_event_info);

extern off_t FileSize(File file);
extern int	FileTruncate(File file, off_t offset, uint32 wait_event_info);
extern void FileWriteback(File file, off_t offset, off_t nbytes, uint32 wait_event_info);
extern char *FilePathName(File file);
extern int	FileGetRawDesc(File file);
extern int	FileGetRawFlags(File file);
extern mode_t FileGetRawMode(File file);

/* Operations used for sharing named temporary files */
extern File PathNameCreateTemporaryFile(const char *path, bool error_on_failure);
extern File PathNameOpenTemporaryFile(const char *path, int mode);
extern bool PathNameDeleteTemporaryFile(const char *path, bool error_on_failure);
extern void PathNameCreateTemporaryDir(const char *basedir, const char *directory);
extern void PathNameDeleteTemporaryDir(const char *dirname);
extern void TempTablespacePath(char *path, Oid tablespace);

/* Operations that allow use of regular stdio --- USE WITH CAUTION */
extern FILE *AllocateFile(const char *name, const char *mode);
extern int	FreeFile(FILE *file);

/* Operations that allow use of pipe streams (popen/pclose) */
extern FILE *OpenPipeStream(const char *command, const char *mode);
extern int	ClosePipeStream(FILE *file);

/* Operations to allow use of the <dirent.h> library routines */
extern DIR *AllocateDir(const char *dirname);
extern struct dirent *ReadDir(DIR *dir, const char *dirname);
extern struct dirent *ReadDirExtended(DIR *dir, const char *dirname,
									  int elevel);
extern int	FreeDir(DIR *dir);

/* Operations to allow use of a plain kernel FD, with automatic cleanup */
extern int	OpenTransientFile(const char *fileName, int fileFlags);
extern int	OpenTransientFilePerm(const char *fileName, int fileFlags, mode_t fileMode);
extern int	CloseTransientFile(int fd);

/* If you've really really gotta have a plain kernel FD, use this */
extern int	BasicOpenFile(const char *fileName, int fileFlags);
extern int	BasicOpenFilePerm(const char *fileName, int fileFlags, mode_t fileMode);

/* Use these for other cases, and also for long-lived BasicOpenFile FDs */
extern bool AcquireExternalFD(void);
extern void ReserveExternalFD(void);
extern void ReleaseExternalFD(void);

/* Make a directory with default permissions */
extern int	MakePGDirectory(const char *directoryName);

/* Miscellaneous support routines */
extern void InitFileAccess(void);
extern void InitTemporaryFileAccess(void);
extern void set_max_safe_fds(void);
extern void closeAllVfds(void);
extern void SetTempTablespaces(Oid *tableSpaces, int numSpaces);
extern bool TempTablespacesAreSet(void);
extern int	GetTempTablespaces(Oid *tableSpaces, int numSpaces);
extern Oid	GetNextTempTableSpace(void);
extern void AtEOXact_Files(bool isCommit);
extern void AtEOSubXact_Files(bool isCommit, SubTransactionId mySubid,
							  SubTransactionId parentSubid);
extern void RemovePgTempFiles(void);
extern void RemovePgTempFilesInDir(const char *tmpdirname, bool missing_ok,
								   bool unlink_all);
extern bool looks_like_temp_rel_name(const char *name);

extern int	pg_fsync(int fd);
extern int	pg_fsync_no_writethrough(int fd);
extern int	pg_fsync_writethrough(int fd);
extern int	pg_fdatasync(int fd);
extern void pg_flush_data(int fd, off_t offset, off_t nbytes);
extern int	pg_truncate(const char *path, off_t length);
extern void fsync_fname(const char *fname, bool isdir);
extern int	fsync_fname_ext(const char *fname, bool isdir, bool ignore_perm, int elevel);
extern int	durable_rename(const char *oldfile, const char *newfile, int elevel);
extern int	durable_unlink(const char *fname, int elevel);
extern void SyncDataDirectory(void);
extern int	data_sync_elevel(int elevel);

/* Filename components */
#define PG_TEMP_FILES_DIR "pgsql_tmp"
#define PG_TEMP_FILE_PREFIX "pgsql_tmp"

#endif							/* FD_H */
