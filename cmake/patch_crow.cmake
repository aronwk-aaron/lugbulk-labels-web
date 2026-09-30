# Applied to Crow's sources by FetchContent (PATCH_COMMAND). Crow buffers a
# request body of any size in memory, so one client could exhaust the
# server's RAM with a single huge POST. This caps it at CROW_MAX_BODY_SIZE
# (64 KB: every request body is a few bytes of JSON; files are read in the
# browser): a body over the limit makes the parser fail and the connection
# is dropped. Idempotent, and upgrades a tree patched by the earlier
# version that also allowed 10 MB uploads under /upload/.
set(parser "${CROW_SOURCE_DIR}/include/crow/parser.h")
file(READ "${parser}" src)
set(original "HTTPParser* self = static_cast<HTTPParser*>(self_);\n            self->req.body.insert(self->req.body.end(), at, at + length);")
set(new "HTTPParser* self = static_cast<HTTPParser*>(self_);\n            if (self->req.body.size() + length > CROW_MAX_BODY_SIZE) return 1; // lugbulk: body size cap\n            self->req.body.insert(self->req.body.end(), at, at + length);")
set(with_uploads "HTTPParser* self = static_cast<HTTPParser*>(self_);\n            // lugbulk: body size cap\n            const size_t lugbulk_limit = self->req.url.rfind(\"/upload/\", 0) == 0 ? CROW_MAX_UPLOAD_SIZE : CROW_MAX_BODY_SIZE;\n            if (self->req.body.size() + length > lugbulk_limit) return 1;\n            self->req.body.insert(self->req.body.end(), at, at + length);")
string(FIND "${src}" "${with_uploads}" found_with_uploads)
string(FIND "${src}" "${original}" found_original)
string(FIND "${src}" "${new}" found_new)
if(NOT found_with_uploads EQUAL -1)
    string(REPLACE "${with_uploads}" "${new}" src "${src}")
    string(REPLACE "#ifndef CROW_MAX_UPLOAD_SIZE\n#define CROW_MAX_UPLOAD_SIZE (10 * 1024 * 1024)\n#endif\n" "" src "${src}")
    file(WRITE "${parser}" "${src}")
elseif(NOT found_original EQUAL -1)
    string(REPLACE "${original}" "${new}" src "${src}")
    string(FIND "${src}" "#define CROW_MAX_BODY_SIZE" has_guard)
    if(has_guard EQUAL -1)
        string(REPLACE "#pragma once\n" "#pragma once\n#ifndef CROW_MAX_BODY_SIZE\n#define CROW_MAX_BODY_SIZE (64 * 1024)\n#endif\n" src "${src}")
    endif()
    file(WRITE "${parser}" "${src}")
elseif(found_new EQUAL -1)
    message(FATAL_ERROR "patch_crow.cmake: Crow's on_body changed; update the patch")
endif()
