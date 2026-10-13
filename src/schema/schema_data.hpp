// Raw tables generated from the Open XML SDK schema data.
#pragma once
#include <cstddef>
#include <cstdint>

namespace docx::schema::data {

struct RawType {
    std::uint32_t key;        // "w:CT_PPr/w:pPr"
    std::uint32_t qname;      // "w:pPr" (0 for abstract bases)
    std::int32_t  base;       // abstract base type, or -1
    std::int32_t  particle;   // root particle, or -1
    std::uint32_t attr_begin, attr_end;
    std::uint32_t val_begin, val_end;  // validators on the text content
    std::uint8_t  flags;      // 1 leaf, 2 leaf text, 4 abstract, 8 derived
    std::uint8_t  composite;  // 1 sequence, 2 choice, 3 all
    std::uint8_t  version;
    std::uint32_t class_name;
    std::uint32_t summary;
};

struct RawParticle {
    std::uint8_t  kind;  // 0 element 1 sequence 2 choice 3 group 4 all 5 any
    std::uint32_t min, max;  // max 0 = unbounded
    std::int32_t  type;  // element type for kind 0
    std::uint32_t child_begin, child_end;
    std::uint8_t  any_ns;  // 0 ##any 1 ##other 2 ##local 3 ##targetNamespace
};

struct RawAttr {
    std::uint32_t qname;
    std::uint8_t  kind;       // ValueKind
    std::uint8_t  item_kind;  // for lists
    std::int32_t  enum_index;
    std::uint32_t val_begin, val_end;
    std::uint8_t  version;
};

struct RawValidator {
    std::uint8_t  kind;   // 0 required 1 string 2 number 3 enum
    std::int8_t   union_id;
    std::uint8_t  flags;
    std::int32_t  enum_index;
    std::uint8_t  num_kind;  // 0 attr's own, 1 signed int, 2 non-negative int, 3 float
    std::uint8_t  pad;
    std::int64_t  min, max;
    std::int32_t  min_len, max_len, length;
    std::uint32_t pattern;
};

struct RawEnum {
    std::uint32_t name, st;
    std::uint32_t facet_begin, facet_end;
};

struct RawPart {
    std::uint32_t name, content_type, rel_type;
    std::int32_t  root;
    std::uint32_t target, path, extension;
    std::uint8_t  version;
};

struct RawNamespace {
    std::uint32_t prefix, uri;
    std::uint8_t  version;
};

extern const char* const strings[];
extern const std::size_t n_strings;
extern const RawType types[];
extern const std::size_t n_types;
extern const RawParticle particles[];
extern const std::size_t n_particles;
extern const std::int32_t particle_children[];
extern const RawAttr attrs[];
extern const std::size_t n_attrs;
extern const RawValidator validators[];
extern const std::size_t n_validators;
extern const RawEnum enums[];
extern const std::size_t n_enums;
extern const std::uint32_t enum_facets[];
extern const RawPart parts[];
extern const std::size_t n_parts;
extern const RawNamespace namespaces[];
extern const std::size_t n_namespaces;

}  // namespace docx::schema::data
