// SPDX-License-Identifier: BSD-3-Clause
#include "util/yaml_dom.h"

#include <cstring>

namespace budyk {

const char* yaml_scalar(const yaml_node_t* n) {
    if (n == nullptr || n->type != YAML_SCALAR_NODE) return nullptr;
    return reinterpret_cast<const char*>(n->data.scalar.value);
}

const yaml_node_t* yaml_find_key(yaml_document_t* doc, const yaml_node_t* map,
                                 const char* key) {
    if (map == nullptr || map->type != YAML_MAPPING_NODE) return nullptr;
    for (auto* pair = map->data.mapping.pairs.start;
         pair     != map->data.mapping.pairs.top; ++pair) {
        const char* ks = yaml_scalar(yaml_document_get_node(doc, pair->key));
        if (ks != nullptr && std::strcmp(ks, key) == 0) {
            return yaml_document_get_node(doc, pair->value);
        }
    }
    return nullptr;
}

} // namespace budyk
