#pragma once
#include <limits>
#include "ItemTemplate.h"
#include "ModDynamicAH.h" // brings in ModDynamicAH::Family

namespace ModDynamicAH
{
    // -----------------------------------------------------------------------
    // Inputs / outputs
    // -----------------------------------------------------------------------
    struct PricingInputs
    {
        ItemTemplate const *tmpl = nullptr;
        uint32 scarcityCount = 0;      // live auctions for this entry
        uint32 onlinePlayers = 0;      // pop heuristic
        uint32 minPriceCopper = 10000; // safety floor
    };

    struct PricingResult
    {
        uint32 startBid = 0; // copper
        uint32 buyout = 0;   // copper
    };

    // -----------------------------------------------------------------------
    // Dynamic price policy
    // -----------------------------------------------------------------------
    class PricePolicy
    {
        // -------- Static economy knobs (set from ModuleState) --------
        static inline double s_avgGoldPerQuest = 10.0; // g.avgGoldPerQuest
        static inline uint32 s_questsPerFamily[(size_t)Family::COUNT] = {1};

    public:
        // Call this ONCE after config load:
        static void SetEconomy(double avgGoldPerQuest,
                               uint32 const *questsPerFamily /*[COUNT]*/)
        {
            s_avgGoldPerQuest = avgGoldPerQuest;
            for (size_t i = 0; i < (size_t)Family::COUNT; ++i)
                s_questsPerFamily[i] = questsPerFamily[i];
        }

        // Main entry -- now family-aware
        static PricingResult Compute(PricingInputs const &in, Family fam)
        {
            PricingResult r;
            if (!in.tmpl)
                return r;

            // -------- Vendor reference --------
            uint32 buyPrice = in.tmpl->BuyPrice;   // vendor sells to player
            uint32 sellPrice = in.tmpl->SellPrice; // vendor buys from player

            uint64 base = 0;
            if (buyPrice > 0)
                base = buyPrice + buyPrice / 5; // +20 %
            else if (sellPrice > 0)
                base = uint64(sellPrice) * 3; // ≈300 %
            else
                base = in.minPriceCopper;

            // -------- Scarcity scaling --------
            double scarcityBoost = 1.0;
            if (in.scarcityCount == 0)
                scarcityBoost = 1.25;
            else if (in.scarcityCount < 5)
                scarcityBoost = 1.15;
            else if (in.scarcityCount < 10)
                scarcityBoost = 1.08;
            else if (in.scarcityCount < 20)
                scarcityBoost = 1.03;

            // -------- Population scaling --------
            double popBoost = 1.0;
            if (in.onlinePlayers <= 50)
                popBoost = 0.95;
            else if (in.onlinePlayers <= 200)
                popBoost = 1.00;
            else if (in.onlinePlayers <= 500)
                popBoost = 1.07;
            else
                popBoost = 1.10;

            uint64 buyout64 = uint64(double(base) * scarcityBoost * popBoost);

            // -------- Economy floor (quests-per-family) --------
            double questGold = s_avgGoldPerQuest *
                               double(s_questsPerFamily[(size_t)fam]); // gold
            uint64 econFloorCopper = uint64(questGold * 10000.0);      // to copper

            if (buyout64 < econFloorCopper)
                buyout64 = econFloorCopper;

            // -------- Clamp high side relative to vendor value --------
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
} // namespace ModDynamicAH
