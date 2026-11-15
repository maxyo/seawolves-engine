#include "platform/android_spyglass.hpp"
#include <cassert>
#include <string>

int main()
{
    using namespace storm::android;
    assert(spyglassCaptainLine("24 фунт.       Мигель де Сандоваль") == std::make_pair(std::string("24 фунт."), std::string("Мигель де Сандоваль")));
    assert(spyglassCaptainLine("Кулеврины  Мушкетный залп   Ян ван Дейк").second == "Ян ван Дейк");
    assert(spyglassCaptainLine("       Хуан").first.empty() && spyglassCaptainLine("       Хуан").second == "Хуан");
    assert(spyglassCaptainLine("   ").second.empty());

    SpyglassState s;
    assert(s.json() == "{\"schemaVersion\":1,\"revision\":0,\"on\":false,\"generation\":0,\"target\":null}");
    assert(!s.acknowledge("0\t1")); // nothing to cover while the telescope is down
    s.turn(true);
    assert(s.on() && s.generation() == 1 && s.json().find("\"target\":null") != std::string::npos);
    SpyglassTarget t;
    t.name = "Санта \"Каталина\""; t.type = "Галеон"; t.shipClass = 2; t.hull = 64; t.sails = 80; t.cannons = 32; t.cannonsMax = 36;
    t.charge = 1; t.nation = 2; t.speed = 11.04; t.distance = 420.0; t.captain = "Мигель"; t.skills[0] = 7; t.skills[1] = 8;
    s.target(t);
    const std::string json = s.json();
    assert(json.find("\"kind\":\"ship\"") != std::string::npos);
    assert(json.find("\"name\":\"Санта \\\"Каталина\\\"\"") != std::string::npos);
    assert(json.find("\"speed\":11.0") != std::string::npos && json.find("\"crew\"") == std::string::npos); // hidden: left out
    assert(json.find("\"skills\":{\"fencing\":7,\"cannons\":8}") != std::string::npos);
    const std::string before = s.json();
    s.target(t);
    assert(s.json() == before); // the same target is no new revision

    // Coverage: the UI's word for the current raise, kept for the raise (no lease), gone when the
    // telescope drops.
    assert(!s.acknowledge("0\t1") && !s.acknowledge("x\t1") && !s.acknowledge("1") && !s.acknowledge("1\t2") && !s.covered());
    assert(s.acknowledge("1\t1") && s.covered() && !s.awaiting());
    assert(s.acknowledge("1\t0") && !s.covered()); // the overlay went away: diagnostics must report a missing card
    s.turn(false);
    assert(!s.covered() && s.json().find("\"on\":false") != std::string::npos);
    s.turn(true);
    assert(s.generation() == 3 && !s.covered() && s.awaiting() && !s.acknowledge("1\t1"));
    // Runtime failure leaves the native overlay disabled; a late touch HUD answer works.
    assert(hudPresenters.declare("spyglass") && !s.covered());
    assert(!hudPresenters.lost().empty() && !s.covered());
    assert(hudPresenters.declare("spyglass"));
    assert(s.acknowledge("3\t1") && s.covered());
    assert(s.acknowledge("3\t0") && !s.covered() && !s.awaiting());
    s.clear();
    t.fort = true;
    s.target(t);
    assert(s.json().find("\"kind\":\"fort\"") != std::string::npos);
    return 0;
}
