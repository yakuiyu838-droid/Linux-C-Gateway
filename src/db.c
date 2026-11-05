#include "db.h"
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

sqlite3 *g_db = NULL;

int db_init(const char *db_name)
{
    if (sqlite3_open(db_name, &g_db) != SQLITE_OK) return -1;

    // 关键：设 busy_timeout，否则并发读写会偶发 SQLITE_BUSY（database is locked）
    // 比如网关在写库时，另一个进程/线程同时查库，没这个就直接报错返回
    sqlite3_busy_timeout(g_db, 1000);

    // WAL 模式：允许「一边写一边读」。采集写库时，查询方不用等写锁
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

    const char *sql = "CREATE TABLE IF NOT EXISTS sensor_data ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "temp REAL NOT NULL, hum REAL NOT NULL, ts INTEGER NOT NULL);";
    return sqlite3_exec(g_db, sql, NULL, NULL, NULL);
}

int db_insert_sensor(double temp, double hum, int ts)
{
    const char *sql = "INSERT INTO sensor_data (temp, hum, ts) VALUES (?, ?, ?);";
    sqlite3_stmt *stmt;

    // 1. 准备语句（检查返回值，prepare 失败后 stmt 为 NULL，后续 bind/step 会崩）
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "db_insert_sensor: prepare 失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    // 2. 绑定参数
    sqlite3_bind_double(stmt, 1, temp);
    sqlite3_bind_double(stmt, 2, hum);
    sqlite3_bind_int(stmt, 3, ts);

    // 3. 执行并获取返回值
    int rc = sqlite3_step(stmt);

    // 4. 销毁语句对象
    sqlite3_finalize(stmt);

    // 5. 判断结果
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
        *temp = sqlite3_column_double(stmt, 0); // 从第 0 列提取 double
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

int db_get_history(int limit, char *out_json, int max_len)
{
    const char *sql = "SELECT temp, hum, ts FROM sensor_data ORDER BY id DESC LIMIT ?;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, limit);

    cJSON *array = cJSON_CreateArray(); // 创建一个 JSON 数组 []

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        cJSON *item = cJSON_CreateObject(); // 创建数组里的每个对象 {}
        cJSON_AddNumberToObject(item, "temp", sqlite3_column_double(stmt, 0));
        cJSON_AddNumberToObject(item, "hum", sqlite3_column_double(stmt, 1));
        cJSON_AddNumberToObject(item, "ts", sqlite3_column_int(stmt, 2));
        cJSON_AddItemToArray(array, item); // 塞进数组
    }
    sqlite3_finalize(stmt);

    // 转成字符串，拷贝给你
    char *json_str = cJSON_PrintUnformatted(array);
    strncpy(out_json, json_str, max_len - 1);
    out_json[max_len - 1] = '\0';   // 手动补 '\0'，防止 json 超长时 strncpy 不结尾

    cJSON_free(json_str);
    cJSON_Delete(array);
    return 0;
}
