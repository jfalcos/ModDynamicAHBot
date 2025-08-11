#include "DynamicAHPlanner.h"
#include "DynamicAHSelection.h"
#include "ObjectMgr.h"
#include "GameTime.h"
#include "World.h"
#include "Player.h"
#include <algorithm>
#include <vector>
#include <unordered_set>
#include "DynamicAHRecipes.h"

namespace ModDynamicAH
{

    static std::unordered_set<uint32> gEss, gShr, gEle, gRare;
    static bool gCatInit = false;

    std::unordered_set<uint32> &DynamicAHPlanner::EssenceSet() { return gEss; }
    std::unordered_set<uint32> &DynamicAHPlanner::ShardSet() { return gShr; }
    std::unordered_set<uint32> &DynamicAHPlanner::ElementalSet() { return gEle; }
    std::unordered_set<uint32> &DynamicAHPlanner::RareRawSet() { return gRare; }

    void DynamicAHPlanner::InitCategorySetsOnce()
    {
        if (gCatInit)
            return;
        auto addList = [](auto const &tbl, std::unordered_set<uint32> &tgt)
        {
            for (auto const &br : tbl)
                for (uint32 id : br.items)
                    tgt.insert(id);
        };
        addList(ENCH_ESSENCE, EssenceSet());
        addList(ENCH_SHARDS, ShardSet());
        addList(ELEMENTALS, ElementalSet());
        addList(RARE_RAW, RareRawSet());
        gCatInit = true;
    }

    double DynamicAHPlanner::CategoryMul(PlannerConfig const &cfg, uint32 itemId)
    {
        if (EssenceSet().count(itemId))
            return cfg.mulEssence;
        if (ShardSet().count(itemId))
            return cfg.mulShard;
        if (ElementalSet().count(itemId))
            return cfg.mulElemental;
        if (RareRawSet().count(itemId))
            return cfg.mulRareRaw;
        return 1.0;
    }

    void DynamicAHPlanner::ResetTick(uint32 onlineCount)
    {
        _queue.Clear();
        _perTickPlanCap.clear();
        _scarcity.Clear();
        _scarcity.Rebuild();
        _online = onlineCount;
        InitCategorySetsOnce();
    }

    static inline uint8 HouseIndex(AuctionHouseId h)
    {
        switch (h)
        {
        case AuctionHouseId::Alliance:
            return 0;
        case AuctionHouseId::Horde:
            return 1;
        default:
            return 2;
        }
    }

    double DynamicAHPlanner::Jitter(uint32 itemId)
    {
        uint32 t = static_cast<uint32>(GameTime::GetGameTime().count());
        uint32 seed = t ^ (itemId * 2654435761u);
        int delta = int(seed % 11) - 5; // -5..+5
        return 1.0 + double(delta) / 100.0;
    }

    uint32 DynamicAHPlanner::ClampToStackable(ItemTemplate const *tmpl, uint32 desired)
    {
        uint32 maxStack = (tmpl && tmpl->Stackable > 0) ? tmpl->Stackable : 1u;
        return std::min(desired ? desired : 1u, maxStack);
    }

    uint32 DynamicAHPlanner::StacksForSkill(uint16 s, PlannerConfig const &cfg)
    {
        if (s < 150)
            return cfg.stacksLow;
        if (s < 300)
            return cfg.stacksMid;
        return cfg.stacksHigh;
    }

    uint32 DynamicAHPlanner::ScarcityCount(uint32 itemId, AuctionHouseId house) const
    {
        return _scarcity.Count(itemId, house);
    }

    bool DynamicAHPlanner::TryPlanOnce(AuctionHouseId house, uint32 itemId)
    {
        uint64 key = (uint64(uint32(house)) << 32) | itemId;
        uint32 &cnt = _perTickPlanCap[key];
        ++cnt;
        return true;
    }

    void DynamicAHPlanner::PriceWithPolicies(PlannerConfig const &cfg, Family fam, uint32 itemId,
                                             ItemTemplate const *tmpl, AuctionHouseId house,
                                             uint32 &outStart, uint32 &outBuy) const
    {
        uint32 active = cfg.scarcityEnabled ? ScarcityCount(itemId, house) : 0;

        PricingInputs in;
        in.tmpl = tmpl;
        in.activeInHouse = active;
        in.onlineCount = _scarcity.OnlineCount();
        in.minPriceCopper = cfg.minPriceCopper; // may be overridden for stackable mats below

        // ---- Recipe-driven bounds (per STACK) ----
        ModDynamicAH::RecipeUsageIndex::Instance().EnsureBuilt();
        uint16 req = ModDynamicAH::RecipeUsageIndex::Instance().EffectiveSkillForReagent(itemId);
        uint16 maxReq = ModDynamicAH::RecipeUsageIndex::Instance().MaxSkillForReagent(itemId);
        if (req > 450)
            req = 450;
        if (maxReq > 450)
            maxReq = 450;

        auto lerp = [](double a, double b, double t)
        { return a + (b - a) * std::clamp(t, 0.0, 1.0); };

        auto stackBaseFloorG = [req, lerp]() -> double
        {
            // Floors per stack tuned for low tiers:
            // 0..75: 0.4..0.8g, 75..150: 0.8..1.6g, 150..225: 1.6..4g,
            // 225..300: 4..12g, 300..375: 12..30g, 375..450: 30..60g
            if (req <= 75)
                return lerp(0.4, 0.8, double(req) / 75.0);
            if (req <= 150)
                return lerp(0.8, 1.6, double(req - 75) / 75.0);
            if (req <= 225)
                return lerp(1.6, 4.0, double(req - 150) / 75.0);
            if (req <= 300)
                return lerp(4.0, 12.0, double(req - 225) / 75.0);
            if (req <= 375)
                return lerp(12.0, 30.0, double(req - 300) / 75.0);
            return lerp(30.0, 60.0, double(req - 375) / 75.0);
        };

        // modest premium if many high-tier recipes also use it
        double spread = std::max(0, int(maxReq) - int(req));        // 0..450
        double boost = std::clamp(spread / 200.0, 0.0, 1.0) * 0.20; // up to +20%

        uint32 stackSize = std::max<uint32>(1u, tmpl->Stackable);
        bool isStackableMat =
            (stackSize > 1) && (fam != Family::Other); // treat mats (cloth/ore/herb/etc.) specially

        uint32 recipeUnitFloor = 0;
        uint32 recipeUnitCeil = 0;

        if (req > 0)
        {
            double floorG = stackBaseFloorG() * (1.0 + boost);
            // ceilings: allow more headroom at higher tiers
            double spanMul =
                (req <= 150) ? 2.0 : (req <= 300) ? 2.5
                                                  : 3.0;
            double ceilG = floorG * spanMul;

            recipeUnitFloor = uint32(std::lround(floorG * 10000.0 / double(stackSize)));
            recipeUnitCeil = uint32(std::lround(ceilG * 10000.0 / double(stackSize)));

            // For stackable mats, use recipe floor as the effective min so low tiers stay cheap.
            if (isStackableMat)
                in.minPriceCopper = std::max<uint32>(recipeUnitFloor, 100u); // at least 1s
            else
                in.minPriceCopper = std::max<uint32>(in.minPriceCopper, recipeUnitFloor);
        }

        // ---- Base unit price from engine ----
        PricingResult base = DynamicAHPricing::Compute(in); // unit-level
        uint32 unitStart = base.startBid;
        uint32 unitBuy = std::max<uint32>(base.buyout, unitStart + 1);

        // ---- Scarcity/category/jitter (unit) ----
        auto mulRound = [](uint32 v, double f) -> uint32
        { return uint32(std::lround(double(v) * f)); };
        double scarcityBoost = cfg.scarcityEnabled ? (1.0 + cfg.scarcityPriceBoostMax / double(1 + active)) : 1.0;
        double catMul = CategoryMul(cfg, itemId);
        double jitter = Jitter(itemId);

        unitStart = mulRound(unitStart, scarcityBoost * catMul * jitter);
        unitBuy = mulRound(unitBuy, scarcityBoost * catMul * jitter);
        if (unitBuy <= unitStart)
            unitBuy = unitStart + 1;

        // ---- Friendly rounding (unit): >=1g → 5s steps, else 1s ----
        auto roundTo = [](uint32 coppers, uint32 stepC) -> uint32
        {
            uint32 r = coppers % stepC;
            uint32 down = coppers - r;
            uint32 up = down + stepC;
            return (coppers - down < up - coppers) ? down : up;
        };
        uint32 step = (unitBuy >= 10000u) ? 500u : 100u;
        unitBuy = roundTo(unitBuy, step);
        unitStart = std::min<uint32>((unitBuy > 0 ? unitBuy - 1 : 0), roundTo(unitStart, step));

        // ---- Vendor/min floors & markup (unit) ----
        DynamicAHVendor::ApplyVendorFloor(tmpl, unitStart, unitBuy, cfg.minPriceCopper, cfg.vendorMinMarkup);

        // ---- Clamp to recipe-based ceiling for mats (prevents low tiers from ballooning) ----
        if (isStackableMat && recipeUnitCeil > 0)
        {
            if (unitBuy > recipeUnitCeil)
                unitBuy = recipeUnitCeil;
            if (unitStart >= unitBuy)
                unitStart = (unitBuy > 0 ? unitBuy - 1 : 0);
        }

        outStart = unitStart;
        outBuy = unitBuy;
    }

    void DynamicAHPlanner::BuildRandomPlan(PlannerConfig const &cfg)
    {
        if (!cfg.enableSeller)
            return;

        SelectionConfig sel;
        sel.blockTrashAndCommon = cfg.blockTrashAndCommon;
        for (int i = 0; i < 6; ++i)
            sel.allowQuality[i] = cfg.allowQuality[i];
        sel.whitelist = cfg.whitelist;
        sel.maxRandomPostsPerCycle = cfg.maxRandomPerCycle;
        sel.minPriceCopper = cfg.minPriceCopper;

        auto candidates = DynamicAHSelection::PickRandomSellables(sel, cfg.maxRandomPerCycle);
        for (auto const &c : candidates)
        {
            ItemTemplate const *tmpl = c.tmpl;
            if (!tmpl)
                continue;

            // simple house distribution
            uint8 which = static_cast<uint8>(c.itemId % 3);
            AuctionHouseId house = (which == 0) ? AuctionHouseId::Alliance : (which == 1) ? AuctionHouseId::Horde
                                                                                          : AuctionHouseId::Neutral;

            if (!TryPlanOnce(house, c.itemId))
                continue;

            uint32 startBid = 0, buyout = 0;
            PriceWithPolicies(cfg, Family::Other, c.itemId, tmpl, house, startBid, buyout);

            uint32 count = ClampToStackable(tmpl, cfg.stDefault);
            _queue.Push(PostRequest{house, c.itemId, count, startBid, buyout, 24 * HOUR});
        }
    }

    // ---- Context planner (short, but uses your ProfessionMats.h tables) ----
    // Helpers to pick items for a skill bracket:
    template <size_t N>
    static MatBracket const *FindBracket(uint16 s, std::array<MatBracket, N> const &tab)
    {
        for (MatBracket const &b : tab)
            if (s >= b.minSkill && s < b.maxSkill)
                return &b;
        return nullptr;
    }

    template <size_t N>
    static uint32 PickForSkill(uint16 s, std::array<MatBracket, N> const &tab)
    {
        if (MatBracket const *b = FindBracket(s, tab))
        {
            size_t idx = (GameTime::GetGameTime().count() / 60) % b->items.size();
            return *(b->items.begin() + idx);
        }
        return 0;
    }

    static bool Enqueue(Player *plr, PlannerConfig const &cfg, DynamicAHPlanner *self,
                        Family fam, uint32 itemId, uint32 desiredStack, uint32 stacksToPost)
    {
        if (!itemId || !plr)
            return false;

        ItemTemplate const *tmpl = sObjectMgr->GetItemTemplate(itemId);
        if (!tmpl)
            return false;

        AuctionHouseId house = (plr->GetTeamId() == TEAM_ALLIANCE) ? AuctionHouseId::Alliance
                                                                   : AuctionHouseId::Horde;

        uint32 unitStart = 0, unitBuy = 0;
        self->PriceWithPolicies(cfg, fam, itemId, tmpl, house, unitStart, unitBuy);

        uint32 count = DynamicAHPlanner::ClampToStackable(tmpl, desiredStack);
        if (count == 0)
            count = 1;

        uint64 sb = uint64(unitStart) * count;
        uint64 bo = uint64(unitBuy) * count;
        if (bo <= sb)
            bo = sb + 1;

        uint32 stackStart = sb > UINT32_MAX ? UINT32_MAX : uint32(sb);
        uint32 stackBuy = bo > UINT32_MAX ? UINT32_MAX : uint32(bo);

        char const *houseTag = (house == AuctionHouseId::Alliance ? "A" : house == AuctionHouseId::Horde ? "H"
                                                                                                         : "N");
        LOG_INFO("mod.dynamicah",
                 "plan: item={} '{}' house={} stack={} unitStart={}c unitBuy={}c stackStart={}c stackBuy={}c",
                 itemId, tmpl->Name1, houseTag, count, unitStart, unitBuy, stackStart, stackBuy);

        for (uint32 i = 0; i < stacksToPost; ++i)
        {
            if (!self->TryPlanOnce(house, itemId))
                break;
            self->Queue().Push(PostRequest{house, itemId, count, stackStart, stackBuy, 24 * HOUR});
        }
        return true;
    }

    // Enqueue for a specific auction house without needing a Player*
    static bool EnqueueHouse(AuctionHouseId house, PlannerConfig const &cfg, DynamicAHPlanner *self,
                             Family fam, uint32 itemId, uint32 desiredStack, uint32 stacksToPost)
    {
        if (!itemId)
            return false;

        ItemTemplate const *tmpl = sObjectMgr->GetItemTemplate(itemId);
        if (!tmpl)
            return false;

        uint32 unitStart = 0, unitBuy = 0;
        self->PriceWithPolicies(cfg, fam, itemId, tmpl, house, unitStart, unitBuy);

        uint32 count = DynamicAHPlanner::ClampToStackable(tmpl, desiredStack);
        if (count == 0)
            count = 1;

        uint64 sb = uint64(unitStart) * count;
        uint64 bo = uint64(unitBuy) * count;
        if (bo <= sb)
            bo = sb + 1;

        uint32 stackStart = sb > UINT32_MAX ? UINT32_MAX : uint32(sb);
        uint32 stackBuy = bo > UINT32_MAX ? UINT32_MAX : uint32(bo);

        char const *houseTag = (house == AuctionHouseId::Alliance ? "A" : house == AuctionHouseId::Horde ? "H"
                                                                                                         : "N");
        LOG_INFO("mod.dynamicah",
                 "plan: item={} '{}' house={} stack={} unitStart={}c unitBuy={}c stackStart={}c stackBuy={}c",
                 itemId, tmpl->Name1, houseTag, count, unitStart, unitBuy, stackStart, stackBuy);

        for (uint32 i = 0; i < stacksToPost; ++i)
        {
            if (!self->TryPlanOnce(house, itemId))
                break;
            self->Queue().Push(PostRequest{house, itemId, count, stackStart, stackBuy, 24 * HOUR});
        }
        return true;
    }

    void DynamicAHPlanner::BuildContextPlan(PlannerConfig const &cfg)
    {
        if (!cfg.contextEnabled)
            return;

        // Global, once-per-cycle: enqueue every material from all tables exactly once per faction house.
        std::vector<std::pair<Family, uint32>> allMats;
        std::unordered_set<uint32> seen;

        auto addAll = [&](Family fam, auto const &tab)
        {
            for (auto const &b : tab)
                for (uint32 id : b.items)
                    if (seen.insert(id).second)
                        allMats.emplace_back(fam, id);
        };

        // Sweep all known material families
        addAll(Family::Cloth, TAILORING_CLOTH);
        addAll(Family::Herb, HERBS);
        addAll(Family::Ore, MINING_ORE);
        addAll(Family::Bar, BS_BARS);
        addAll(Family::Dust, ENCH_DUSTS);
        addAll(Family::Essence, ENCH_ESSENCE);
        addAll(Family::Shard, ENCH_SHARDS);
        addAll(Family::Leather, LEATHERS);
        addAll(Family::Stone, MINING_STONE);
        addAll(Family::Meat, COOKING_MEAT);
        addAll(Family::Fish, FISHING_RAW);
        addAll(Family::Jewelcrafting, JEWELCRAFT_GEMS)

            auto stackSizeFor = [&](Family fam) -> uint32
        {
            switch (fam)
            {
            case Family::Cloth:
                return cfg.stCloth;
            case Family::Herb:
                return cfg.stHerb;
            case Family::Ore:
                return cfg.stOre;
            case Family::Bar:
                return cfg.stBar;
            case Family::Dust:
                return cfg.stDust;
            case Family::Essence:
                return cfg.stDust; // keep parity with previous behavior
            case Family::Shard:
                return 1; // shards single
            case Family::Leather:
                return cfg.stLeather;
            case Family::Stone:
                return cfg.stStone;
            case Family::Meat:
                return cfg.stMeat;
            case Family::Fish:
                return cfg.stFish;
            default:
                return cfg.stDefault;
            }
        };

        const AuctionHouseId houses[2] = {AuctionHouseId::Alliance, AuctionHouseId::Horde};
        const uint32 stacksToPost = cfg.stacksMid;

        for (auto const &[fam, itemId] : allMats)
        {
            uint32 desiredStack = stackSizeFor(fam);
            for (AuctionHouseId h : houses)
                EnqueueHouse(h, cfg, this, fam, itemId, desiredStack, stacksToPost);
        }
    }

    void DynamicAHPlanner::BuildScarcityCache(ModuleState const & /*s*/)
    {
        _scarcity.Clear();
        _scarcity.Rebuild();
    }
}