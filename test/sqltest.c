#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "base.h"
#include "av_log.h"
#include "sqlite3.h"

#define SQLITE_DB   "nvr.db"
#define SQLBUF_SIZE (4*1024)

#define SQL_CREATE_TABLE_SEG    "CREATE TABLE if not exists segTable(devNo int1,partion int1,fileNo int2,channel int1,storeType int,\
startTime int,stopTime int,dataSize int,idxAmount int,idxIAmount int,segPos int,segAttr int1,packSerial int1);"

#define SQL_INSERT_TABLE_SEG    "insert into segTable values(%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d);"
#define SQL_INSERT_TABLE_SEG2   "insert into segTable values(?,?,?,?,?,?,?,?,?,?,?,?,?);"

#define SQL_SELECT_TABLE_SEG    "select * from segTable"
#define SQL_SELECT_TABLE_SEG2   "select * from segTable where channel == 11"
#define SQL_SELECT_TABLE_SEG3   "select * from segTable where channel == 11 and idxAmount >= 50"
#define SQL_SELECT_TABLE_SEG4   "select fileNo,channel,startTime,stopTime,dataSize from segTable where channel < 16"

extern int SqlitetestOnefile_Init();

n_timeval_t t_start;

void get_interval_start()
{
	get_current_time(&t_start);
}

int get_interval_end(int inter_msec, char * str)
{
    int retval;
    n_timeval_t t_end;
	get_current_time(&t_end);
    retval = (int)((t_end.tv_sec - t_start.tv_sec) * 1000 + (t_end.tv_usec - t_start.tv_usec) / 1000);
    if (retval >= inter_msec)
    {
        printf("%s inter_msec:%d\n", str, retval);
    }
    return 0;
}

int cbSelectTableSeg(void * NotUsed, int argc, char ** argv, char ** azColName)
{
    int i;
    static int cnt = 0;

    if (cnt == 0)
    {
        for (i = 0; i < argc; i++)
        {
            //printf("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
        }
    }

    if (cnt++ % 1000 == 0)
    {
        //printf("cnt:%d argc:%d\n", cnt, argc);
    }

    return 0;
}

#define SQL_INSERT_CNT (200*1000)
int segTableInsert(sqlite3 * db)
{
    int ret = 0, i = 0;
    char * errMsg = NULL;
    char sqlBuf[SQLBUF_SIZE] = {0};

    int devNo = 0, partion = 0, fileNo = 0, channel = 0, storeType = 0;
    int startTime = 0, stopTime = 0, dataSize = 0, idxAmount = 0, idxIAmount = 0, segPos = 0, segAttr = 0, packSerial = 0;

    /* 显式事务方式 */
    sqlite3_exec(db, "begin;", 0, 0, 0);
#if 0
    for (i = 0; i < SQL_INSERT_CNT; i++)
    {
        devNo = i % 2;
        fileNo = i / 32 % 1024;
        channel = i % 32;
        storeType = i & 0xf;
        startTime = 0x112233 + i * 100;
        stopTime = startTime + 99;
        dataSize = 1024 * 1024 + (i % 100) * 197 + i;
        idxAmount = (i % 7) * 13 + i % 19;
        idxIAmount = idxAmount / 25;
        segPos += dataSize;
        segAttr = i * 7 % 2;
        packSerial = i * 17 % 2;

        sprintf(sqlBuf, SQL_INSERT_TABLE_SEG, devNo, partion, fileNo, channel, storeType, startTime, stopTime, dataSize, idxAmount, idxIAmount, segPos, segAttr, packSerial);
        //printf("sqlBuf[%d]:%s\n", i, sqlBuf);

        ret = sqlite3_exec(db, sqlBuf, NULL, 0, &errMsg);
        CHECK_GOTOM(ret != SQLITE_OK, RELEASE, "sqlite3_exec error! %s\n", sqlite3_errmsg(db));
    }
#else
    sqlite3_stmt * stmt;
    ret = sqlite3_prepare_v2(db, SQL_INSERT_TABLE_SEG2, strlen(SQL_INSERT_TABLE_SEG2), &stmt, 0);
	if (ret != 0)
	{
		av_log(AV_LOG_ERROR, "sqlite3_prepare_v2 error! %s\n", sqlite3_errmsg(db));
		goto RELEASE;
	}

    for (i = 0; i < SQL_INSERT_CNT; i++)
    {
        devNo = i % 2;
        partion = i / 1024 % 2;
        fileNo = i / 32 % 1024;
        channel = i % 32;
        storeType = i & 0xf;
        startTime = 0x112233 + i * 100;
        stopTime = startTime + 99;
        dataSize = 1024 * 1024 + (i % 100) * 197 + i;
        idxAmount = (i % 7) * 13 + i % 19;
        idxIAmount = idxAmount / 25;
        segPos += dataSize;
        segAttr = i * 7 % 2;
        packSerial = i * 17 % 2;

        sqlite3_reset(stmt);
        sqlite3_bind_int(stmt, 1, devNo);
        sqlite3_bind_int(stmt, 2, partion);
        sqlite3_bind_int(stmt, 3, fileNo);
        sqlite3_bind_int(stmt, 4, channel);
        sqlite3_bind_int(stmt, 5, storeType);
        sqlite3_bind_int(stmt, 6, startTime);
        sqlite3_bind_int(stmt, 7, stopTime);
        sqlite3_bind_int(stmt, 8, dataSize);
        sqlite3_bind_int(stmt, 9, idxAmount);
        sqlite3_bind_int(stmt, 10, idxIAmount);
        sqlite3_bind_int(stmt, 11, segPos);
        sqlite3_bind_int(stmt, 12, segAttr);
        sqlite3_bind_int(stmt, 13, packSerial);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
#endif
RELEASE:
    sqlite3_exec(db, "commit;", 0, 0, 0);

    sqlite3_free(errMsg);

    return ret;
}

int main()
{
    int ret = 0;
    char * errMsg = NULL;
	int flags;	

	clock_win32_init();

	SqlitetestOnefile_Init();

    get_interval_start();

    get_interval_end(0,"sqlite3_open start!");
    sqlite3 * db = NULL;

#if 0
	ret = sqlite3_open(SQLITE_DB, &db);
	if (ret != SQLITE_OK)
	{
		av_log(AV_LOG_ERROR, "sqlite_open error! %s\n", sqlite3_errmsg(db));
		goto RELEASE;
	}
#endif

#if 1
	flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_MAIN_JOURNAL;

    ret = sqlite3_open_v2(SQLITE_DB, &db, flags, "HB_SQL");
	if (ret != 0)
	{
		av_log(AV_LOG_ERROR, "sqlite3_open error! %s\n", sqlite3_errmsg(db));
		goto RELEASE;
	}
#endif

	av_log(AV_LOG_INFO, "sqlite3_open ok\n");
    get_interval_end(0,"sqlite3_open end!");

    get_interval_end(0,"sqlite3_exec SQL_CREATE_TABLE_SEG start!");
    ret = sqlite3_exec(db, SQL_CREATE_TABLE_SEG, NULL, 0, &errMsg);
	if (ret != SQLITE_OK)
	{
		av_log(AV_LOG_ERROR, "sqlite3_exec error! %s\n", sqlite3_errmsg(db));
		goto RELEASE;
	}
	av_log(AV_LOG_INFO, "sqlite3_exec SQL_CREATE_TABLE_SEG ok\n");
    get_interval_end(0,"sqlite3_exec SQL_CREATE_TABLE_SEG end!");

#if 1
    get_interval_end(0,"sqlite3_exec segTableInsert start!\n");
    ret = segTableInsert(db);
	if (ret != SQLITE_OK)
	{
		av_log(AV_LOG_ERROR, "segTableInsert error! %s\n", sqlite3_errmsg(db));
		goto RELEASE;
	}
	av_log(AV_LOG_INFO, "sqlite3_exec segTableInsert ok\n");
    get_interval_end(0,"sqlite3_exec segTableInsert end!\n");
#endif

#if 0
	get_interval_end(0,"sqlite3_exec SQL_SELECT_TABLE_SEG start!");
    ret = sqlite3_exec(db, SQL_SELECT_TABLE_SEG4, cbSelectTableSeg, 0, &errMsg);
	if (ret != SQLITE_OK)
	{
		av_log(AV_LOG_ERROR, "sqlite3_exec error! %s\n", sqlite3_errmsg(db));
		goto RELEASE;
	}
	av_log(AV_LOG_INFO, "sqlite3_exec SQL_SELECT_TABLE_SEG ok\n");
    get_interval_end(0,"sqlite3_exec SQL_SELECT_TABLE_SEG end!");
#endif

RELEASE:
	sleep_ms(3000);
    sqlite3_free(errMsg);
    sqlite3_close(db);

    return 0;
}
