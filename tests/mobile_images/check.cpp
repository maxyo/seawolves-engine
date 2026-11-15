#include "mobile_image_cache.hpp"
#include "mobile_interface_model.hpp"
#include "mobile_image_bridge.hpp"
#include "mobile_variable_reads.hpp"
#include <map>
#include <cassert>
#include <iostream>
using namespace storm::mobile;
int main()
{
    // Stable handle per picture, screen invalidation, UV changes, a bounded registry.
    ImageCache cache;
    cache.screen(4);
    auto a = cache.reference({"INTERFACES\\test.tga", {}});
    auto same = cache.reference({"resource/textures/interfaces/test.tga", {}});
    assert(a.handle && a.handle == same.handle);
    // Optional semantic identity belongs to each reference, not the bitmap cache.
    assert(MobileModelImage(a).kind.empty());
    auto atlas = a;
    atlas.source = {"atlas", "ICONS_SPEC", "FencingL skill icon", {}, {}, -1};
    const auto atlasModel = MobileModelImage(atlas);
    assert(atlasModel.kind == "atlas" && atlasModel.group == "ICONS_SPEC" && atlasModel.image == "FencingL skill icon" &&
           atlasModel.handle == static_cast<int64_t>(a.handle));
    // A source missing what its kind needs is no source.
    auto incomplete = atlas;
    incomplete.source.group.clear();
    assert(MobileModelImage(incomplete).kind.empty() && MobileModelImage(incomplete).image.empty());
    assert(atlas.handle == same.handle && same.source.kind.empty());
    // A picture the engine names by file: its texture-namespace name and the rectangle shown.
    assert(MobileTextureName("Interfaces\\Maps\\Kayman.tga") == "interfaces/maps/kayman");
    assert(MobileTextureName("resource/textures/loading/inside/kiss.tga.tx") == "loading/inside/kiss");
    assert(MobileTextureName("../secret.tga").empty() && MobileTextureName("C:/x.tga").empty());
    auto texture = a;
    texture.source = {"texture", {}, "interfaces/maps/kayman", {}, {}, -1, {0.f, 0.25f, 0.5f, 1.f}};
    const auto textureModel = MobileModelImage(texture);
    assert(textureModel.kind == "texture" && textureModel.image == "interfaces/maps/kayman" && textureModel.uv[1] == 0.25f &&
           textureModel.uv[2] == 0.5f && textureModel.uv[3] == 1.f);
    assert(MobileModelImage(atlas).uv[2] == 1.f); // the rectangle belongs to texture sources only
    auto battle = WithBattleIconSource(a, "land", "LandTarget_SLB", 16);
    assert(MobileModelImage(battle).atlas == "LandTarget_SLB" && MobileModelImage(battle).pic == 16 && MobileModelImage(battle).mode == "land");
    assert(WithBattleIconSource({}, "land", "LandTarget_SLB", 16).source.kind.empty());
    assert(WithBattleIconSource(a, "land", "LandTarget_SLB", -1).source.kind.empty());
    auto changed = cache.reference({"interfaces/test.tga", {0, 0, .5f, 1}});
    assert(a.handle != changed.handle);
    assert(!cache.reference({"../outside", {}}).handle);
    assert(!cache.reference({"/absolute", {}}).handle);
    cache.screen(5);
    assert(cache.reference({"interfaces/test.tga", {}}).handle != a.handle);
    for (int i = 0; i < 4100; ++i)
        cache.reference({"interfaces/" + std::to_string(i), {}});
    assert(cache.descriptors() == 4096);
    // The model of collected widgets: reading order is the position, type-specific fields only for
    // their type, cells and pictures carried over, broken text repaired and reported.
    MobileWidget widget{"TABLE", "choice", "old label", "1", 2, 5, true};
    widget.rowId = "Hull";
    widget.cells.push_back({0, "red", 0xff123456, {atlas}});
    widget.actions = {"select", "activate"};
    widget.min = 3, widget.max = 9, widget.step = 2, widget.exclusive = true; // not a slider, not a check
    MobileWidget slider{"SLIDE", "slider", "Volume", "4", -1, 9, true};
    slider.min = 0, slider.max = 10, slider.step = 1;
    MobileWidget check{"CHECK", "check", "Bad \xff text", "1", -1, 12, true};
    check.exclusive = true;
    check.context["note"] = "ok";
    check.headerCells = std::vector<MobileCell>{{1, "head\xc3", 0xff000000, {}}};
    std::vector<std::string> repaired;
    const auto model = MobileInterfaceModel("ship", "english", 4, 7, {widget, slider, check}, &repaired);
    assert(model.screen == "ship" && model.language == "english" && model.opening == 4 && model.revision == 7 && model.widgets.size() == 3);
    const auto &row = model.widgets[0];
    assert(row.node == "TABLE" && row.row_id == "Hull" && row.label == "old label" && row.index == 2 && row.order == 0 && row.enabled);
    assert(row.min == 0 && row.max == 0 && row.step == 1 && !row.exclusive);
    assert(row.cells.size() == 1 && row.cells[0].color_argb == 0xff123456 && row.cells[0].images.at(0).group == "ICONS_SPEC");
    assert(row.actions == (std::vector<std::string>{"select", "activate"}));
    assert(model.widgets[1].order == 1 && model.widgets[1].max == 10 && model.widgets[1].step == 1);
    assert(model.widgets[2].exclusive && model.widgets[2].context.at("note") == "ok");
    assert(model.widgets[2].label == "Bad \xEF\xBF\xBD text" && model.widgets[2].header_cells->at(0).text == "head\xEF\xBF\xBD");
    assert(repaired == std::vector<std::string>{"CHECK"});
    // Equal collections make equal models (what decides a new revision).
    assert(MobileInterfaceModel("ship", "english", 4, 7, {widget, slider, check}) == model);
    auto moved = slider;
    moved.value = "5";
    assert(!(MobileInterfaceModel("ship", "english", 4, 7, {widget, moved, check}) == model) && !(moved == slider));
    // The script variables a collection read are its inputs: a read outside a collection is nobody's,
    // a variable that reads differently now asks for a new collection.
    VariableReads reads;
    using Kind = VariableReads::Kind;
    std::map<std::string, std::optional<std::string>> vars{{"TradeMode", "1"}, {"sMessageMode", "sink"}};
    const auto read = [&vars](const char *name, Kind) { return vars.count(name) ? vars[name] : std::nullopt; };
    reads.Note("TradeMode", Kind::Integer, vars["TradeMode"]);
    reads.Begin();
    reads.Note("sMessageMode", Kind::Text, vars["sMessageMode"]);
    reads.Note("missing", Kind::Object, std::nullopt);
    reads.Note("sMessageMode", Kind::Text, vars["sMessageMode"]);
    reads.End();
    assert(reads.Count() == 2 && !reads.Changed(read));
    vars["TradeMode"] = "0"; // not read by the collection
    assert(!reads.Changed(read));
    vars["sMessageMode"] = "free";
    assert(reads.Changed(read));
    vars["sMessageMode"] = "sink";
    vars["missing"] = "0x1"; // a reference that points somewhere now
    assert(reads.Changed(read));
    std::cout
        << "PASS mobile images: picture handles, screen lifetimes, bounds, sources and the interface model\n";
}
