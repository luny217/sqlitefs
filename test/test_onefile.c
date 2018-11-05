/*
** 2007 September 14
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** OVERVIEW:
**
**   This file contains some example code demonstrating how the SQLite
**   vfs feature can be used to have SQLite operate directly on an
**   embedded media, without using an intermediate file system.
**
**   Because this is only a demo designed to run on a workstation, the
**   underlying media is simulated using a regular file-system file. The
**   size of the file is fixed when it is first created (default size 10 MB).
**   From SQLite's point of view, this space is used to store a single
**   database file and the journal file.
**
**   Any statement journal created is stored in volatile memory obtained
**   from sqlite3_malloc(). Any attempt to create a temporary database file
**   will fail (SQLITE_IOERR). To prevent SQLite from attempting this,
**   it should be configured to store all temporary database files in
**   main memory (see pragma "temp_store" or the SQLITE_TEMP_STORE compile
**   time option).
**
** ASSUMPTIONS:
**
**   After it has been created, the blob file is accessed using the
**   following three functions only:
**
**       mediaRead();            - Read a 512 byte block from the file.
**       mediaWrite();           - Write a 512 byte block to the file.
**       mediaSync();            - Tell the media hardware to sync.
**
**   It is assumed that these can be easily implemented by any "real"
**   media vfs driver adapting this code.
**
** FILE FORMAT:
**
**   The basic principle is that the "database file" is stored at the
**   beginning of the 10 MB blob and grows in a forward direction. The
**   "journal file" is stored at the end of the 10MB blob and grows
**   in the reverse direction. If, during a transaction, insufficient
**   space is available to expand either the journal or database file,
**   an SQLITE_FULL error is returned. The database file is never allowed
**   to consume more than 90% of the blob space. If SQLite tries to
**   create a file larger than this, SQLITE_FULL is returned.
**
**   No allowance is made for "wear-leveling", as is required by.
**   embedded devices in the absence of equivalent hardware features.
**
**   The first 512 block byte of the file is reserved for storing the
**   size of the "database file". It is updated as part of the sync()
**   operation. On startup, it can only be trusted if no journal file
**   exists. If a journal-file does exist, then it stores the real size
**   of the database region. The second and subsequent blocks store the
**   actual database content.
**
**   The size of the "journal file" is not stored persistently in the
**   file. When the system is running, the size of the journal file is
**   stored in volatile memory. When recovering from a crash, this vfs
**   reports a very large size for the journal file. The normal journal
**   header and checksum mechanisms serve to prevent SQLite from
**   processing any data that lies past the logical end of the journal.
**
**   When SQLite calls OsDelete() to delete the journal file, the final
**   512 bytes of the blob (the area containing the first journal header)
**   are zeroed.
**
** LOCKING:
**
**   File locking is a no-op. Only one connection may be open at any one
**   time using this demo vfs.
*/

#include "sqlite3.h"
#include <assert.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include "win32io.h"
#endif // _WIN32

#include "av_log.h"

/*
** Maximum pathname length supported by the fs backend.
*/
#define BLOCKSIZE 512
#define BLOBSIZE 10485760

/*
** Name used to identify this VFS.
*/
#define FS_VFS_NAME "HB_SQL"

typedef struct _fs_real_file fs_real_file;
struct _fs_real_file
{
    sqlite3_file * pFile;
    const char * zName;
    int nDatabase;              /* Current size of database region 数据库区域大小*/
    int nJournal;               /* Current size of journal region 日志区域大小*/
    int nBlob;                  /* Total size of allocated blob 分配的总字节数*/
    int nRef;                   /* Number of pointers to this structure */
    fs_real_file * pNext;
    fs_real_file ** ppThis;
};

typedef struct fs_file fs_file;
struct fs_file
{
    sqlite3_file base;
    int eType;
    fs_real_file * pReal;
};

/* Values for fs_file.eType. */
#define DATABASE_FILE   1
#define JOURNAL_FILE    2

/* Useful macros used in several places */
#define MIN(x,y) ((x)<(y)?(x):(y))
#define MAX(x,y) ((x)>(y)?(x):(y))

/*
** Method declarations for fs_file.
*/
static int fsClose(sqlite3_file *);
static int fsRead(sqlite3_file *, void *, int iAmt, sqlite3_int64 iOfst);
static int fsWrite(sqlite3_file *, const void *, int iAmt, sqlite3_int64 iOfst);
static int fsTruncate(sqlite3_file *, sqlite3_int64 size);
static int fsSync(sqlite3_file *, int flags);
static int fsFileSize(sqlite3_file *, sqlite3_int64 * pSize);
static int fsLock(sqlite3_file *, int);
static int fsUnlock(sqlite3_file *, int);
static int fsCheckReservedLock(sqlite3_file *, int * pResOut);
static int fsFileControl(sqlite3_file *, int op, void * pArg);
static int fsSectorSize(sqlite3_file *);
static int fsDeviceCharacteristics(sqlite3_file *);

/*
** Method declarations for fs_vfs.
*/
static int fsOpen(sqlite3_vfs *, const char *, sqlite3_file *, int , int *);
static int fsDelete(sqlite3_vfs *, const char * zName, int syncDir);
static int fsAccess(sqlite3_vfs *, const char * zName, int flags, int *);
static int fsFullPathname(sqlite3_vfs *, const char * zName, int nOut, char * zOut);
static void * fsDlOpen(sqlite3_vfs *, const char * zFilename);
static void fsDlError(sqlite3_vfs *, int nByte, char * zErrMsg);
static void (*fsDlSym(sqlite3_vfs *, void *, const char * zSymbol))(void);
static void fsDlClose(sqlite3_vfs *, void *);
static int fsRandomness(sqlite3_vfs *, int nByte, char * zOut);
static int fsSleep(sqlite3_vfs *, int microseconds);
static int fsCurrentTime(sqlite3_vfs *, double *);

int fs_open(sqlite3_vfs * pVfs, const char * zName, sqlite3_file * pFile, int flags, int * pOutFlags);
int fs_read(sqlite3_file * pFile, void * zBuf, int iAmt, sqlite_int64 iOfst);


struct _fs_vfs_t
{
    sqlite3_vfs base;
    fs_real_file * pFileList;
    sqlite3_vfs * pParent;
};
typedef struct _fs_vfs_t fs_vfs_t;

static fs_vfs_t fs_vfs =
{
    {
        1,                                          /* iVersion */
        0,                                          /* szOsFile */
        0,                                          /* mxPathname */
        0,                                          /* pNext */
        FS_VFS_NAME,                                /* zName */
        0,                                          /* pAppData */
        fs_open,                                     /* xOpen */
        fsDelete,                                   /* xDelete */
        fsAccess,                                   /* xAccess */
        fsFullPathname,                             /* xFullPathname */
        fsDlOpen,                                   /* xDlOpen */
        fsDlError,                                  /* xDlError */
        fsDlSym,                                    /* xDlSym */
        fsDlClose,                                  /* xDlClose */
        fsRandomness,                               /* xRandomness */
        fsSleep,                                    /* xSleep */
        fsCurrentTime,                              /* xCurrentTime */
        0                                           /* xCurrentTimeInt64 */
    },
    0,                                            /* pFileList */
    0                                             /* pParent */
};

static sqlite3_io_methods fs_io_methods =
{
    1,                            /* iVersion */
    fsClose,                      /* xClose */
    fs_read,                       /* xRead */
    fsWrite,                      /* xWrite */
    fsTruncate,                   /* xTruncate */
    fsSync,                       /* xSync */
    fsFileSize,                   /* xFileSize */
    fsLock,                       /* xLock */
    fsUnlock,                     /* xUnlock */
    fsCheckReservedLock,          /* xCheckReservedLock */
    fsFileControl,                /* xFileControl */
    fsSectorSize,                 /* xSectorSize */
    fsDeviceCharacteristics,      /* xDeviceCharacteristics */
    0,                            /* xShmMap */
    0,                            /* xShmLock */
    0,                            /* xShmBarrier */
    0                             /* xShmUnmap */
};

/*
** struct mfs_sqlite_vfs_real_file
*/
typedef struct _sqvfs_rfile_t
{
    sqlite3_file * sql_file;
    int8_t * name;
    int32_t sz_database;              /* 数据库区域大小*/
    int32_t sz_journal;               /* 日志区域大小*/
    int32_t sz_blob;                  /* 分配的总字节数*/
    int32_t sz_ref;                   /* 引用数 */
    struct _sqvfs_rfile_t * ptr_next;
    struct _sqvfs_rfile_t ** pptr_this;
} sqvfs_rfile_t;

typedef struct _mfs_sqlite_vfs_t
{
    sqlite3_file base;
    int32_t fd;
    int32_t etype;
    sqvfs_rfile_t * real_file;
} mfs_sqlite_vfs_t;

int xopen(const char * zname)
{
	int32_t fd;	

#ifdef _WIN32
	TCHAR * dev = TEXT("\\\\.\\") TEXT("H:");
	fd = xopen_win32(dev);
#else
	fd = xopen_linux(dev);
#endif // _WIN32

	if (fd < 0)
	{
		av_log(AV_LOG_ERROR, "xopen error!\n");
	}
	return fd;
}

int xclose()
{
	return 0;
}

int xread(int32_t fd, void * buf, int size, sqlite_int64 offset)
{
	int rc = SQLITE_OK;

#ifdef _WIN32
	rc = xread_win32(fd, buf, size, offset);
#else
	fd = xopen_linux(fd, buf, size, offset);
#endif // _WIN32

	return rc;
}

int xwrite(int32_t fd, void * buf, int size, sqlite_int64 offset)
{
	int rc = SQLITE_OK;

#ifdef _WIN32
	rc = xwrite_win32(fd, buf, size, offset);
#else
	fd = xopen_linux(fd, buf, size, offset);
#endif // _WIN32

	return rc;
}

int xsize(int32_t fd, int32_t *psize)
{
	int rc = SQLITE_OK;
	unsigned char zs[4];

#if 0
	rc = xread(fd, zs, 4, 0);/*读取偏移0的4字节*/
	*psize = (zs[0] << 24) + (zs[1] << 16) + (zs[2] << 8) + zs[3];/*数据库大小*/
	if (rc == SQLITE_OK)
	{
		rc = xread(p->fd, zs, 4, pReal->nBlob - 4);
		if (zS[0] || zS[1] || zS[2] || zS[3])
		{
			pReal->nJournal = pReal->nBlob;/*倒数4字节为日志文件大小*/
		}
	}
#endif
	return 0;
}

int xsync()
{
	return 0;
}

int fs_open(sqlite3_vfs * pVfs, const char * zName, sqlite3_file * pFile, int flags, int * pOutFlags)
{
    mfs_sqlite_vfs_t * p = (mfs_sqlite_vfs_t *)pFile;
    sqvfs_rfile_t * real_file = NULL;
    int rc = SQLITE_OK;
    int eType;
    int nName;

    eType = ((flags & (SQLITE_OPEN_MAIN_DB)) ? DATABASE_FILE : JOURNAL_FILE);

    p->etype = eType;

    nName = (int)strlen(zName) - ((eType == JOURNAL_FILE) ? 8 : 0);

    p->base.pMethods = &fs_io_methods;

    if (!real_file)
    {
		sqlite3_int64 size = 0;
		uint8_t zs[512] = {0};
        real_file = sqlite3_malloc(sizeof(sqvfs_rfile_t));
        if (!real_file)
        {
            rc = SQLITE_NOMEM;
            goto open_out;
        }
        memset(real_file, 0, sizeof(sqvfs_rfile_t));

        p->fd = xopen(zName);
		if (p->fd < 0)
		{
			rc = SQLITE_ERROR;
			av_log(AV_LOG_ERROR, "xopen error!\n");
			goto open_out;
		}

        p->real_file = real_file;

        real_file->name  = zName;
        real_file->sz_blob = BLOBSIZE;

#if 0
		rc = xsize(p->fd, &size);/*获取数据库大小*/
		if (rc != SQLITE_OK)
		{
			goto open_out;
		}		
		real_file->sz_blob = (int)size;
#endif

		rc = xread(p->fd, zs, 512, 0);/*读取偏移0的4字节*/
		if (rc != SQLITE_OK)
		{
			goto open_out;
		}
		real_file->sz_database = (zs[0] << 24) + (zs[1] << 16) + (zs[2] << 8) + zs[3];/*数据库大小*/
		if (real_file->sz_database == 0)
		{
			rc = xwrite(p->fd, "\0", 1, BLOBSIZE - 1);/*大小为0，则写入默认大小*/
			if (rc != SQLITE_OK)
			{
				goto open_out;
			}
			real_file->sz_blob = BLOBSIZE;
		}

		rc = xread(p->fd, zs, 4, real_file->sz_blob - 4);
		if (rc != SQLITE_OK)
		{
			goto open_out;
		}
		if (zs[0] || zs[1] || zs[2] || zs[3])
		{
			real_file->sz_journal = real_file->sz_blob;/*倒数4字节为日志文件大小*/
		}        
    }

open_out:

    return rc;
}

int fs_close(sqlite3_file * pFile)
{
    return 0;
}

int fs_read(sqlite3_file * pFile, void * zBuf, int iAmt, sqlite_int64 iOfst)
{
    int rc = SQLITE_OK;
    mfs_sqlite_vfs_t * p = (mfs_sqlite_vfs_t *)pFile;

    fs_real_file * pReal = p->real_file;
    sqlite3_file * pF = pReal->pFile;




    if ((p->etype == DATABASE_FILE && (iAmt + iOfst) > pReal->nDatabase)
            || (p->etype == JOURNAL_FILE && (iAmt + iOfst) > pReal->nJournal)
       )
    {
        rc = SQLITE_IOERR_SHORT_READ;
    }
    else if (p->etype == DATABASE_FILE)
    {
        rc = pF->pMethods->xRead(pF, zBuf, iAmt, iOfst + BLOCKSIZE);
    }
    else

    {
        /* Journal file. */
        int iRem = iAmt;
        int iBuf = 0;
        int ii = (int)iOfst;
        while (iRem > 0 && rc == SQLITE_OK)
        {
            int iRealOff = pReal->nBlob - BLOCKSIZE * ((ii / BLOCKSIZE) + 1) + ii % BLOCKSIZE;
            int iRealAmt = MIN(iRem, BLOCKSIZE - (iRealOff % BLOCKSIZE));

            rc = pF->pMethods->xRead(pF, &((char *)zBuf)[iBuf], iRealAmt, iRealOff);
            ii += iRealAmt;
            iBuf += iRealAmt;
            iRem -= iRealAmt;
        }
    }


    return rc;
}

int fs_write(sqlite3_file * pFile, const void * zBuf, int iAmt, sqlite_int64 iOfst)
{
    return 0;
}

/*
** Open an fs file handle.
*/
static int fsOpen(sqlite3_vfs * pVfs, const char * zName, sqlite3_file * pFile, int flags, int * pOutFlags)
{
    fs_vfs_t * pFsVfs = (fs_vfs_t *)pVfs;
    fs_file * p = (fs_file *)pFile;
    fs_real_file * pReal = 0;
    int eType;
    int nName;
    int rc = SQLITE_OK;

    eType = ((flags & (SQLITE_OPEN_MAIN_DB)) ? DATABASE_FILE : JOURNAL_FILE);
    p->base.pMethods = &fs_io_methods;
    p->eType = eType;

    assert(strlen("-journal") == 8);
    nName = (int)strlen(zName) - ((eType == JOURNAL_FILE) ? 8 : 0);
    pReal = pFsVfs->pFileList;
    for (; pReal && strncmp(pReal->zName, zName, nName); pReal = pReal->pNext);

    if (!pReal)
    {
        int real_flags = (flags&~(SQLITE_OPEN_MAIN_DB)) | SQLITE_OPEN_TEMP_DB;
        sqlite3_int64 size;
        sqlite3_file * pRealFile;
        sqlite3_vfs * pParent = pFsVfs->pParent;
        assert(eType == DATABASE_FILE);

        pReal = (fs_real_file *)sqlite3_malloc(sizeof(*pReal) + pParent->szOsFile);
        if (!pReal)
        {
            rc = SQLITE_NOMEM;
            goto open_out;
        }
        memset(pReal, 0, sizeof(*pReal) + pParent->szOsFile);
        pReal->zName = zName;
        pReal->pFile = (sqlite3_file *)(&pReal[1]);

        rc = pParent->xOpen(pParent, zName, pReal->pFile, real_flags, pOutFlags);
        if (rc != SQLITE_OK)
        {
            goto open_out;
        }
        pRealFile = pReal->pFile;

        rc = pRealFile->pMethods->xFileSize(pRealFile, &size);/*获取数据库大小*/
        if (rc != SQLITE_OK)
        {
            goto open_out;
        }
        if (size == 0)
        {
            rc = pRealFile->pMethods->xWrite(pRealFile, "\0", 1, BLOBSIZE - 1);/*大小为0，则写入默认大小*/
            pReal->nBlob = BLOBSIZE;
        }
        else
        {
            unsigned char zS[4];
            pReal->nBlob = (int)size;
            rc = pRealFile->pMethods->xRead(pRealFile, zS, 4, 0);/*读取偏移0的4字节*/
            pReal->nDatabase = (zS[0] << 24) + (zS[1] << 16) + (zS[2] << 8) + zS[3];/*数据库大小*/
            if (rc == SQLITE_OK)
            {
                rc = pRealFile->pMethods->xRead(pRealFile, zS, 4, pReal->nBlob - 4);
                if (zS[0] || zS[1] || zS[2] || zS[3])
                {
                    pReal->nJournal = pReal->nBlob;/*倒数4字节为日志文件大小*/
                }
            }
        }

        if (rc == SQLITE_OK)
        {
            pReal->pNext = pFsVfs->pFileList;
            if (pReal->pNext)
            {
                pReal->pNext->ppThis = &pReal->pNext;
            }
            pReal->ppThis = &pFsVfs->pFileList;
            pFsVfs->pFileList = pReal;
        }
    }

open_out:
    if (pReal)
    {
        if (rc == SQLITE_OK)
        {
            p->pReal = pReal;
            pReal->nRef++;
        }
        else
        {
            if (pReal->pFile->pMethods)
            {
                pReal->pFile->pMethods->xClose(pReal->pFile);
            }
            sqlite3_free(pReal);
        }
    }
    return rc;
}

/*
** Close an fs-file.
*/
static int fsClose(sqlite3_file * pFile)
{
    int rc = SQLITE_OK;
    fs_file * p = (fs_file *)pFile;
    fs_real_file * pReal = p->pReal;

    /* Decrement the real_file ref-count. */
    pReal->nRef--;
    assert(pReal->nRef >= 0);

    /* When the ref-count reaches 0, destroy the structure */
    if (pReal->nRef == 0)
    {
        *pReal->ppThis = pReal->pNext;
        if (pReal->pNext)
        {
            pReal->pNext->ppThis = pReal->ppThis;
        }
        rc = pReal->pFile->pMethods->xClose(pReal->pFile);
        sqlite3_free(pReal);
    }

    return rc;
}

/*
** Read data from an fs-file.
** iAmt 字节数
*/
static int fsRead(sqlite3_file * pFile, void * zBuf, int iAmt, sqlite_int64 iOfst)
{
    int rc = SQLITE_OK;
    fs_file * p = (fs_file *)pFile;
    fs_real_file * pReal = p->pReal;
    sqlite3_file * pF = pReal->pFile;

    if ((p->eType == DATABASE_FILE && (iAmt + iOfst) > pReal->nDatabase)
            || (p->eType == JOURNAL_FILE && (iAmt + iOfst) > pReal->nJournal)
       )
    {
        rc = SQLITE_IOERR_SHORT_READ;
    }
    else if (p->eType == DATABASE_FILE)
    {
        rc = pF->pMethods->xRead(pF, zBuf, iAmt, iOfst + BLOCKSIZE);
    }
    else
    {
        /* Journal file. */
        int iRem = iAmt;
        int iBuf = 0;
        int ii = (int)iOfst;
        while (iRem > 0 && rc == SQLITE_OK)
        {
            int iRealOff = pReal->nBlob - BLOCKSIZE * ((ii / BLOCKSIZE) + 1) + ii % BLOCKSIZE;
            int iRealAmt = MIN(iRem, BLOCKSIZE - (iRealOff % BLOCKSIZE));

            rc = pF->pMethods->xRead(pF, &((char *)zBuf)[iBuf], iRealAmt, iRealOff);
            ii += iRealAmt;
            iBuf += iRealAmt;
            iRem -= iRealAmt;
        }
    }

    return rc;
}

/*
** Write data to an fs-file.
** iAmt 字节数
*/
static int fsWrite(sqlite3_file * pFile, const void * zBuf, int iAmt, sqlite_int64 iOfst)
{
    int rc = SQLITE_OK;
    fs_file * p = (fs_file *)pFile;
    fs_real_file * pReal = p->pReal;
    sqlite3_file * pF = pReal->pFile;

    if (p->eType == DATABASE_FILE)
    {
        if ((iAmt + iOfst + BLOCKSIZE) > (pReal->nBlob - pReal->nJournal))
        {
            rc = SQLITE_FULL;
        }
        else
        {
            rc = pF->pMethods->xWrite(pF, zBuf, iAmt, iOfst + BLOCKSIZE);
            if (rc == SQLITE_OK)
            {
                pReal->nDatabase = (int)MAX(pReal->nDatabase, iAmt + iOfst);
            }
        }
    }
    else
    {
        /* Journal file. */
        int iRem = iAmt;
        int iBuf = 0;
        int ii = (int)iOfst;
        while (iRem > 0 && rc == SQLITE_OK)
        {
            int iRealOff = pReal->nBlob - BLOCKSIZE * ((ii / BLOCKSIZE) + 1) + ii % BLOCKSIZE;
            int iRealAmt = MIN(iRem, BLOCKSIZE - (iRealOff % BLOCKSIZE));

            if (iRealOff < (pReal->nDatabase + BLOCKSIZE))
            {
                rc = SQLITE_FULL;
            }
            else
            {
                rc = pF->pMethods->xWrite(pF, &((char *)zBuf)[iBuf], iRealAmt, iRealOff);
                ii += iRealAmt;
                iBuf += iRealAmt;
                iRem -= iRealAmt;
            }
        }
        if (rc == SQLITE_OK)
        {
            pReal->nJournal = (int)MAX(pReal->nJournal, iAmt + iOfst);
        }
    }

    return rc;
}

/*
** Truncate an fs-file.
*/
static int fsTruncate(sqlite3_file * pFile, sqlite_int64 size)
{
    fs_file * p = (fs_file *)pFile;
    fs_real_file * pReal = p->pReal;
    if (p->eType == DATABASE_FILE)
    {
        pReal->nDatabase = (int)MIN(pReal->nDatabase, size);
    }
    else
    {
        pReal->nJournal = (int)MIN(pReal->nJournal, size);
    }
    return SQLITE_OK;
}

/*
** Sync an fs-file.
*/
static int fsSync(sqlite3_file * pFile, int flags)
{
    fs_file * p = (fs_file *)pFile;
    fs_real_file * pReal = p->pReal;
    sqlite3_file * pRealFile = pReal->pFile;
    int rc = SQLITE_OK;

    if (p->eType == DATABASE_FILE)
    {
        unsigned char zSize[4];
        zSize[0] = (pReal->nDatabase & 0xFF000000) >> 24;
        zSize[1] = (unsigned char)((pReal->nDatabase & 0x00FF0000) >> 16);
        zSize[2] = (pReal->nDatabase & 0x0000FF00) >> 8;
        zSize[3] = (pReal->nDatabase & 0x000000FF);
        rc = pRealFile->pMethods->xWrite(pRealFile, zSize, 4, 0);
    }
    if (rc == SQLITE_OK)
    {
        rc = pRealFile->pMethods->xSync(pRealFile, flags & (~SQLITE_SYNC_DATAONLY));
    }

    return rc;
}

/*
** Return the current file-size of an fs-file.
*/
static int fsFileSize(sqlite3_file * pFile, sqlite_int64 * pSize)
{
    fs_file * p = (fs_file *)pFile;
    fs_real_file * pReal = p->pReal;
    if (p->eType == DATABASE_FILE)
    {
        *pSize = pReal->nDatabase;
    }
    else
    {
        *pSize = pReal->nJournal;
    }
    return SQLITE_OK;
}

/*
** Lock an fs-file.
*/
static int fsLock(sqlite3_file * pFile, int eLock)
{
    return SQLITE_OK;
}

/*
** Unlock an fs-file.
*/
static int fsUnlock(sqlite3_file * pFile, int eLock)
{
    return SQLITE_OK;
}

/*
** Check if another file-handle holds a RESERVED lock on an fs-file.
*/
static int fsCheckReservedLock(sqlite3_file * pFile, int * pResOut)
{
    *pResOut = 0;
    return SQLITE_OK;
}

/*
** File control method. For custom operations on an fs-file.
*/
static int fsFileControl(sqlite3_file * pFile, int op, void * pArg)
{
    return SQLITE_OK;
}

/*
** Return the sector-size in bytes for an fs-file.
*/
static int fsSectorSize(sqlite3_file * pFile)
{
    return BLOCKSIZE;
}

/*
** Return the device characteristic flags supported by an fs-file.
*/
static int fsDeviceCharacteristics(sqlite3_file * pFile)
{
    return 0;
}

/*
** Delete the file located at zPath. If the dirSync argument is true,
** ensure the file-system modifications are synced to disk before
** returning.
*/
static int fsDelete(sqlite3_vfs * pVfs, const char * zPath, int dirSync)
{
    int rc = SQLITE_OK;
    fs_vfs_t * pFsVfs = (fs_vfs_t *)pVfs;
    fs_real_file * pReal;
    sqlite3_file * pF;
    int nName = (int)strlen(zPath) - 8;

    assert(strlen("-journal") == 8);
    assert(strcmp("-journal", &zPath[nName]) == 0);

    pReal = pFsVfs->pFileList;
    for (; pReal && strncmp(pReal->zName, zPath, nName); pReal = pReal->pNext);
    if (pReal)
    {
        pF = pReal->pFile;
        rc = pF->pMethods->xWrite(pF, "\0\0\0\0", 4, pReal->nBlob - BLOCKSIZE);
        if (rc == SQLITE_OK)
        {
            pReal->nJournal = 0;
        }
    }
    return rc;
}

/*
** Test for access permissions. Return true if the requested permission
** is available, or false otherwise.
*/
static int fsAccess(sqlite3_vfs * pVfs, const char * zPath, int flags, int * pResOut)
{
    fs_vfs_t * pFsVfs = (fs_vfs_t *)pVfs;
    fs_real_file * pReal;
    int isJournal = 0;
    int nName = (int)strlen(zPath);

    if (flags != SQLITE_ACCESS_EXISTS)
    {
        sqlite3_vfs * pParent = ((fs_vfs_t *)pVfs)->pParent;
        return pParent->xAccess(pParent, zPath, flags, pResOut);
    }

    assert(strlen("-journal") == 8);
    if (nName > 8 && strcmp("-journal", &zPath[nName - 8]) == 0)
    {
        nName -= 8;
        isJournal = 1;
    }

    pReal = pFsVfs->pFileList;
    for (; pReal && strncmp(pReal->zName, zPath, nName); pReal = pReal->pNext);

    *pResOut = (pReal && (!isJournal || pReal->nJournal > 0));
    return SQLITE_OK;
}

/*
** Populate buffer zOut with the full canonical pathname corresponding
** to the pathname in zPath. zOut is guaranteed to point to a buffer
** of at least (FS_MAX_PATHNAME+1) bytes.
*/
static int fsFullPathname(
    sqlite3_vfs * pVfs,           /* Pointer to vfs object */
    const char * zPath,           /* Possibly relative input path */
    int nOut,                     /* Size of output buffer in bytes */
    char * zOut                   /* Output buffer */
)
{
    sqlite3_vfs * pParent = ((fs_vfs_t *)pVfs)->pParent;
    return pParent->xFullPathname(pParent, zPath, nOut, zOut);
}

/*
** Open the dynamic library located at zPath and return a handle.
*/
static void * fsDlOpen(sqlite3_vfs * pVfs, const char * zPath)
{
    sqlite3_vfs * pParent = ((fs_vfs_t *)pVfs)->pParent;
    return pParent->xDlOpen(pParent, zPath);
}

/*
** Populate the buffer zErrMsg (size nByte bytes) with a human readable
** utf-8 string describing the most recent error encountered associated
** with dynamic libraries.
*/
static void fsDlError(sqlite3_vfs * pVfs, int nByte, char * zErrMsg)
{
    sqlite3_vfs * pParent = ((fs_vfs_t *)pVfs)->pParent;
    pParent->xDlError(pParent, nByte, zErrMsg);
}

/*
** Return a pointer to the symbol zSymbol in the dynamic library pHandle.
*/
static void (*fsDlSym(sqlite3_vfs * pVfs, void * pH, const char * zSym))(void)
{
    sqlite3_vfs * pParent = ((fs_vfs_t *)pVfs)->pParent;
    return pParent->xDlSym(pParent, pH, zSym);
}

/*
** Close the dynamic library handle pHandle.
*/
static void fsDlClose(sqlite3_vfs * pVfs, void * pHandle)
{
    sqlite3_vfs * pParent = ((fs_vfs_t *)pVfs)->pParent;
    pParent->xDlClose(pParent, pHandle);
}

/*
** Populate the buffer pointed to by zBufOut with nByte bytes of
** random data.
*/
static int fsRandomness(sqlite3_vfs * pVfs, int nByte, char * zBufOut)
{
    sqlite3_vfs * pParent = ((fs_vfs_t *)pVfs)->pParent;
    return pParent->xRandomness(pParent, nByte, zBufOut);
}

/*
** Sleep for nMicro microseconds. Return the number of microseconds
** actually slept.
*/
static int fsSleep(sqlite3_vfs * pVfs, int nMicro)
{
    sqlite3_vfs * pParent = ((fs_vfs_t *)pVfs)->pParent;
    return pParent->xSleep(pParent, nMicro);
}

/*
** Return the current time as a Julian Day number in *pTimeOut.
*/
static int fsCurrentTime(sqlite3_vfs * pVfs, double * pTimeOut)
{
    sqlite3_vfs * pParent = ((fs_vfs_t *)pVfs)->pParent;
    return pParent->xCurrentTime(pParent, pTimeOut);
}

/*
** This procedure registers the fs vfs with SQLite. If the argument is
** true, the fs vfs becomes the new default vfs. It is the only publicly
** available function in this file.
*/
int fs_register(void)
{
    if (fs_vfs.pParent) return SQLITE_OK;
    fs_vfs.pParent = sqlite3_vfs_find(0);
    fs_vfs.base.mxPathname = fs_vfs.pParent->mxPathname;
    fs_vfs.base.szOsFile = sizeof(fs_file);
    return sqlite3_vfs_register(&fs_vfs.base, 0);
}

#if 1 //def SQLITE_TEST
int SqlitetestOnefile_Init()
{
    return fs_register();
}
#endif
