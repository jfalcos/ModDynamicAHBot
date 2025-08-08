#pragma once
#include "ItemTemplate.h"
#include <limits>

namespace ModDynamicAH
{
    enum class Family : uint8_t; // forward-declared so we can pass it by value

    struct PricingInputs
    {
        ItemTemplate const *tmpl = nullptr;
        uint32 scarcityCount = 0;      // number of active auctions for this item
        uint32 onlinePlayers = 0;      // mild scalar
        uint32 minPriceCopper = 10000; // fallback
    };

    struct PricingResult
    {
        uint32 startBid = 0; // copper
        uint32 buyout = 0;   // copper
    };

    class PricePolicy
    {
    public:
        static PricingResult Compute(PricingInputs const &in, Family fam)
        {
            (void)fam; // economy scaling coming soon

            PricingResult r;
            if (!in.tmpl)
                return r;

            uint32 buyPrice = in.tmpl->BuyPrice;   // vendor sells to player
            uint32 sellPrice = in.tmpl->SellPrice; // vendor buys from player

            uint64 base = 0;
            if (buyPrice > 0)
                base = buyPrice + buyPrice / 5; // +20%
            else if (sellPrice > 0)
                base = uint64(sellPrice) * 3; // ~300%
            else
                base = in.minPriceCopper;

            double scarcityBoost = 1.0;
            if (in.scarcityCount == 0)
                scarcityBoost = 1.25;
            else if (in.scarcityCount < 5)
                scarcityBoost = 1.15;
            else if (in.scarcityCount < 10)
                scarcityBoost = 1.08;
            else if (in.scarcityCount < 20)
                scarcityBoost = 1.03;

            double popBoost = 1.0;
            if (in.onlinePlayers <= 50)
                popBoost = 0.95;
            else if (in.onlinePlayers <= 200)
                popBoost = 1.0;
            else if (in.onlinePlayers <= 500)
                popBoost = 1.07;
            else
                popBoost = 1.1;

            uint64 buyout64 = uint64(double(base) * scarcityBoost * popBoost);

            uint64 highClamp = 0;
            if (buyPrice > 0)
                highClamp = uint64(buyPrice) * 20;
            else if (sellPrice > 0)
                highClamp = uint64(sellPrice) * 5;
            else
                highClamp = uint64(in.minPriceCopper) * 10;

            if (buyout64 > highClamp)
                buyout64 = highClamp;
            if (buyout64 < in.minPriceCopper)
                buyout64 = in.minPriceCopper;

            r.buyout = uint32(std::min<uint64>(buyout64, std::numeric_limits<uint32>::max()));
            r.startBid = std::max<uint32>(in.minPriceCopper, r.buyout * 6 / 10);
            return r;
        }
    };
}
