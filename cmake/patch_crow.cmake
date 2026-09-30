# Applied to Crow's sources by FetchContent (PATCH_COMMAND). Crow buffers a
# request body of any size in memory, so one client could exhaust the
# server's RAM with a single huge POST. This caps it: a body over
# CROW_MAX_BODY_SIZE bytes (compile definition) makes the parser fail and
# the connection is dropped. Idempotent — safe to run on an already
# patched tree.
set(parser "${CROW_SOURCE_DIR}/include/crow/parser.h")
file(READ "${parser}" src)
string(FIND "${src}" "CROW_MAX_BODY_SIZE" already)
if(already EQUAL -1)
    set(old "HTTPParser* self = static_cast<HTTPParser*>(self_);\n            self->req.body.insert(self->req.body.end(), at, at + length);")
    set(new "HTTPParser* self = static_cast<HTTPParser*>(self_);\n            if (self->req.body.size() + length > CROW_MAX_BODY_SIZE) return 1; // lugbulk: body size cap\n            self->req.body.insert(self->req.body.end(), at, at + length);")
    string(FIND "${src}" "${old}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "patch_crow.cmake: Crow's on_body changed; update the patch")
    endif()
    string(REPLACE "${old}" "${new}" src "${src}")
    set(guard "#ifndef CROW_MAX_BODY_SIZE\n#define CROW_MAX_BODY_SIZE (64 * 1024)\n#endif\n")
    string(REPLACE "#pragma once\n" "#pragma once\n${guard}" src "${src}")
    file(WRITE "${parser}" "${src}")
endif()
