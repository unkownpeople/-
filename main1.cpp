    // main.cpp
    #include "httplib.h"
    #include "json.hpp"
    #include <sqlite3.h>
    #include <iostream>
    #include <set>
    #include <ctime>
    #include <mutex>

    using json = nlohmann::json;
    using namespace httplib;

    std::set<std::string> online_users;
    std::mutex user_mutex;

    std::string current_time() {
        time_t now = time(0);
        char buf[80];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
        return std::string(buf);
    }

    sqlite3* init_db(const std::string& db_name) {
        sqlite3* db;
        if (sqlite3_open(db_name.c_str(), &db)) {
            std::cerr << "Cannot open database: " << sqlite3_errmsg(db) << std::endl;
            exit(1);
        }

        const char* create_users = "CREATE TABLE IF NOT EXISTS users (username TEXT PRIMARY KEY, password TEXT);";
        const char* create_msgs = "CREATE TABLE IF NOT EXISTS messages (id INTEGER PRIMARY KEY AUTOINCREMENT,sender TEXT,receiver TEXT,content TEXT,timestamp TEXT ); ";

        char* err;
        if (sqlite3_exec(db, create_users, nullptr, nullptr, &err) != SQLITE_OK) {
            std::cerr << "SQL error: " << err << std::endl;
            sqlite3_free(err);
        }
        if (sqlite3_exec(db, create_msgs, nullptr, nullptr, &err) != SQLITE_OK) {
            std::cerr << "SQL error: " << err << std::endl;
            sqlite3_free(err);
        }

        return db;
    }

    int main() {
        sqlite3* db = init_db("users.db"); // 修改为你的数据库文件名
        Server svr;

        svr.Post("/register", [&](const Request& req, Response& res) {
            auto j = json::parse(req.body);
            std::string username = j["username"], password = j["password"];

            sqlite3_stmt* stmt;
            std::string sql = "INSERT INTO users(username, password) VALUES (?, ?)";
            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, password.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_DONE) {
                    res.set_content("{\"status\": \"ok\"}", "application/json");
                }
                else {
                    res.status = 400;
                    res.set_content("{\"error\": \"User exists\"}", "application/json");
                }
            }
            sqlite3_finalize(stmt);
            });

        svr.Post("/login", [&](const Request& req, Response& res) {
            auto j = json::parse(req.body);
            std::string username = j["username"], password = j["password"];

            {
                std::lock_guard<std::mutex> lock(user_mutex);
                if (online_users.count(username)) {
                    res.status = 409;
                    res.set_content("{\"error\": \"User already logged in\"}", "application/json");
                    return;
                }
            }

            sqlite3_stmt* stmt;
            std::string sql = "SELECT * FROM users WHERE username=? AND password=?";
            bool valid = false;

            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, password.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW) valid = true;
            }
            sqlite3_finalize(stmt);

            if (valid) {
                std::lock_guard<std::mutex> lock(user_mutex);
                online_users.insert(username);
                res.set_content("{\"status\": \"ok\"}", "application/json");
            }
            else {
                res.status = 401;
                res.set_content("{\"error\": \"Invalid credentials\"}", "application/json");
            }
            });

        svr.Post("/logout", [&](const Request& req, Response& res) {
            auto j = json::parse(req.body);
            std::string username = j["username"];
            std::lock_guard<std::mutex> lock(user_mutex);
            online_users.erase(username);
            res.set_content("{\"status\": \"logged out\"}", "application/json");
            });

        svr.Post("/send_message", [&](const Request& req, Response& res) {
            auto j = json::parse(req.body);
            std::string sender = j["sender"];
            std::string receiver = j["receiver"];
            std::string content = j["content"];
            std::string timestamp = current_time();

            sqlite3_stmt* stmt;
            std::string sql = "INSERT INTO messages(sender, receiver, content, timestamp) VALUES (?, ?, ?, ?)";
            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, sender.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, receiver.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, content.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, timestamp.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
            }
            sqlite3_finalize(stmt);
            res.set_content("{\"status\": \"ok\"}", "application/json");
            });

        svr.Get("/messages", [&](const Request& req, Response& res) {
            auto user = req.get_param_value("user");
            auto peer = req.get_param_value("peer");

            std::string sql = "SELECT sender, receiver, content, timestamp FROM messages WHERE (sender = ? AND receiver = ?) OR (sender = ? AND receiver = ?) ORDER BY id DESC LIMIT 50";
            sqlite3_stmt* stmt;
            json out = json::array();

            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, user.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, peer.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, peer.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, user.c_str(), -1, SQLITE_TRANSIENT);

                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    json msg = {
                        {"sender", (const char*)sqlite3_column_text(stmt, 0)},
                        {"receiver", (const char*)sqlite3_column_text(stmt, 1)},
                        {"content", (const char*)sqlite3_column_text(stmt, 2)},
                        {"timestamp", (const char*)sqlite3_column_text(stmt, 3)}
                    };
                    out.push_back(msg);
                }
            }
            sqlite3_finalize(stmt);
            res.set_content(out.dump(), "application/json");
            });


        svr.Get(R"(/messages/user/(\w+))", [&](const Request& req, Response& res) {
            std::string username = req.matches[1];
            std::string sql = "SELECT sender, receiver, content, timestamp FROM messages WHERE sender = ? ORDER BY id DESC LIMIT 50";
            sqlite3_stmt* stmt;
            json out = json::array();

            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    json msg = {
                        {"sender", (const char*)sqlite3_column_text(stmt, 0)},
                        {"receiver", (const char*)sqlite3_column_text(stmt, 1)},
                        {"content", (const char*)sqlite3_column_text(stmt, 2)},
                        {"timestamp", (const char*)sqlite3_column_text(stmt, 3)}
                    };
                    out.push_back(msg);
                }
            }
            sqlite3_finalize(stmt);
            res.set_content(out.dump(), "application/json");
            });

        svr.Put("/update_user", [&](const Request& req, Response& res) {
            auto j = json::parse(req.body);
            std::string old_username = j["old_username"];
            std::string new_username = j["new_username"];
            std::string new_password = j["new_password"];

            sqlite3_stmt* stmt;
            std::string sql = "UPDATE users SET username = ?, password = ? WHERE username = ?";

            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, new_username.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, new_password.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, old_username.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_DONE) {
                    std::lock_guard<std::mutex> lock(user_mutex);
                    online_users.erase(old_username);
                    online_users.insert(new_username);
                    res.set_content("{\"status\": \"updated\"}", "application/json");
                }
                else {
                    res.status = 400;
                    res.set_content("{\"error\": \"Update failed\"}", "application/json");
                }
            }
            sqlite3_finalize(stmt);
            });

        /*svr.Get("/online_users", [&](const Request& req, Response& res) {
            json j = json::array();
            std::lock_guard<std::mutex> lock(user_mutex);
            for (const auto& user : online_users) j.push_back(user);
            res.set_content(j.dump(), "application/json");
            });
         */

        // 新增 unified online_users 接口
        svr.Get("/online_users", [&](const Request& req, Response& res) {
            json result;
            json users = json::array();
            std::lock_guard<std::mutex> lock(user_mutex);
            for (const auto& user : online_users) {
                users.push_back(user);
            }
            result["online_users"] = users;
            result["online_count"] = users.size();
            res.set_content(result.dump(), "application/json");
            });

        svr.Get("/users", [&](const Request& req, Response& res) {
            json out = json::array();
            std::lock_guard<std::mutex> lock(user_mutex);

            std::string sql = "SELECT username FROM users";
            sqlite3_stmt* stmt;

            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    std::string name = (const char*)sqlite3_column_text(stmt, 0);
                    bool is_online = online_users.count(name) > 0;
                    out.push_back({ {"username", name}, {"online", is_online} });
                }
            }
            sqlite3_finalize(stmt);

            res.set_content(out.dump(), "application/json");
            });
        /*svr.set_ws_handler("/ws", [&](const Request& req, Response& res, const ContentReader& reader) {
            res.status = 426;
            res.set_header("Connection", "Upgrade");
            res.set_header("Upgrade", "websocket");
            });
    */


        svr.listen("0.0.0.0", 8080);
        sqlite3_close(db);
        return 0;
    }
