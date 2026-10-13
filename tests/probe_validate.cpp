// Scratch probe: open a docx, validate main parts with our schema runtime.
#include <cstdio>

#include "opc/package.hpp"
#include "schema/schema.hpp"
#include "xml/xml.hpp"

int main(int argc, char** argv) {
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        try {
            auto pkg = docx::opc::Package::open(argv[i]);
            for (auto& name : pkg.part_names()) {
                auto ctype = pkg.content_type_of(name);
                if (docx::schema::part_root_type(ctype) < 0) continue;
                auto& d = pkg.xml(name);
                docx::xml::normalize(d);
                auto root = d.document_element();
                if (docx::schema::root_type(root.name()) < 0) continue;
                auto issues = docx::schema::validate(root);
                for (auto& is : issues) {
                    std::printf("%s %s %s :: %s\n", argv[i], name.c_str(), is.path.c_str(), is.message.c_str());
                    bad++;
                }
            }
        } catch (std::exception& e) {
            std::printf("%s: %s\n", argv[i], e.what());
            bad++;
        }
    }
    std::printf("issues: %d\n", bad);
    return bad ? 1 : 0;
}
