#include "db.h"
#include <stdio.h>

sqlite3 *g_db = NULL;

int db_init(const char *db_name) 
{
    if (sqlite3_open(db_name, &g_db) != SQLITE_OK) 
    {
        return -1;
    }
    const char *sql = "CREATE TABLE IF NOT EXISTS sensor_data ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "temp REAL NOT NULL, hum REAL NOT NULL, ts INTEGER NOT NULL);";
    return sqlite3_exec(g_db, sql, NULL, NULL, NULL);
}

int db_insert_sensor(double temp, double hum, int ts) 
{
    const char *sql = "INSERT INTO sensor_data (temp, hum, ts) VALUES (?, ?, ?);";
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    sqlite3_bind_double(stmt, 1, temp);
    sqlite3_bind_double(stmt, 2, hum);
    sqlite3_bind_int(stmt, 3, ts);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

void db_close() 
{
    if (g_db) sqlite3_close(g_db);
}