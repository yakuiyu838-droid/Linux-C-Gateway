#include "db.h"
#include <stdio.h>

sqlite3 *g_db = NULL;

int db_init(const char *db_name) 
{
    if (sqlite3_open(db_name, &g_db) != SQLITE_OK) return -1;
    const char *sql = "CREATE TABLE IF NOT EXISTS sensor_data ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "temp REAL NOT NULL, hum REAL NOT NULL, ts INTEGER NOT NULL);";
    return sqlite3_exec(g_db, sql, NULL, NULL, NULL);
}

int db_insert_sensor(double temp, double hum, int ts) 
{
        // 关键节点：prepare -> bind -> step -> finalize
    const char *sql = "INSERT INTO sensor_data (temp, hum) VALUES (?, ?);";
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);//准备阶段
    sqlite3_bind_double(stmt, 1, temp);            //绑定参数1
    sqlite3_bind_double(stmt, 2, hum);             //绑定参数2
    sqlite3_step(stmt);                            //执行
    sqlite3_finalize(stmt);                        //销毁回收内存

    return rc == SQLITE_DONE ? 0 : -1;
}

int db_get_latest(double *temp, double *hum, int *ts) 
{
    const char *sql = "SELECT temp, hum, ts FROM sensor_data ORDER BY id DESC LIMIT 1;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) 
    {
        *temp = sqlite3_column_double(stmt, 0); //从第 0 列提取 double
        *hum  = sqlite3_column_double(stmt, 1); 
        *ts   = sqlite3_column_int(stmt, 2);    
        sqlite3_finalize(stmt);
        return 0; 
    }
    sqlite3_finalize(stmt);
    return -1; 
}

void db_close() 
{
    if (g_db) sqlite3_close(g_db);
}