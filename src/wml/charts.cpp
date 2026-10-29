// Charts (Insert > Chart): read the cached data Word shows, create bar,
// column, line, pie, doughnut, area and scatter charts from data, and
// replace a chart's data. Charts carry literal data (no embedded workbook):
// Word shows them as-is; Edit Data offers to make a workbook.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

#include "schema/schema.hpp"
#include "util/util.hpp"
#include "wml/document.hpp"
#include "wml/edit.hpp"
#include "wml/structure.hpp"
#include "wml/text.hpp"
#include "xml/xml.hpp"

namespace docx::wml {

namespace {

const char* kChartCt = "application/vnd.openxmlformats-officedocument.drawingml.chart+xml";

std::string esc(const std::string& s) { return util::xml_escape(util::xml_safe(s)); }

std::string num_text(double v) {
    if (std::fabs(v - std::round(v)) < 1e-9 && std::fabs(v) < 1e15) return std::to_string((long long)std::llround(v));
    char b[64];
    std::snprintf(b, sizeof b, "%.15g", v);
    return b;
}

// Values of a c:cat / c:val / c:tx: literal, or the cache of a reference.
std::vector<std::string> points(pugi::xml_node n) {
    std::vector<std::string> v;
    if (!n) return v;
    pugi::xml_node data;
    for (auto q : {"c:strLit", "c:numLit", "c:strRef/c:strCache", "c:numRef/c:numCache", "c:multiLvlStrRef/c:multiLvlStrCache"}) {
        auto x = n.select_node(q).node();
        if (x) { data = x; break; }
    }
    if (!data) {
        if (auto t = n.child("c:v")) v.push_back(t.text().get());
        return v;
    }
    int count = data.child("c:ptCount").attribute("val").as_int();
    if (xml::name_is(data, "c:multiLvlStrCache")) {
        // the innermost level is the category label
        data = data.child("c:lvl");
        count = std::max<int>(count, (int)xml::children(data, "c:pt").size());
    }
    v.assign(std::max(0, count), "");
    for (auto pt : data.children("c:pt")) {
        int i = pt.attribute("idx").as_int();
        if (i >= 0 && i < (int)v.size()) v[i] = pt.child("c:v").text().get();
        else if (i >= 0 && i < 100000) { v.resize(i + 1); v[i] = pt.child("c:v").text().get(); }
    }
    return v;
}

std::string rich_text(pugi::xml_node tx) {
    std::string s;
    for (auto t : xml::descendants(tx, "a:t")) s += t.text().get();
    if (s.empty()) for (auto v : xml::descendants(tx, "c:v")) s += v.text().get();
    return s;
}

const std::map<std::string, std::string>& chart_kinds() {
    static const std::map<std::string, std::string> m = {
        {"c:barChart", "bar"}, {"c:bar3DChart", "bar"}, {"c:lineChart", "line"}, {"c:line3DChart", "line"}, {"c:pieChart", "pie"},
        {"c:pie3DChart", "pie"}, {"c:doughnutChart", "doughnut"}, {"c:areaChart", "area"}, {"c:area3DChart", "area"},
        {"c:scatterChart", "scatter"}, {"c:radarChart", "radar"}, {"c:bubbleChart", "bubble"}, {"c:stockChart", "stock"},
        {"c:surfaceChart", "surface"}, {"c:surface3DChart", "surface"}, {"c:ofPieChart", "pie"}};
    return m;
}

struct ChartRef {
    pugi::xml_node frame;  // c:chart in the document
    std::string story, part;
};

std::vector<ChartRef> chart_refs(Document& d) {
    std::vector<ChartRef> v;
    for (auto& story : d.story_parts()) {
        if (!d.pkg().has(story)) continue;
        auto root = d.pkg().xml(story).document_element();
        for (auto c : xml::descendants(root, "c:chart")) {
            if (xml::ancestor(c, "mc:Fallback")) continue;
            std::string rid = xml::attr(c, "r:id");
            auto* rels = d.pkg().rels_if(story);
            const opc::Relationship* r = rels ? rels->by_id(rid) : nullptr;
            if (!r) continue;
            std::string part = opc::Package::resolve(story, r->target);
            if (!d.pkg().has(part)) continue;
            v.push_back({c, story, part});
        }
    }
    return v;
}

Json chart_json(Document& d, const ChartRef& ref, int index) {
    auto cs = d.pkg().xml(ref.part).document_element();
    auto chart = cs.child("c:chart");
    auto plot = chart.child("c:plotArea");
    Json j = {{"chart", index}};
    if (auto t = chart.child("c:title")) j["title"] = rich_text(t.child("c:tx"));
    // the drawing's size and alt text
    if (auto inl = xml::ancestor(ref.frame, "wp:inline") ? xml::ancestor(ref.frame, "wp:inline") : xml::ancestor(ref.frame, "wp:anchor")) {
        auto ex = inl.child("wp:extent");
        j["width"] = std::round(ex.attribute("cx").as_double() / 12700 * 10) / 10;
        j["height"] = std::round(ex.attribute("cy").as_double() / 12700 * 10) / 10;
        if (auto dp = inl.child("wp:docPr"); dp && !xml::attr(dp, "descr").empty()) j["alt_text"] = xml::attr(dp, "descr");
    }
    if (auto p = xml::ancestor(ref.frame, "w:p")) j["paragraph"] = para_id(p);
    Json plots = Json::array();
    for (auto pc : plot.children()) {
        auto it = chart_kinds().find(pc.name());
        if (it == chart_kinds().end()) continue;
        Json pj = {{"type", it->second}};
        if (it->second == "bar") pj["direction"] = xml::attr(pc.child("c:barDir"), "val", "col") == "bar" ? "horizontal" : "vertical";
        if (auto g = pc.child("c:grouping")) pj["grouping"] = xml::attr(g, "val");
        if (std::string(pc.name()).find("3D") != std::string::npos) pj["3d"] = true;
        Json series = Json::array();
        for (auto s : pc.children("c:ser")) {
            Json sj;
            auto name = points(s.child("c:tx"));
            sj["name"] = name.empty() ? "" : name[0];
            auto cats = points(s.child("c:cat"));
            if (cats.empty()) cats = points(s.child("c:xVal"));
            auto vals = points(s.child("c:val"));
            if (vals.empty()) vals = points(s.child("c:yVal"));
            Json jv = Json::array();
            for (auto& x : vals) {
                double dv = util::to_double(x, NAN);
                jv.push_back(std::isnan(dv) ? Json(nullptr) : Json(dv));
            }
            if (!cats.empty()) sj["categories"] = cats;
            sj["values"] = jv;
            series.push_back(sj);
        }
        pj["series"] = series;
        plots.push_back(pj);
    }
    j["plots"] = plots;
    if (plots.size() == 1) {
        j["type"] = plots[0]["type"];
        if (plots[0].contains("direction")) j["direction"] = plots[0]["direction"];
    }
    // axis titles
    for (auto ax : plot.children()) {
        std::string q = ax.name();
        if (q != "c:catAx" && q != "c:valAx" && q != "c:dateAx") continue;
        if (auto t = ax.child("c:title")) {
            std::string pos = xml::attr(ax.child("c:axPos"), "val");
            j[(pos == "l" || pos == "r") ? "y_axis_title" : "x_axis_title"] = rich_text(t.child("c:tx"));
        }
    }
    j["legend"] = (bool)chart.child("c:legend");
    return j;
}

// ── building ──

std::string title_xml(const std::string& t, int size_hundredths) {
    return "<c:title><c:tx><c:rich><a:bodyPr rot=\"0\" spcFirstLastPara=\"1\" vertOverflow=\"ellipsis\" vert=\"horz\" wrap=\"square\" anchor=\"ctr\" anchorCtr=\"1\"/>"
           "<a:lstStyle/><a:p><a:pPr><a:defRPr sz=\"" + std::to_string(size_hundredths) + "\" b=\"0\"/></a:pPr><a:r><a:rPr lang=\"en-US\"/><a:t>" + esc(t) +
           "</a:t></a:r></a:p></c:rich></c:tx><c:overlay val=\"0\"/></c:title>";
}

std::string str_lit(const std::vector<std::string>& v) {
    std::string s = "<c:strLit><c:ptCount val=\"" + std::to_string(v.size()) + "\"/>";
    for (std::size_t i = 0; i < v.size(); i++) s += "<c:pt idx=\"" + std::to_string(i) + "\"><c:v>" + esc(v[i]) + "</c:v></c:pt>";
    return s + "</c:strLit>";
}

std::string num_lit(const std::vector<std::optional<double>>& v) {
    std::string s = "<c:numLit><c:formatCode>General</c:formatCode><c:ptCount val=\"" + std::to_string(v.size()) + "\"/>";
    for (std::size_t i = 0; i < v.size(); i++)
        if (v[i]) s += "<c:pt idx=\"" + std::to_string(i) + "\"><c:v>" + num_text(*v[i]) + "</c:v></c:pt>";
    return s + "</c:numLit>";
}

struct Series {
    std::string name;
    std::vector<std::optional<double>> values;
    std::vector<std::optional<double>> x;  // scatter
    std::string color;
};

struct Spec {
    std::string type = "column";  // column, bar, line, pie, doughnut, area, scatter
    std::string grouping = "clustered";
    std::string title, x_title, y_title;
    std::vector<std::string> categories;
    std::vector<Series> series;
    bool legend = true, labels = false, markers = true, smooth = false;
    std::string legend_pos = "b";
};

std::string color_of(const std::string& in) {
    std::string c = in;
    if (!c.empty() && c[0] == '#') c = c.substr(1);
    static const std::map<std::string, std::string> names = {
        {"red", "FF0000"}, {"green", "00B050"}, {"blue", "0070C0"}, {"orange", "FFC000"}, {"yellow", "FFFF00"}, {"purple", "7030A0"},
        {"gray", "808080"}, {"grey", "808080"}, {"black", "000000"}, {"white", "FFFFFF"}, {"navy", "002060"}, {"teal", "008080"}};
    if (auto it = names.find(util::lower(c)); it != names.end()) return it->second;
    if (c.size() != 6 || !std::all_of(c.begin(), c.end(), [](char ch) { return std::isxdigit((unsigned char)ch); }))
        throw std::runtime_error("chart color '" + in + "' must be RRGGBB or a basic color name");
    for (auto& ch : c) ch = (char)std::toupper((unsigned char)ch);
    return c;
}

std::string sppr_fill(const std::string& color, bool line, int idx) {
    std::string fill = color.empty() ? "<a:schemeClr val=\"accent" + std::to_string(idx % 6 + 1) + "\"/>" : "<a:srgbClr val=\"" + color + "\"/>";
    if (line) return "<c:spPr><a:ln w=\"28575\" cap=\"rnd\"><a:solidFill>" + fill + "</a:solidFill><a:round/></a:ln></c:spPr>";
    return "<c:spPr><a:solidFill>" + fill + "</a:solidFill><a:ln><a:noFill/></a:ln></c:spPr>";
}

std::string axes_xml(const Spec& s, bool val_x) {
    std::string cat = val_x ? "<c:valAx><c:axId val=\"500000001\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling><c:delete val=\"0\"/><c:axPos val=\"b\"/>"
                            : "<c:catAx><c:axId val=\"500000001\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling><c:delete val=\"0\"/><c:axPos val=\"" +
                                  std::string(s.type == "bar" ? "l" : "b") + "\"/>";
    if (!s.x_title.empty()) cat += title_xml(s.x_title, 1000);
    if (val_x) cat += "<c:numFmt formatCode=\"General\" sourceLinked=\"1\"/>";
    cat += "<c:majorTickMark val=\"none\"/><c:minorTickMark val=\"none\"/><c:tickLblPos val=\"nextTo\"/><c:crossAx val=\"500000002\"/><c:crosses val=\"autoZero\"/>";
    cat += val_x ? "<c:crossBetween val=\"midCat\"/></c:valAx>" : "<c:auto val=\"1\"/><c:lblAlgn val=\"ctr\"/><c:lblOffset val=\"100\"/><c:noMultiLvlLbl val=\"0\"/></c:catAx>";
    std::string val = "<c:valAx><c:axId val=\"500000002\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling><c:delete val=\"0\"/><c:axPos val=\"" +
                      std::string(s.type == "bar" ? "b" : "l") + "\"/><c:majorGridlines><c:spPr><a:ln w=\"9525\"><a:solidFill><a:schemeClr val=\"tx1\"><a:lumMod val=\"15000\"/>"
                      "<a:lumOff val=\"85000\"/></a:schemeClr></a:solidFill></a:ln></c:spPr></c:majorGridlines>";
    if (!s.y_title.empty()) val += title_xml(s.y_title, 1000);
    val += std::string("<c:numFmt formatCode=\"") + (s.grouping == "percentStacked" ? "0%" : "General") +
           "\" sourceLinked=\"1\"/><c:majorTickMark val=\"none\"/><c:minorTickMark val=\"none\"/><c:tickLblPos val=\"nextTo\"/>"
           "<c:crossAx val=\"500000001\"/><c:crosses val=\"autoZero\"/><c:crossBetween val=\"" + std::string(s.type == "area" || s.type == "column" || s.type == "bar" ? "between" : "midCat") +
           "\"/></c:valAx>";
    return cat + val;
}

std::string chart_xml(const Spec& s) {
    std::string plot;
    bool pie = s.type == "pie" || s.type == "doughnut";
    std::string dl = s.labels ? "<c:dLbls><c:showLegendKey val=\"0\"/><c:showVal val=\"" + std::string(pie ? "0" : "1") +
                                     "\"/><c:showCatName val=\"0\"/><c:showSerName val=\"0\"/><c:showPercent val=\"" + (pie ? "1" : "0") +
                                     "\"/><c:showBubbleSize val=\"0\"/></c:dLbls>"
                              : "";
    auto ser = [&](std::size_t i) {
        const auto& se = s.series[i];
        std::string x = "<c:ser><c:idx val=\"" + std::to_string(i) + "\"/><c:order val=\"" + std::to_string(i) + "\"/>";
        x += "<c:tx><c:v>" + esc(se.name.empty() ? "Series " + std::to_string(i + 1) : se.name) + "</c:v></c:tx>";
        if (pie) {
            // one color per slice
            x += "<c:spPr><a:ln w=\"19050\"><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill></a:ln></c:spPr>";
            for (std::size_t k = 0; k < se.values.size(); k++)
                x += "<c:dPt><c:idx val=\"" + std::to_string(k) + "\"/><c:bubble3D val=\"0\"/>" + sppr_fill("", false, (int)k) + "</c:dPt>";
        } else {
            x += sppr_fill(se.color, s.type == "line" || s.type == "scatter", (int)i);
            if (s.type == "bar" || s.type == "column") x += "<c:invertIfNegative val=\"0\"/>";
            if (s.type == "line" || s.type == "scatter")
                x += s.markers ? "<c:marker><c:symbol val=\"circle\"/><c:size val=\"5\"/></c:marker>" : "<c:marker><c:symbol val=\"none\"/></c:marker>";
        }
        x += dl;
        if (s.type == "scatter") {
            std::vector<std::optional<double>> xs = se.x;
            if (xs.empty()) for (std::size_t k = 0; k < se.values.size(); k++) xs.push_back((double)(k + 1));
            x += "<c:xVal>" + num_lit(xs) + "</c:xVal><c:yVal>" + num_lit(se.values) + "</c:yVal>";
            x += "<c:smooth val=\"" + std::string(s.smooth ? "1" : "0") + "\"/>";
        } else {
            if (!s.categories.empty()) x += "<c:cat>" + str_lit(s.categories) + "</c:cat>";
            x += "<c:val>" + num_lit(se.values) + "</c:val>";
            if (s.type == "line") x += "<c:smooth val=\"" + std::string(s.smooth ? "1" : "0") + "\"/>";
        }
        return x + "</c:ser>";
    };
    std::string sers;
    for (std::size_t i = 0; i < s.series.size(); i++) sers += ser(i);
    std::string axids = "<c:axId val=\"500000001\"/><c:axId val=\"500000002\"/>";
    if (s.type == "column" || s.type == "bar") {
        plot = "<c:barChart><c:barDir val=\"" + std::string(s.type == "bar" ? "bar" : "col") + "\"/><c:grouping val=\"" + s.grouping +
               "\"/><c:varyColors val=\"0\"/>" + sers + "<c:gapWidth val=\"" + (s.grouping == "clustered" ? "219" : "150") + "\"/>" +
               (s.grouping != "clustered" ? "<c:overlap val=\"100\"/>" : "<c:overlap val=\"-27\"/>") + axids + "</c:barChart>";
    } else if (s.type == "line") {
        plot = "<c:lineChart><c:grouping val=\"" + std::string(s.grouping == "clustered" ? "standard" : s.grouping) + "\"/><c:varyColors val=\"0\"/>" +
               sers + "<c:marker val=\"1\"/>" + axids + "</c:lineChart>";
    } else if (s.type == "area") {
        plot = "<c:areaChart><c:grouping val=\"" + std::string(s.grouping == "clustered" ? "standard" : s.grouping) + "\"/><c:varyColors val=\"0\"/>" +
               sers + axids + "</c:areaChart>";
    } else if (s.type == "pie") {
        plot = "<c:pieChart><c:varyColors val=\"1\"/>" + sers + "<c:firstSliceAng val=\"0\"/></c:pieChart>";
    } else if (s.type == "doughnut") {
        plot = "<c:doughnutChart><c:varyColors val=\"1\"/>" + sers + "<c:firstSliceAng val=\"0\"/><c:holeSize val=\"55\"/></c:doughnutChart>";
    } else if (s.type == "scatter") {
        plot = "<c:scatterChart><c:scatterStyle val=\"" + std::string(s.smooth ? "smoothMarker" : "lineMarker") + "\"/><c:varyColors val=\"0\"/>" + sers + axids +
               "</c:scatterChart>";
    }
    std::string axes = pie ? "" : axes_xml(s, s.type == "scatter");
    std::string legend = s.legend ? "<c:legend><c:legendPos val=\"" + s.legend_pos + "\"/><c:overlay val=\"0\"/></c:legend>" : "";
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
           "<c:chartSpace xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\" "
           "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
           "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
           "<c:date1904 val=\"0\"/><c:lang val=\"en-US\"/><c:roundedCorners val=\"0\"/><c:chart>" +
           (s.title.empty() ? std::string("<c:autoTitleDeleted val=\"1\"/>") : title_xml(s.title, 1400) + "<c:autoTitleDeleted val=\"0\"/>") +
           "<c:plotArea><c:layout/>" + plot + axes + "</c:plotArea>" + legend +
           "<c:plotVisOnly val=\"1\"/><c:dispBlanksAs val=\"gap\"/></c:chart>"
           "<c:spPr><a:noFill/><a:ln><a:noFill/></a:ln></c:spPr>"
           "<c:txPr><a:bodyPr/><a:lstStyle/><a:p><a:pPr><a:defRPr/></a:pPr><a:endParaRPr lang=\"en-US\"/></a:p></c:txPr></c:chartSpace>";
}

std::optional<double> as_num(const Json& v) {
    if (v.is_number()) return v.get<double>();
    if (v.is_null()) return std::nullopt;
    if (v.is_string()) {
        std::string s = util::trim(v.get<std::string>());
        s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == ',' || c == '$' || c == '%'; }), s.end());
        if (s.empty()) return std::nullopt;
        char* end = nullptr;
        double d = std::strtod(s.c_str(), &end);
        if (end == s.c_str() + s.size()) return d;
    }
    throw std::runtime_error("chart values must be numbers (got " + v.dump() + ")");
}

Spec spec_from(const Json& j) {
    Spec s;
    std::string t = util::lower(j.value("type", std::string("column")));
    static const std::map<std::string, std::string> alias = {{"columns", "column"}, {"bars", "bar"}, {"horizontal_bar", "bar"}, {"lines", "line"},
                                                             {"donut", "doughnut"}, {"xy", "scatter"}, {"vertical_bar", "column"}};
    if (auto it = alias.find(t); it != alias.end()) t = it->second;
    static const std::set<std::string> types = {"column", "bar", "line", "pie", "doughnut", "area", "scatter"};
    if (!types.count(t)) throw std::runtime_error(util::unknown_key("chart type", t, {"column", "bar", "line", "pie", "doughnut", "area", "scatter"}));
    s.type = t;
    std::string g = util::lower(j.value("grouping", std::string("clustered")));
    if (g == "stacked_percent" || g == "percent" || g == "100%") g = "percentStacked";
    if (g != "clustered" && g != "stacked" && g != "percentStacked" && g != "standard")
        throw std::runtime_error("grouping is clustered, stacked or percent");
    if (g == "standard") g = "clustered";
    s.grouping = (t == "pie" || t == "doughnut" || t == "scatter") ? "clustered" : g;
    s.title = j.value("title", std::string());
    s.x_title = j.value("x_axis_title", std::string());
    s.y_title = j.value("y_axis_title", std::string());
    s.legend = j.value("legend", true);
    if (j.contains("legend_position")) {
        static const std::map<std::string, std::string> pos = {{"bottom", "b"}, {"top", "t"}, {"left", "l"}, {"right", "r"}};
        auto it = pos.find(j["legend_position"].get<std::string>());
        if (it == pos.end()) throw std::runtime_error("legend_position is bottom, top, left or right");
        s.legend_pos = it->second;
    }
    s.labels = j.value("data_labels", false);
    s.markers = j.value("markers", true);
    s.smooth = j.value("smooth", false);
    if (j.contains("categories")) for (auto& c : j["categories"]) s.categories.push_back(c.is_string() ? c.get<std::string>() : c.dump());
    if (!j.contains("series") || !j["series"].is_array() || j["series"].empty())
        throw std::runtime_error("series is a list like [{\"name\": \"Sales\", \"values\": [1, 2, 3]}]");
    for (auto& sj : j["series"]) {
        Series se;
        if (sj.is_array()) {
            for (auto& v : sj) se.values.push_back(as_num(v));
        } else {
            se.name = sj.value("name", std::string());
            if (!sj.contains("values")) throw std::runtime_error("each series needs values");
            for (auto& v : sj["values"]) se.values.push_back(as_num(v));
            if (sj.contains("x")) for (auto& v : sj["x"]) se.x.push_back(as_num(v));
            if (sj.contains("color")) se.color = color_of(sj["color"].get<std::string>());
        }
        if (se.values.empty()) throw std::runtime_error("a series has no values");
        if (!se.x.empty() && se.x.size() != se.values.size()) throw std::runtime_error("x and values differ in length");
        s.series.push_back(std::move(se));
    }
    std::size_t n = 0;
    for (auto& se : s.series) n = std::max(n, se.values.size());
    if (s.categories.empty() && t != "scatter")
        for (std::size_t i = 0; i < n; i++) s.categories.push_back(std::to_string(i + 1));
    if (!s.categories.empty() && s.categories.size() < n) s.categories.resize(n);
    if ((t == "pie" || t == "doughnut") && s.series.size() > 1 && t == "pie")
        throw std::runtime_error("a pie chart shows one series (use doughnut for several, or column)");
    return s;
}

}  // namespace

Json list_charts(Document& d) {
    Json v = Json::array();
    int i = 1;
    for (auto& r : chart_refs(d)) v.push_back(chart_json(d, r, i++));
    return v;
}

pugi::xml_node insert_chart(Document& d, pugi::xml_node p, int pos, const Json& spec_in, double wpt, double hpt, const EditOpts& o) {
    Spec s = spec_from(spec_in.is_object() ? spec_in : Json::object());
    std::string part = d.pkg().unique_name("/word/charts/chart", ".xml");
    d.pkg().add_xml_part(part, kChartCt, chart_xml(s));
    std::string story = d.main();
    std::string rid = d.pkg().rels(story).add(opc::rt::chart, opc::Package::relative(story, part));
    if (wpt <= 0) wpt = 432;
    if (hpt <= 0) hpt = wpt * 0.6;
    long cx = std::lround(wpt * 12700), cy = std::lround(hpt * 12700);
    int id = 1;
    for (auto& sp : d.story_parts())
        if (d.pkg().has(sp))
            for (auto n : d.pkg().xml(sp).select_nodes("//wp:docPr/@id")) id = std::max(id, n.attribute().as_int() + 1);
    std::string alt = spec_in.value("alt_text", s.title);
    std::string frag = "<w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\"><wp:extent cx=\"" + std::to_string(cx) +
                       "\" cy=\"" + std::to_string(cy) + "\"/><wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/><wp:docPr id=\"" + std::to_string(id) +
                       "\" name=\"Chart " + std::to_string(id) + "\"" + (alt.empty() ? "" : " descr=\"" + esc(alt) + "\"") +
                       "/><wp:cNvGraphicFramePr/><a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/chart\">"
                       "<c:chart r:id=\"" + rid + "\"/></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>";
    pugi::xml_document holder;
    std::string err;
    if (!xml::parse_fragment(frag, holder, &err)) throw std::runtime_error("chart: " + err);
    xml::declare(xml::root_of(p), "c");
    xml::declare(xml::root_of(p), "wp");
    xml::declare(xml::root_of(p), "a");
    // place the run at pos (split as needed)
    split_at(p, pos);
    ParaText pt = analyze(p);
    int n = (int)pt.vis.size();
    pugi::xml_node parent = p, before;
    if (pos < n) {
        auto node = pt.atoms[pt.vis[pos]].run;
        while (is_revision_container(node.parent())) node = node.parent();
        parent = node.parent();
        before = node;
    } else if (n > 0) {
        auto node = pt.atoms[pt.vis[n - 1]].run;
        while (is_revision_container(node.parent())) node = node.parent();
        parent = node.parent();
        before = node.next_sibling();
    }
    pugi::xml_node r = xml::copy_into(parent, holder.first_child(), before);
    bool track = o.track ? *o.track : d.track_changes();
    if (track) {
        auto ins = parent.insert_child_before("w:ins", r);
        stamp(d, ins);
        ins.append_move(r);
    }
    return r;
}

void set_chart_data(Document& d, int index, const Json& spec_in) {
    auto refs = chart_refs(d);
    if (index < 1 || index > (int)refs.size())
        throw std::runtime_error("no chart " + std::to_string(index) + " (the document has " + std::to_string(refs.size()) + ")");
    auto& ref = refs[index - 1];
    // start from what the chart shows now, then apply the changes
    Json cur = chart_json(d, ref, index);
    Json j = spec_in.is_object() ? spec_in : Json::object();
    if (!j.contains("type")) {
        std::string t = cur.value("type", std::string("column"));
        if (t == "bar" && cur.value("direction", std::string()) == "vertical") t = "column";
        static const std::set<std::string> ok = {"column", "bar", "line", "pie", "doughnut", "area", "scatter"};
        if (!ok.count(t))
            throw std::runtime_error("chart " + std::to_string(index) + " is a " + t + " chart, which set_chart_data can't rebuild; "
                                     "give type (column, bar, line, pie, doughnut, area, scatter) to replace it");
        j["type"] = t;
    }
    if (!j.contains("title") && cur.contains("title")) j["title"] = cur["title"];
    if (!j.contains("series")) {
        Json series = Json::array();
        if (!cur["plots"].empty())
            for (auto& s : cur["plots"][0]["series"]) series.push_back({{"name", s["name"]}, {"values", s["values"]}});
        j["series"] = series;
    }
    if (!j.contains("categories") && !cur["plots"].empty() && !cur["plots"][0]["series"].empty() &&
        cur["plots"][0]["series"][0].contains("categories"))
        j["categories"] = cur["plots"][0]["series"][0]["categories"];
    for (auto k : {"x_axis_title", "y_axis_title"}) if (!j.contains(k) && cur.contains(k)) j[k] = cur[k];
    Spec s = spec_from(j);
    // a fresh chart part body; the embedded workbook (if any) no longer matches, so drop the link
    auto& doc = d.pkg().xml(ref.part);
    doc.reset();
    auto x = chart_xml(s);
    if (!doc.load_buffer(x.data(), x.size(), pugi::parse_default | pugi::parse_declaration))
        throw std::runtime_error("chart XML failed to build");
    if (auto* rels = d.pkg().rels_if(ref.part)) {
        std::vector<std::string> drop;
        for (auto& r : rels->items)
            if (r.type.find("package") != std::string::npos || r.type.find("oleObject") != std::string::npos) drop.push_back(r.id);
        for (auto& id : drop) d.pkg().rels(ref.part).remove(id);
    }
}

}  // namespace docx::wml
