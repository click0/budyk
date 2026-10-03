// SPDX-License-Identifier: BSD-3-Clause
#include "util/tmpfile.h"
#include "util/yaml_dom.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

using namespace budyk;

int main() {
    // 1. write_private_tmp: the file exists with the body, is 0600, is
    //    named after the prefix, and lives in $TMPDIR when that is set.
    {
        char tmpl[] = "/tmp/budyk_util_XXXXXX";
        const char* dir = ::mkdtemp(tmpl);
        assert(dir != nullptr);
        ::setenv("TMPDIR", dir, 1);

        char path[256];
        assert(write_private_tmp("budyk_test_", "hello\nworld", path, sizeof(path)));
        assert(std::strncmp(path, dir, std::strlen(dir)) == 0);
        assert(std::strstr(path, "/budyk_test_") != nullptr);
        struct stat st{};
        assert(::stat(path, &st) == 0);
        assert((st.st_mode & 0777) == 0600);
        assert(st.st_size == 11);
        FILE* f = std::fopen(path, "r");
        char buf[32] = {0};
        assert(std::fread(buf, 1, sizeof(buf) - 1, f) == 11);
        std::fclose(f);
        assert(std::string(buf) == "hello\nworld");
        ::unlink(path);

        // An empty body is a valid, empty file.
        assert(write_private_tmp("budyk_test_", "", path, sizeof(path)));
        assert(::stat(path, &st) == 0 && st.st_size == 0);
        ::unlink(path);

        // Too small a buffer for the path, or an unwritable directory:
        // false, nothing left behind.
        char tiny[8];
        assert(!write_private_tmp("budyk_test_", "x", tiny, sizeof(tiny)));
        ::setenv("TMPDIR", "/nonexistent/budyk", 1);
        assert(!write_private_tmp("budyk_test_", "x", path, sizeof(path)));

        ::unsetenv("TMPDIR");
        ::rmdir(dir);
        // Without TMPDIR the file goes to /tmp.
        assert(write_private_tmp("budyk_test_", "x", path, sizeof(path)));
        assert(std::strncmp(path, "/tmp/budyk_test_", 16) == 0);
        ::unlink(path);
    }

    // 2. yaml_find_key / yaml_scalar on a small document: nested keys,
    //    a missing key, a non-scalar value, a non-mapping node.
    {
        const char* text =
            "web:\n"
            "  auth:\n"
            "    enabled: true\n"
            "  list: [1, 2]\n"
            "port: 8080\n";
        yaml_parser_t parser;
        yaml_document_t doc;
        assert(yaml_parser_initialize(&parser) == 1);
        yaml_parser_set_input_string(&parser, reinterpret_cast<const unsigned char*>(text),
                                     std::strlen(text));
        assert(yaml_parser_load(&parser, &doc) == 1);
        const yaml_node_t* root = yaml_document_get_root_node(&doc);
        assert(root != nullptr);

        assert(std::string(yaml_scalar(yaml_find_key(&doc, root, "port"))) == "8080");
        const yaml_node_t* web = yaml_find_key(&doc, root, "web");
        assert(web != nullptr && yaml_scalar(web) == nullptr);          // a mapping, not a scalar
        const yaml_node_t* auth = yaml_find_key(&doc, web, "auth");
        assert(std::string(yaml_scalar(yaml_find_key(&doc, auth, "enabled"))) == "true");
        assert(yaml_find_key(&doc, root, "nope") == nullptr);
        assert(yaml_find_key(&doc, nullptr, "x") == nullptr);
        const yaml_node_t* list = yaml_find_key(&doc, web, "list");
        assert(list != nullptr && yaml_scalar(list) == nullptr);
        assert(yaml_find_key(&doc, list, "x") == nullptr);              // not a mapping
        assert(yaml_scalar(nullptr) == nullptr);

        yaml_document_delete(&doc);
        yaml_parser_delete(&parser);
    }

    std::printf("test_util: PASS\n");
    return 0;
}
