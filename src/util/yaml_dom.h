// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <yaml.h>

namespace budyk {

// The two libyaml DOM lookups every YAML reader in budyk needs (the
// config loader, the simple-YAML rules transpiler).

// The value node for `key` in a mapping node, or nullptr when `map` is
// not a mapping or has no such key.
const yaml_node_t* yaml_find_key(yaml_document_t* doc, const yaml_node_t* map,
                                 const char* key);

// A scalar node's text, or nullptr for a missing or non-scalar node.
const char* yaml_scalar(const yaml_node_t* n);

} // namespace budyk
