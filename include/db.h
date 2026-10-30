#ifndef DB_H
#define DB_H

#include "sqlite3.h"

// 声明全局数据库句柄，让别的文件也能用
extern sqlite3 *g_db;

// 初始化数据库并建表
int db_init(const char *db_name);
// 插入一条数据
int db_insert_sensor(double temp, double hum, int ts);
// 查询最新的一条数据
int db_get_latest(double *temp, double *hum, int *ts);
// 关闭数据库
void db_close();

#endif