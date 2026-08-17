// lugbulk-labels-web — hosted counterpart to the lugbulk-label CLI.
//
// Scaffolding stage: proves the build (Crow + SQLite + libharu + CURL +
// OpenSSL) links and serves a request. Routes below will grow into:
//   GET  /                    dashboard (list of saved sheets, if logged in)
//   GET  /auth/login          kick off Google OAuth
//   GET  /auth/callback       OAuth redirect target, stores refresh token
//   POST /sheets              add a sheet by URL/ID
//   POST /sheets/:id/labels   generate labels.pdf for a sheet, synchronous
//   POST /sheets/:id/lots     generate lot_counts.csv/pdf for a sheet
//   GET  /sheets/:id/history  run log for a sheet
//
// See sql/schema.sql for the users/sheets/runs tables.

#include "crow.h"

int main() {
    crow::SimpleApp app;

    CROW_ROUTE(app, "/healthz")([]() {
        return crow::response(200, "ok");
    });

    CROW_ROUTE(app, "/")([]() {
        return "lugbulk-labels-web: scaffolding stage, nothing built yet.";
    });

    app.port(8080).multithreaded().run();
}
