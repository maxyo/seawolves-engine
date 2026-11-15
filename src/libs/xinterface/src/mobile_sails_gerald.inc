// Included inside xinterface.cpp's Android implementation: the price, money and applied emblem
// of the sail emblem screen (PROGRAM/interface/SailsGerald.c) for mobile_sails_gerald.hpp.
// The emblem rows themselves come from SCROLL_GERALD (CXI_SCROLLIMAGE::MobileCollect).
void XINTERFACE::MobileCollectSailsGerald(std::vector<MobileWidget> &widgets)
{
    const auto value=MobileVariableInteger("price");
    const std::optional<int> price=value?std::optional<int>(*value):std::nullopt;
    auto *hero=MobileVariableObject("pchar");
    const char *money=hero?hero->GetAttribute("Money"):nullptr;
    auto *ship=MobileVariableObject("shref");
    std::optional<std::string> applied;
    if(ship){auto *a=ship->FindAClass(ship,"ShipSails.Gerald_Name");const char *s=a?a->GetThisAttr():nullptr;applied=std::string(s?s:"");}
    widgets.push_back(storm::mobile::SailsGeraldMarker(price,money?std::optional<std::string>(money):std::nullopt,applied));
}
