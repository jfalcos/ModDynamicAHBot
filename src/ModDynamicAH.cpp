#include "AuctionHouseMgr.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "ObjectGuid.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldSessionMgr.h"
#include "AccountMgr.h"
#include "WorldSession.h"
#include "WorldSocket.h"
#include "SharedDefines.h"
#include "Optional.h"

#include <memory>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <array>
#include <algorithm>
#include <sstream>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "ModDynamicAH.h"
#include "Pricing.h"
#include "Selection.h"
#include "ModDynamicAHBuy.h" // buy engine
#include "ProfessionMats.h"  // new comprehensive mat tables

using namespace ModDynamicAH;

// one buy engine instance for this TU
static ModDynamicAH::BuyEngine g_buy;

namespace
{
    using ModDynamicAH::Family; // use the public enum everywhere in this TU

    static void Chatf(ChatHandler *handler, char const *fmt, ...)
    {
        if (!handler)
            return;
        char buffer[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, ap);
        va_end(ap);
        handler->SendSysMessage(buffer);
    }

    // ---------------- internal helpers / types ----------------
    static inline uint8 HouseIdx(ModDynamicAH::House h)
    {
        switch (h)
        {
        case ModDynamicAH::House::Alliance:
            return 0;
        case ModDynamicAH::House::Horde:
            return 1;
        case ModDynamicAH::House::Neutral:
            return 2;
        }
        return 2;
    }

    static const char *FamilyName(Family f)
    {
        switch (f)
        {
        case Family::Herb:
            return "herb";
        case Family::Ore:
            return "ore";
        case Family::Bar:
            return "bar";
        case Family::Cloth:
            return "cloth";
        case Family::Leather:
            return "leather";
        case Family::Dust:
            return "dust";
        case Family::Stone:
            return "stone";
        case Family::Meat:
            return "meat";
        case Family::Fish:
            return "fish";
        case Family::Gem:
            return "gem";
        case Family::Bandage:
            return "bandage";
        case Family::Potion:
            return "potion";
        case Family::Ink:
            return "ink";
        case Family::Pigment:
            return "pigment";
        default:
            return "other";
        }
    }

    static bool ParseFamily(std::string s, Family &out)
    {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        if (s == "herb")
            out = Family::Herb;
        else if (s == "ore")
            out = Family::Ore;
        else if (s == "bar")
            out = Family::Bar;
        else if (s == "cloth")
            out = Family::Cloth;
        else if (s == "leather")
            out = Family::Leather;
        else if (s == "dust")
            out = Family::Dust;
        else if (s == "stone")
            out = Family::Stone;
        else if (s == "meat")
            out = Family::Meat;
        else if (s == "fish")
            out = Family::Fish;
        else if (s == "gem")
            out = Family::Gem;
        else if (s == "bandage")
            out = Family::Bandage;
        else if (s == "potion")
            out = Family::Potion;
        else if (s == "ink")
            out = Family::Ink;
        else if (s == "pigment")
            out = Family::Pigment;
        else if (s == "other")
            out = Family::Other;
        else
            return false;
        return true;
    }

    struct ModuleState
    {
        // seller/planner
        bool enableSeller = true;
        bool dryRun = true;
        uint32 intervalMin = 30;
        uint32 maxRandomPerCycle = 50;
        bool loopEnabled = true;

        uint32 minPriceCopper = 10000;
        bool blockTrashAndCommon = true;
        bool allowQuality[6] = {false, false, true, true, true, false};
        std::unordered_set<uint32> whiteAllow;
        bool neverBuyAboveVendorBuyPrice = true;

        uint32 ownerAlliance = 0;
        uint32 ownerHorde = 0;
        uint32 ownerNeutral = 0;

        // context
        bool contextEnabled = true;
        uint32 contextMaxPerBracket = 4; // how many mats per bracket we may enqueue for one player
        double contextWeightBoost = 1.5; // multiplier for mats that occur in ≥2 recipes
        bool contextSkipVendor = true;

        // scarcity
        bool scarcityEnabled = true;
        double scarcityPriceBoostMax = 0.30;
        uint32 scarcityPerItemPerTickCap = 1;

        // vendor floor
        double vendorMinMarkup = 0.25;
        bool vendorConsiderBuyPrice = true;

        // price multipliers (1.0 = baseline 100 %)
        double mulDust = 1.00;
        double mulEssence = 1.25;
        double mulShard = 2.00;
        double mulElemental = 3.00;
        double mulRareRaw = 3.00;

        // stack targets
        uint32 stDefault = 20, stCloth = 20, stOre = 20, stBar = 20, stHerb = 20, stLeather = 20, stDust = 20, stGem = 20, stStone = 20, stMeat = 20, stBandage = 20, stPotion = 5, stInk = 10, stPigment = 20, stFish = 20;
        uint32 stacksLow = 2, stacksMid = 3, stacksHigh = 2;

        bool debugContextLogs = false;

        uint64 nextRunMs = 0;

        PostQueue postQueue;
        std::unordered_map<uint64, uint32> tickPlanCounts;

        std::unordered_map<uint32, uint8> vendorSoldCache;

        // ---- Economy (Gold/Quest scaling) ----
        double avgGoldPerQuest = 10.0;                       // default if cfg missing
        uint32 questsPerFamily[(size_t)Family::COUNT] = {0}; // filled from cfg

        struct CycleCache
        {
            std::unordered_map<uint64, uint32> scarcityByHouseItem;
            std::unordered_map<uint32, PricingResult> baselinePriceByItem;
            uint32 onlineCount = 0;
            void Clear()
            {
                scarcityByHouseItem.clear();
                baselinePriceByItem.clear();
                onlineCount = 0;
            }
        } cycle;

        struct Caps
        {
            bool enabled = true;
            uint32 totalPerCycleLimit = 150;
            uint32 totalPlanned = 0;
            uint32 perHouseLimit[3] = {80, 80, 120};
            uint32 perHousePlanned[3] = {0, 0, 0};
            uint32 familyLimit[(size_t)Family::COUNT];
            uint32 familyPlanned[(size_t)Family::COUNT];
            void ResetCounts()
            {
                totalPlanned = 0;
                for (int i = 0; i < 3; ++i)
                    perHousePlanned[i] = 0;
                for (size_t i = 0; i < (size_t)Family::COUNT; ++i)
                    familyPlanned[i] = 0;
            }
            void InitDefaults()
            {
                uint32 def[(size_t)Family::COUNT] =
                    {60, 50, 50, 60, 40, 40, 40, 25, 40, 20, 20, 30, 10, 20, 20, 20, 80};
                for (size_t i = 0; i < (size_t)Family::COUNT; ++i)
                {
                    familyLimit[i] = def[i];
                    familyPlanned[i] = 0;
                }
            }
        } caps;

        // setup config (values loaded from config)
        std::string setupAccName, setupAccPass, setupAccEmail;
        std::string setupAlliName, setupHordName, setupNeutName;
        uint8 setupAlliRace = 1, setupAlliClass = 1, setupAlliGender = 0;
        uint8 setupHordRace = 2, setupHordClass = 1, setupHordGender = 0;
        uint8 setupNeutRace = 5, setupNeutClass = 4, setupNeutGender = 0;
    };

    ModuleState g;

    struct CycleProfile
    {
        uint64 tScarcity = 0, tContext = 0, tRandom = 0, tBuyScan = 0, tApply = 0;
        uint32 plannedPosts = 0, appliedPosts = 0, plannedBuys = 0;
        void Reset() { *this = {}; }
    } g_lastProfile;

    static uint64 NowMs() { return uint64(GameTime::GetGameTimeMS().count()); }

    struct CreateInfoPublic : public CharacterCreateInfo
    {
        void SetBasics(std::string const &name, uint8 race, uint8 cls, uint8 gender)
        {
            Name = name;
            Race = race;
            Class = cls;
            Gender = gender;
            Skin = Face = HairStyle = HairColor = FacialHair = 0;
            OutfitId = 0;
            CharCount = 0;
        }
    };

    static inline ObjectGuid OwnerGuidFor(ModDynamicAH::House h)
    {
        uint32 low = 0;
        switch (h)
        {
        case ModDynamicAH::House::Alliance:
            low = g.ownerAlliance;
            break;
        case ModDynamicAH::House::Horde:
            low = g.ownerHorde;
            break;
        case ModDynamicAH::House::Neutral:
            low = g.ownerNeutral;
            break;
        }
        return low ? ObjectGuid::Create<HighGuid::Player>(low) : ObjectGuid::Empty;
    }

    static inline AuctionHouseId ToAH(ModDynamicAH::House h)
    {
        switch (h)
        {
        case ModDynamicAH::House::Alliance:
            return AuctionHouseId::Alliance;
        case ModDynamicAH::House::Horde:
            return AuctionHouseId::Horde;
        case ModDynamicAH::House::Neutral:
        default:
            return AuctionHouseId::Neutral;
        }
    }

    // ---------- Item-category lookup sets ----------
    static std::unordered_set<uint32> gEssenceSet, gShardSet, gElementalSet, gRareRawSet;

    static void InitCategorySets()
    {
        auto addList = [](auto const &tbl, std::unordered_set<uint32> &tgt)
        {
            for (auto const &br : tbl)
                for (uint32 id : br.items)
                    tgt.insert(id);
        };
        addList(ENCH_ESSENCE, gEssenceSet);
        addList(ENCH_SHARDS, gShardSet);
        addList(ELEMENTALS, gElementalSet);
        addList(RARE_RAW, gRareRawSet);
    }

    static double CategoryMultiplier(uint32 itemId)
    {
        if (gEssenceSet.count(itemId))
            return g.mulEssence;
        if (gShardSet.count(itemId))
            return g.mulShard;
        if (gElementalSet.count(itemId))
            return g.mulElemental;
        if (gRareRawSet.count(itemId))
            return g.mulRareRaw;
        return 1.0; // dust, herbs, etc.
    }

    // -------- Scarcity cache ----------
    static void BuildScarcityCache()
    {
        g.cycle.scarcityByHouseItem.clear();

        if (QueryResult r = CharacterDatabase.Query(
                "SELECT ii.itemEntry, ah.houseid, COUNT(*) "
                "FROM auctionhouse ah JOIN item_instance ii ON ii.guid = ah.itemguid "
                "GROUP BY ii.itemEntry, ah.houseid"))
        {
            do
            {
                Field *f = r->Fetch();
                uint32 itemEntry = f[0].Get<uint32>();
                uint32 houseId = f[1].Get<uint32>();
                uint32 cnt = f[2].Get<uint32>();
                uint64 key = (uint64(houseId) << 32) | itemEntry;
                g.cycle.scarcityByHouseItem[key] = cnt;
            } while (r->NextRow());
        }

        g.cycle.onlineCount = static_cast<uint32>(sWorldSessionMgr->GetActiveSessionCount());
    }

    static inline uint32 ScarcityCount(uint32 itemId, AuctionHouseId houseId)
    {
        uint64 key = (uint64(uint32(houseId)) << 32) | itemId;
        auto it = g.cycle.scarcityByHouseItem.find(key);
        return it != g.cycle.scarcityByHouseItem.end() ? it->second : 0u;
    }

    // returns: 0 = not vendor, 1 = limited stock, 2 = unlimited
    static uint8 VendorStockType(uint32 itemId, ItemTemplate const *tmpl)
    {
        auto it = g.vendorSoldCache.find(itemId);
        if (it != g.vendorSoldCache.end())
            return it->second;

        uint8 res = 0;

        // First check npc_vendor rows (preferred, tells us if limited)
        if (QueryResult qr = WorldDatabase.Query("SELECT maxcount FROM npc_vendor WHERE item = {} LIMIT 1", itemId))
        {
            int32 maxc = qr->Fetch()[0].Get<int32>(); // 0 = unlimited, >0 = limited stack
            res = (maxc == 0) ? 2 : 1;
        }

        // No vendor row – fall back to BuyPrice heuristic
        else if (g.vendorConsiderBuyPrice && tmpl && tmpl->BuyPrice > 0)
            res = 2;

        g.vendorSoldCache.emplace(itemId, res);
        return res;
    }

    static void ApplyVendorFloor(ItemTemplate const *tmpl, uint32 &startBid, uint32 &buyout)
    {
        if (!tmpl)
            return;
        uint32 vendorBuy = tmpl->BuyPrice;
        if (vendorBuy == 0)
        {
            startBid = std::max(startBid, g.minPriceCopper);
            buyout = std::max(buyout, std::max(startBid, g.minPriceCopper));
            return;
        }
        double floorD = std::max<double>(g.minPriceCopper, double(vendorBuy) * (1.0 + g.vendorMinMarkup));
        uint32 floorV = static_cast<uint32>(floorD);
        startBid = std::max(startBid, floorV);
        buyout = std::max(buyout, std::max(startBid, floorV));
    }

    // -------- Helpers ----------
    static double PriceJitter(uint32 itemId)
    {
        uint32 t = static_cast<uint32>(GameTime::GetGameTime().count());
        uint32 seed = t ^ (itemId * 2654435761u);
        int delta = int(seed % 11) - 5; // -5..+5
        return 1.0 + double(delta) / 100.0;
    }

    static uint32 ClampToStackable(ItemTemplate const *tmpl, uint32 desired)
    {
        uint32 maxStack = (tmpl && tmpl->Stackable > 0) ? tmpl->Stackable : 1u;
        if (desired == 0)
            desired = 1;
        return std::min(desired, maxStack);
    }

    static bool TryPlanOnce(ModDynamicAH::House house, uint32 itemId)
    {
        if (g.scarcityPerItemPerTickCap == 0)
            return true;
        uint64 key = (uint64(static_cast<uint32>(house)) << 32) | itemId;
        uint32 &cnt = g.tickPlanCounts[key];
        if (cnt >= g.scarcityPerItemPerTickCap)
            return false;
        ++cnt;
        return true;
    }

    // =========================================================================
    // Context planner (professions)
    // =========================================================================

    template <size_t N>
    static MatBracket const *FindBracket(uint16 skill, std::array<MatBracket, N> const &table)
    {
        for (MatBracket const &b : table)
            if (skill >= b.minSkill && skill < b.maxSkill)
                return &b;
        return nullptr;
    }

    template <size_t N>
    static uint32 PickForSkill(uint16 skill, std::array<MatBracket, N> const &table)
    {
        if (MatBracket const *bp = FindBracket(skill, table))
        {
            size_t idx = (GameTime::GetGameTime().count() / 60) % bp->items.size();
            return *(bp->items.begin() + idx);
        }
        return 0;
    }

    template <size_t N>
    static std::pair<uint32, uint32> PickPairForSkill(uint16 skill, std::array<MatBracket, N> const &table)
    {
        if (MatBracket const *bp = FindBracket(skill, table))
        {
            auto it = bp->items.begin();
            uint32 first = (bp->items.size() >= 1) ? *it : 0;
            uint32 second = 0;
            if (bp->items.size() >= 2)
            {
                ++it;
                second = *it;
            }
            if (((GameTime::GetGameTime().count() / 60) % 2) == 1 && second)
                std::swap(first, second);
            return {first, second};
        }
        return {0, 0};
    }

    static uint32 StacksForSkill(uint16 skill)
    {
        if (skill < 150)
            return g.stacksLow;
        if (skill < 300)
            return g.stacksMid;
        return g.stacksHigh;
    }

    // per-item/tick cap
    static bool EnqueueContextMat(Player *plr, uint32 itemId, uint32 desiredStack, char const *catName, uint16 skillForLog, std::unordered_set<uint32> &seenIdsThisPlayer, uint32 stacksToPost, Family fam)
    {
        if (!itemId)
            return false;
        if (seenIdsThisPlayer.find(itemId) != seenIdsThisPlayer.end())
            return false;

        ItemTemplate const *tmpl = sObjectMgr->GetItemTemplate(itemId);
        if (!tmpl)
            return false;

        uint8 vendType = VendorStockType(itemId, tmpl);

        ModDynamicAH::House house = (plr->GetTeamId() == TEAM_ALLIANCE) ? ModDynamicAH::House::Alliance : ModDynamicAH::House::Horde;
        AuctionHouseId ahId = ToAH(house);

        seenIdsThisPlayer.insert(itemId);

        uint32 vendorSell = tmpl->SellPrice;
        double base = std::max<double>(g.minPriceCopper, double(vendorSell) * 1.15);
        double levelScale = 1.0 + (std::min<uint32>(plr->GetLevel(), 80u) / 80.0) * 0.5;

        uint32 active = g.scarcityEnabled ? ScarcityCount(itemId, ahId) : 0;
        double scarcityBoost = g.scarcityEnabled ? (1.0 + g.scarcityPriceBoostMax / double(1 + active)) : 1.0;
        double jitter = PriceJitter(itemId);

        uint32 startBidBase = static_cast<uint32>(base * levelScale * scarcityBoost * jitter);
        uint32 buyoutBase = static_cast<uint32>(startBidBase * 1.6);

        double mul = CategoryMultiplier(itemId);
        startBidBase = uint32(double(startBidBase) * mul);
        buyoutBase = uint32(double(buyoutBase) * mul);

        VendorStockType(itemId, tmpl);
        ApplyVendorFloor(tmpl, startBidBase, buyoutBase);

        uint32 countPerStack = ClampToStackable(tmpl, desiredStack);

        uint32 stacksPosted = 0;
        for (uint32 i = 0; i < stacksToPost; ++i)
        {
            if (g.caps.enabled)
            {
                if (g.caps.totalPlanned >= g.caps.totalPerCycleLimit)
                    break;
                uint8 hi = HouseIdx(house);
                if (g.caps.perHousePlanned[hi] >= g.caps.perHouseLimit[hi])
                    break;
                size_t fi = static_cast<size_t>(fam);
                if (g.caps.familyPlanned[fi] >= g.caps.familyLimit[fi])
                    break;
                ++g.caps.totalPlanned;
                ++g.caps.perHousePlanned[hi];
                ++g.caps.familyPlanned[fi];
            }

            if (!TryPlanOnce(house, itemId))
                break;

            PostRequest req;
            req.house = house;
            req.itemId = itemId;
            req.count = countPerStack;
            req.startBid = startBidBase;
            req.buyout = buyoutBase;
            req.duration = 24 * HOUR;

            g.postQueue.Push(std::move(req));
            ++stacksPosted;
        }

        if (stacksPosted > 0 && g.debugContextLogs)
        {
            if (ItemTemplate const *t2 = sObjectMgr->GetItemTemplate(itemId))
            {
                LOG_INFO("mod.dynamicah", "[CTX] {} ({} skill={}) -> {} x{} ({} stacks)",
                         plr->GetName().c_str(), catName, skillForLog, t2->Name1.c_str(), countPerStack, stacksPosted);
            }
        }

        return stacksPosted > 0;
    }

    // Pick up to K distinct mats from the bracket list, applying a simple weight:
    // first item weight = boost (if list size >=2), rest = 1.0.
    template <size_t N>
    static std::vector<uint32> SelectMats(uint16 skill, std::array<MatBracket, N> const &table,
                                          uint32 wantCount, double weightBoost)
    {
        std::vector<uint32> out;
        if (wantCount == 0)
            return out;

        MatBracket const *bp = FindBracket(skill, table);
        if (!bp)
            return out;

        // Build a small local vector with weights.
        struct W
        {
            uint32 id;
            double w;
        };
        std::vector<W> pool;
        size_t idx = 0;
        for (uint32 id : bp->items)
        {
            double w = (idx == 0 && bp->items.size() >= 2) ? weightBoost : 1.0;
            pool.push_back({id, w});
            ++idx;
        }
        if (pool.empty())
            return out;

        // Simple weighted random without extra libs
        uint32 seed = static_cast<uint32>(GameTime::GetGameTime().count() ^ bp->minSkill);
        auto rand01 = [&seed]()
        {
            seed = seed * 1664525u + 1013904223u;
            return double(seed & 0xFFFF) / 65535.0;
        };

        while (!pool.empty() && out.size() < wantCount)
        {
            double totalW = 0.0;
            for (auto const &p : pool)
                totalW += p.w;
            double pick = rand01() * totalW;

            size_t chosen = 0;
            for (; chosen < pool.size(); ++chosen)
            {
                if (pick < pool[chosen].w)
                    break;
                pick -= pool[chosen].w;
            }
            out.push_back(pool[chosen].id);
            pool.erase(pool.begin() + chosen);
        }
        return out;
    }

    static uint32 PlanContextForOnePlayer(Player *plr)
    {
        if (!plr || !plr->IsInWorld())
            return 0;

        uint32 pushedForPlayer = 0;
        std::unordered_set<uint32> seen;

        auto canPush = [&]() -> bool
        { return true; };
        auto pushed = [&]()
        { ++pushedForPlayer; };

        // --- Tailoring / First Aid (cloth & bandages) ---
        if (plr->HasSkill(SKILL_TAILORING) || plr->HasSkill(SKILL_FIRST_AID))
        {
            uint16 s = plr->HasSkill(SKILL_TAILORING)
                           ? plr->GetSkillValue(SKILL_TAILORING)
                           : plr->GetSkillValue(SKILL_FIRST_AID);
            auto mats = SelectMats(s, TAILORING_CLOTH, std::min<uint32>(g.contextMaxPerBracket, 4u), g.contextWeightBoost);
            for (uint32 id : mats)
                if (canPush() && EnqueueContextMat(plr, id, g.stCloth, "Cloth", s, seen, StacksForSkill(s), Family::Cloth))
                    pushed();
        }

        // --- Mining (ores) ---
        if (plr->HasSkill(SKILL_MINING))
        {
            uint16 s = plr->GetSkillValue(SKILL_MINING);
            auto mats = SelectMats(s, MINING_ORE, g.contextMaxPerBracket, g.contextWeightBoost);
            for (uint32 id : mats)
                if (canPush() && EnqueueContextMat(plr, id, g.stOre, "Mining/Ore", s, seen, StacksForSkill(s), Family::Ore))
                    pushed();
        }

        // --- Blacksmithing (bars) ---
        if (canPush() && plr->HasSkill(SKILL_BLACKSMITHING))
        {
            uint16 s = plr->GetSkillValue(SKILL_BLACKSMITHING);
            if (EnqueueContextMat(plr, PickForSkill(s, BS_BARS), g.stBar, "Blacksmithing/Bars", s, seen, StacksForSkill(s), Family::Bar))
                pushed();
        }

        // --- Enchanting (dusts, essences, shards & rods) ---
        if (canPush() && plr->HasSkill(SKILL_ENCHANTING))
        {
            uint16 s = plr->GetSkillValue(SKILL_ENCHANTING);

            // 1) Dusts
            auto matsD = SelectMats(s, ENCH_DUSTS, g.contextMaxPerBracket, g.contextWeightBoost);
            for (uint32 id : matsD)
                if (canPush() && EnqueueContextMat(plr, id, g.stDust, "Enchanting/Dust", s, seen, StacksForSkill(s), Family::Dust))
                    pushed();

            // 2) Essences
            auto matsE = SelectMats(s, ENCH_ESSENCE, g.contextMaxPerBracket, g.contextWeightBoost);
            for (uint32 id : matsE)
                if (canPush() && EnqueueContextMat(plr, id, g.stDust, "Enchanting/Essence", s, seen, StacksForSkill(s), Family::Essence))
                    pushed();

            // 3) Shards & Rods (silver/golden/fel-iron rods)
            auto matsR = SelectMats(s, ENCH_SHARDS, g.contextMaxPerBracket, g.contextWeightBoost);
            for (uint32 id : matsR)
                if (canPush() && EnqueueContextMat(plr, id, 1, "Enchanting/Rods & Shards", s, seen, 1, Family::Shard))
                    pushed();
        }

        // --- Jewelcrafting (prospecting ore) ---
        if (plr->HasSkill(SKILL_JEWELCRAFTING))
        {
            uint16 s = plr->GetSkillValue(SKILL_JEWELCRAFTING);
            if (canPush() &&
                EnqueueContextMat(plr, PickForSkill(s, MINING_ORE), g.stOre, "Jewelcrafting/Ore", s, seen, StacksForSkill(s), Family::Ore))
                pushed();
        }

        // --- Inscription (herbs) ---
        if (plr->HasSkill(SKILL_INSCRIPTION))
        {
            uint16 s = plr->GetSkillValue(SKILL_INSCRIPTION);
            auto mats = SelectMats(s, HERBS, g.contextMaxPerBracket, g.contextWeightBoost);
            for (uint32 id : mats)
                if (canPush() && EnqueueContextMat(plr, id, g.stHerb, "Inscription/Herbs", s, seen, StacksForSkill(s), Family::Herb))
                    pushed();
        }

        // --- Alchemy (herbs + philosopher’s-stone dusts/essences) ---
        if (plr->HasSkill(SKILL_ALCHEMY))
        {
            uint16 s = plr->GetSkillValue(SKILL_ALCHEMY);

            // 1) Standard herb reagents
            auto matsH = SelectMats(s, HERBS, g.contextMaxPerBracket, g.contextWeightBoost);
            for (uint32 id : matsH)
                if (canPush() && EnqueueContextMat(plr, id, g.stHerb, "Alchemy/Herbs", s, seen, StacksForSkill(s), Family::Herb))
                    pushed();

            // 2) Philosopher’s-stone dusts (e.g. Strange Dust, Vision Dust…)
            auto matsD2 = SelectMats(s, ENCH_DUSTS, g.contextMaxPerBracket, g.contextWeightBoost);
            for (uint32 id : matsD2)
                if (canPush() && EnqueueContextMat(plr, id, g.stDust, "Alchemy/Dust", s, seen, StacksForSkill(s), Family::Dust))
                    pushed();

            // 3) Philosopher’s-stone essences (e.g. Lesser Magic Essence…)
            auto matsE2 = SelectMats(s, ENCH_ESSENCE, g.contextMaxPerBracket, g.contextWeightBoost);
            for (uint32 id : matsE2)
                if (canPush() && EnqueueContextMat(plr, id, g.stDust, "Alchemy/Essence", s, seen, StacksForSkill(s), Family::Essence))
                    pushed();
        }

        // --- Herbalism (herbs) ---
        if (plr->HasSkill(SKILL_HERBALISM))
        {
            uint16 s = plr->GetSkillValue(SKILL_HERBALISM);
            auto mats = SelectMats(s, HERBS, g.contextMaxPerBracket, g.contextWeightBoost);
            for (uint32 id : mats)
                if (canPush() && EnqueueContextMat(plr, id, g.stHerb, "Herbalism/Herbs", s, seen, StacksForSkill(s), Family::Herb))
                    pushed();
        }

        // --- Leatherworking & Skinning (leathers) ---
        if (canPush() && plr->HasSkill(SKILL_LEATHERWORKING))
        {
            uint16 s = plr->GetSkillValue(SKILL_SKINNING);
            if (EnqueueContextMat(plr, PickForSkill(s, LEATHERS), g.stLeather, "Leatherworking/Leather", s, seen, StacksForSkill(s), Family::Leather))
                pushed();
        }
        if (canPush() && plr->HasSkill(SKILL_SKINNING) && !plr->HasSkill(SKILL_LEATHERWORKING))
        {
            uint16 s = plr->GetSkillValue(SKILL_SKINNING);
            if (EnqueueContextMat(plr, PickForSkill(s, LEATHERS), g.stLeather, "Skinning/Leather", s, seen, StacksForSkill(s), Family::Leather))
                pushed();
        }

        // --- Engineering (stones & smelting bars) ---
        if (plr->HasSkill(SKILL_ENGINEERING))
        {
            uint16 eng = plr->GetSkillValue(SKILL_ENGINEERING);
            uint16 mine = plr->HasSkill(SKILL_MINING) ? plr->GetSkillValue(SKILL_MINING) : 1;
            uint16 ref = std::max<uint16>(eng, mine);

            if (canPush() &&
                EnqueueContextMat(plr, PickForSkill(ref, MINING_STONE), g.stStone, "Engineering/Stone", ref, seen, StacksForSkill(ref), Family::Stone))
                pushed();
            if (canPush() &&
                EnqueueContextMat(plr, PickForSkill(ref, SMELTING_BARS), g.stBar, "Engineering/Bars", ref, seen, StacksForSkill(ref), Family::Bar))
                pushed();
        }

        // --- Cooking (meat) ---
        if (canPush() && plr->HasSkill(SKILL_COOKING))
        {
            uint16 s = plr->GetSkillValue(SKILL_COOKING);
            static const std::array<MatBracket, 8> COOKING_MEAT = {{
                {1, 60, {769, 2672}},
                {60, 120, {3173, 3667}},
                {120, 180, {3730, 3731}},
                {180, 240, {3712, 12223}},
                {240, 300, {3174, 12037}},
                {300, 325, {27668, 27669}},
                {325, 350, {27682, 31670}},
                {350, 450, {43013, 43009}},
            }};
            if (EnqueueContextMat(plr, PickForSkill(s, COOKING_MEAT), g.stMeat, "Cooking/Meat", s, seen, StacksForSkill(s), Family::Meat))
                pushed();
        }

        // --- Fishing (raw fish) ---
        if (canPush() && plr->HasSkill(SKILL_FISHING))
        {
            uint16 s = plr->GetSkillValue(SKILL_FISHING);
            if (EnqueueContextMat(plr, PickForSkill(s, FISHING_RAW), g.stFish, "Fishing/Fish", s, seen, StacksForSkill(s), Family::Fish))
                pushed();
        }

        return pushedForPlayer;
    }

    static void BuildContextPlan()
    {
        if (!g.contextEnabled)
            return;

        uint32 considered = 0;
        HashMapHolder<Player>::MapType const &m = HashMapHolder<Player>::GetContainer();

        for (auto const &kv : m)
        {
            Player *plr = kv.second;
            if (!plr || !plr->IsInWorld())
                continue;
            (void)PlanContextForOnePlayer(plr);
            ++considered;
        }

        LOG_DEBUG("mod.dynamicah", "Context planner considered {} players; queue size now {}", considered, g.postQueue.Size());
    }

    // =========================================================================
    // Random plan
    // =========================================================================

    static double PriceMultiplierFromScarcity(uint32 itemId, AuctionHouseId houseId)
    {
        uint32 active = ScarcityCount(itemId, houseId);
        return g.scarcityEnabled ? (1.0 + g.scarcityPriceBoostMax / double(1 + active)) : 1.0;
    }

    static void BuildRandomPlan()
    {
        if (!g.enableSeller)
            return;

        SelectionConfig selCfg;
        selCfg.blockTrashAndCommon = g.blockTrashAndCommon;
        for (int i = 0; i < 6; ++i)
            selCfg.allowQuality[i] = g.allowQuality[i];
        selCfg.whiteAllowList = g.whiteAllow;
        selCfg.maxRandomPostsPerCycle = g.maxRandomPerCycle;
        selCfg.minPriceCopper = g.minPriceCopper;

        auto candidates = Selector::PickRandomSellables(selCfg, g.maxRandomPerCycle);

        for (ItemCandidate const &c : candidates)
        {
            ItemTemplate const *tmpl = c.tmpl;
            if (!tmpl)
                continue;

            PricingResult base;
            auto it = g.cycle.baselinePriceByItem.find(c.itemId);
            if (it != g.cycle.baselinePriceByItem.end())
                base = it->second;
            else
            {
                PricingInputs pin{tmpl, 0, g.cycle.onlineCount, g.minPriceCopper};
                base = PricePolicy::Compute(pin, Family::Other);
                g.cycle.baselinePriceByItem.emplace(c.itemId, base);
            }

            // simple house distribution
            uint8 which = static_cast<uint8>(c.itemId % 3);
            AuctionHouseId houseId = (which == 0) ? AuctionHouseId::Alliance : (which == 1) ? AuctionHouseId::Horde
                                                                                            : AuctionHouseId::Neutral;
            ModDynamicAH::House h = (houseId == AuctionHouseId::Alliance) ? ModDynamicAH::House::Alliance : (houseId == AuctionHouseId::Horde) ? ModDynamicAH::House::Horde
                                                                                                                                               : ModDynamicAH::House::Neutral;

            // caps (Other family)
            if (g.caps.enabled)
            {
                if (g.caps.totalPlanned >= g.caps.totalPerCycleLimit)
                    continue;
                uint8 hi = HouseIdx(h);
                if (g.caps.perHousePlanned[hi] >= g.caps.perHouseLimit[hi])
                    continue;
                size_t fi = static_cast<size_t>(Family::Other);
                if (g.caps.familyPlanned[fi] >= g.caps.familyLimit[fi])
                    continue;
                ++g.caps.totalPlanned;
                ++g.caps.perHousePlanned[hi];
                ++g.caps.familyPlanned[fi];
            }

            if (!TryPlanOnce(h, c.itemId))
                continue;

            double scarcity = PriceMultiplierFromScarcity(c.itemId, houseId);
            double jitter = PriceJitter(c.itemId);

            uint32 startBid = static_cast<uint32>(std::max<double>(g.minPriceCopper, base.startBid) * scarcity * jitter);
            uint32 buyout = static_cast<uint32>(std::max<double>(double(startBid) * 1.4, base.buyout) * scarcity * jitter);

            double mul = CategoryMultiplier(c.itemId);
            startBid = uint32(double(startBid) * mul);
            buyout = uint32(double(buyout) * mul);

            VendorStockType(c.itemId, tmpl);
            ApplyVendorFloor(tmpl, startBid, buyout);

            uint32 count = ClampToStackable(tmpl, g.stDefault);

            PostRequest req;
            req.house = h;
            req.itemId = c.itemId;
            req.count = count;
            req.startBid = startBid;
            req.buyout = buyout;
            req.duration = 24 * HOUR;

            g.postQueue.Push(std::move(req));
        }
    }

    // -------- Posting --------
    static bool PostSingleAuction(ChatHandler *handler, uint32 itemId, uint32 count, AuctionHouseId houseId, uint32 startBid, uint32 buyout, uint32 durationSeconds)
    {
        ObjectGuid owner = OwnerGuidFor((houseId == AuctionHouseId::Alliance) ? ModDynamicAH::House::Alliance : (houseId == AuctionHouseId::Horde) ? ModDynamicAH::House::Horde
                                                                                                                                                   : ModDynamicAH::House::Neutral);
        if (!owner)
        {
            if (handler)
                Chatf(handler, "ModDynamicAH: no seller GUID configured; run `.dah setup`.");
            return false;
        }

        Item *item = Item::CreateItem(itemId, count, nullptr);
        if (!item)
        {
            if (handler)
                Chatf(handler, "ModDynamicAH: could not create item %u", itemId);
            return false;
        }
        item->SetOwnerGUID(owner);
        item->SetGuidValue(ITEM_FIELD_CONTAINED, owner);

        AuctionHouseEntry const *ahEntry = AuctionHouseMgr::GetAuctionHouseEntryFromHouse(houseId);
        if (!ahEntry)
        {
            if (handler)
                Chatf(handler, "ModDynamicAH: invalid AH entry");
            delete item;
            return false;
        }

        uint32 deposit = AuctionHouseMgr::GetAuctionDeposit(ahEntry, durationSeconds, item, count);

        AuctionEntry *AH = new AuctionEntry;
        AH->Id = sObjectMgr->GenerateAuctionID();
        AH->houseId = houseId;
        AH->item_guid = item->GetGUID();
        AH->item_template = itemId;
        AH->itemCount = count;
        AH->owner = owner;
        AH->startbid = startBid;
        AH->bidder = ObjectGuid::Empty;
        AH->bid = 0;
        AH->buyout = buyout;
        uint32 auctionTime = uint32(durationSeconds * sWorld->getRate(RATE_AUCTION_TIME));
        AH->expire_time = GameTime::GetGameTime().count() + auctionTime;
        AH->deposit = deposit;
        AH->auctionHouseEntry = ahEntry;
        const char *itemName = "unknown";
        if (ItemTemplate const *tp = sObjectMgr->GetItemTemplate(itemId))
            itemName = tp->Name1.c_str();

        AuctionHouseObject *auctionHouse = sAuctionMgr->GetAuctionsMapByHouseId(houseId);
        sAuctionMgr->AddAItem(item);
        auctionHouse->AddAuction(AH);

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        item->SaveToDB(trans);
        AH->SaveToDB(trans);
        CharacterDatabase.CommitTransaction(trans);

        if (handler)
        {
            Chatf(handler,
                  "ModDynamicAH: posted %s (item %u) x%u auctionId=%u start=%u buyout=%u dur=%us house=%u",
                  itemName, itemId, count, AH->Id, startBid, buyout, durationSeconds, uint32(houseId));
        }

        LOG_INFO("mod.dynamicah", "Posted {} (item {}) x{} id {} start {} buyout {} dur {}s house {}",
                 itemName, itemId, count, AH->Id, startBid, buyout, durationSeconds, uint32(houseId));
        return true;
    }

    static void ApplyPlanOnWorld(uint32 maxToApply, ChatHandler *handler = nullptr)
    {
        auto batch = g.postQueue.Drain(maxToApply);
        if (batch.empty())
            return;

        for (PostRequest const &r : batch)
        {
            if (g.dryRun)
            {
                LOG_INFO("mod.dynamicah", "[DRY-RUN] Would post: item {} x{} house {} start={} buyout={} expires {}s",
                         r.itemId, r.count, static_cast<uint32>(r.house), r.startBid, r.buyout, r.duration);
                if (handler)
                    handler->PSendSysMessage("ModDynamicAH[DRY]: item %u x%u house=%u start=%u buyout=%u dur=%us",
                                             r.itemId, r.count, static_cast<uint32>(r.house), r.startBid, r.buyout, r.duration);
                continue;
            }
            AuctionHouseId ah = ToAH(r.house);
            if (PostSingleAuction(handler, r.itemId, r.count, ah, r.startBid, r.buyout, r.duration))
                ++g_lastProfile.appliedPosts;
        }
    }

    // -------- Setup helpers (account/characters) --------
    static char const *ResultToCStr(AccountOpResult r)
    {
        switch (r)
        {
        case AOR_OK:
            return "OK";
        case AOR_NAME_TOO_LONG:
            return "NAME_TOO_LONG";
        case AOR_PASS_TOO_LONG:
            return "PASS_TOO_LONG";
        case AOR_EMAIL_TOO_LONG:
            return "EMAIL_TOO_LONG";
        case AOR_NAME_ALREADY_EXIST:
            return "NAME_ALREADY_EXIST";
        case AOR_NAME_NOT_EXIST:
            return "NAME_NOT_EXIST";
        case AOR_DB_INTERNAL_ERROR:
            return "DB_INTERNAL_ERROR";
        default:
            return "UNKNOWN";
        }
    }

    static uint32 EnsureAccount(std::string const &name, std::string const &pass, std::string const &email)
    {
        if (uint32 accId = AccountMgr::GetId(name))
            return accId;
        AccountOpResult res = AccountMgr::CreateAccount(name, pass, email);
        if (res != AOR_OK && res != AOR_NAME_ALREADY_EXIST)
        {
            LOG_ERROR("mod.dynamicah", "CreateAccount('{}') failed: {}", name, ResultToCStr(res));
            return 0;
        }
        uint32 accId = AccountMgr::GetId(name);
        if (!accId)
        {
            LOG_ERROR("mod.dynamicah", "CreateAccount('{}') reported {}, but account ID still 0", name, ResultToCStr(res));
            return 0;
        }
        LOG_INFO("mod.dynamicah", "Account '{}' ready, id={}", name, accId);
        return accId;
    }

    static uint32 FindCharGuidByName(std::string const &name)
    {
        if (name.empty())
            return 0;
        std::string esc = name;
        CharacterDatabase.EscapeString(esc);
        std::string sql = "SELECT guid FROM characters WHERE name = '" + esc + "'";
        if (QueryResult r = CharacterDatabase.Query(sql.c_str()))
            return r->Fetch()[0].Get<uint32>();
        return 0;
    }

    static std::unique_ptr<WorldSession> MakeEphemeralSession(uint32 accountId, std::string const &accountName)
    {
        std::shared_ptr<WorldSocket> sock;
        AccountTypes sec = AccountTypes(SEC_PLAYER);
        uint8 expansion = uint8(sWorld->getIntConfig(CONFIG_EXPANSION));
        time_t mute_time = 0;
        LocaleConstant locale = LocaleConstant(LOCALE_enUS);
        uint32 recruiter = 0;
        bool isARecruiter = false;
        bool skipQueue = true;
        uint32 totalTime = 0;

        return std::make_unique<WorldSession>(accountId, std::string(accountName), sock, sec, expansion, mute_time, locale, recruiter, isARecruiter, skipQueue, totalTime);
    }

    static uint32 CreateCharacter(uint32 accountId, std::string const &name, uint8 race, uint8 cls, uint8 gender)
    {
        std::string accName = "";
        (void)AccountMgr::GetName(accountId, accName);
        std::unique_ptr<WorldSession> session = MakeEphemeralSession(accountId, accName);

        struct CreateInfoPublic info;
        info.SetBasics(name, race, cls, gender);

        std::shared_ptr<Player> newChar(new Player(session.get()), [](Player *ptr)
                                        { if (ptr->HasAtLoginFlag(AT_LOGIN_FIRST)) ptr->CleanupsBeforeDelete(); delete ptr; });
        newChar->GetMotionMaster()->Initialize();

        ObjectGuid::LowType lowGuid = sObjectMgr->GetGenerator<HighGuid::Player>().Generate();
        if (!newChar->Create(lowGuid, &info))
        {
            LOG_ERROR("mod.dynamicah", "CreateCharacter failed for '{}'", name);
            return 0;
        }
        newChar->SetAtLoginFlag(AT_LOGIN_FIRST);

        CharacterDatabaseTransaction characterTransaction = CharacterDatabase.BeginTransaction();
        newChar->SaveToDB(characterTransaction, true, false);
        CharacterDatabase.CommitTransaction(characterTransaction);

        LOG_INFO("mod.dynamicah", "Created character '{}' guid={}", name, lowGuid);
        return static_cast<uint32>(lowGuid);
    }

    static void DoSetup(ChatHandler *handler)
    {
        g.setupAccName = sConfigMgr->GetOption<std::string>(ModDynamicAH::CFG_SETUP_ACC_NAME, "dynamicah");
        g.setupAccPass = sConfigMgr->GetOption<std::string>(ModDynamicAH::CFG_SETUP_ACC_PASS, "change_me");
        g.setupAccEmail = sConfigMgr->GetOption<std::string>("ModDynamicAH.Setup.AccountEmail", "dynamicah@example.invalid");

        g.setupAlliName = sConfigMgr->GetOption<std::string>(ModDynamicAH::CFG_SETUP_ALLI_NAME, "AHSellerA");
        g.setupHordName = sConfigMgr->GetOption<std::string>(ModDynamicAH::CFG_SETUP_HORD_NAME, "AHSellerH");
        g.setupNeutName = sConfigMgr->GetOption<std::string>(ModDynamicAH::CFG_SETUP_NEUT_NAME, "AHSellerN");

        g.setupAlliRace = uint8(sConfigMgr->GetOption<uint32>(ModDynamicAH::CFG_SETUP_ALLI_RACE, 1));
        g.setupAlliClass = uint8(sConfigMgr->GetOption<uint32>(ModDynamicAH::CFG_SETUP_ALLI_CLASS, 1));
        g.setupAlliGender = uint8(sConfigMgr->GetOption<uint32>(ModDynamicAH::CFG_SETUP_ALLI_GENDER, 0));

        g.setupHordRace = uint8(sConfigMgr->GetOption<uint32>(ModDynamicAH::CFG_SETUP_HORD_RACE, 2));
        g.setupHordClass = uint8(sConfigMgr->GetOption<uint32>(ModDynamicAH::CFG_SETUP_HORD_CLASS, 1));
        g.setupHordGender = uint8(sConfigMgr->GetOption<uint32>(ModDynamicAH::CFG_SETUP_HORD_GENDER, 0));

        g.setupNeutRace = uint8(sConfigMgr->GetOption<uint32>(ModDynamicAH::CFG_SETUP_NEUT_RACE, 5));
        g.setupNeutClass = uint8(sConfigMgr->GetOption<uint32>(ModDynamicAH::CFG_SETUP_NEUT_CLASS, 4));
        g.setupNeutGender = uint8(sConfigMgr->GetOption<uint32>(ModDynamicAH::CFG_SETUP_NEUT_GENDER, 0));

        LOG_INFO("mod.dynamicah", "Setup starting: account='{}' email='{}'", g.setupAccName, g.setupAccEmail);

        uint32 accId = EnsureAccount(g.setupAccName, g.setupAccPass, g.setupAccEmail);
        if (!accId)
        {
            handler->PSendSysMessage("ModDynamicAH: setup failed creating or finding account '{}'", g.setupAccName);
            return;
        }

        auto ensureChar = [&](std::string const &n, uint8 r, uint8 c, uint8 gdr) -> uint32
        { if (uint32 guid = FindCharGuidByName(n)) return guid; return CreateCharacter(accId, n, r, c, gdr); };

        if (!g.ownerAlliance)
            g.ownerAlliance = ensureChar(g.setupAlliName, g.setupAlliRace, g.setupAlliClass, g.setupAlliGender);
        if (!g.ownerHorde)
            g.ownerHorde = ensureChar(g.setupHordName, g.setupHordRace, g.setupHordClass, g.setupHordGender);
        if (!g.ownerNeutral)
            g.ownerNeutral = ensureChar(g.setupNeutName, g.setupNeutRace, g.setupNeutClass, g.setupNeutGender);

        handler->PSendSysMessage("ModDynamicAH: setup complete.");
        handler->PSendSysMessage("Alliance GUID: {}", std::to_string(g.ownerAlliance));
        handler->PSendSysMessage("Horde GUID: {}", std::to_string(g.ownerHorde));
        handler->PSendSysMessage("Neutral GUID: {}", std::to_string(g.ownerNeutral));
        handler->PSendSysMessage("Copy these into config and `.reload config` to persist across restarts.");
        LOG_INFO("mod.dynamicah", "Setup finished: A={} H={} N={}", g.ownerAlliance, g.ownerHorde, g.ownerNeutral);
    }

    // -------- Scheduling --------
    static void DoOneCycle()
    {
        g_lastProfile.Reset();

        g.tickPlanCounts.clear();
        g.cycle.Clear();
        g.caps.ResetCounts();

        uint64 t0 = NowMs();
        BuildScarcityCache();
        g_lastProfile.tScarcity = NowMs() - t0;

        BuildContextPlan();
        g_lastProfile.tContext = 0;

        BuildRandomPlan();
        g_lastProfile.tRandom = 0;

        // BUY: configure & build
        uint64 t3 = NowMs();
        g_buy.ResetCycle();
        g_buy.SetFilters(g.allowQuality, g.whiteAllow);

        auto fairFn = [&](uint32_t itemId, uint32_t active) -> PricingResult
        {
            ItemTemplate const *tmpl = sObjectMgr->GetItemTemplate(itemId);
            PricingInputs pin{tmpl, active, g.cycle.onlineCount, g.minPriceCopper};
            return PricePolicy::Compute(pin, Family::Other);
        };
        auto scarceFn = [&](uint32_t itemId, AuctionHouseId house) -> uint32_t
        { return ScarcityCount(itemId, house); };
        auto vendorFn = [&](uint32_t itemId) -> std::pair<bool, uint32_t>
        {
            ItemTemplate const *tmpl = sObjectMgr->GetItemTemplate(itemId);
            uint8 vtype = VendorStockType(itemId, tmpl);
            bool isVendor = (vtype != 0);
            uint32 buy = (tmpl ? tmpl->BuyPrice : 0);
            return {isVendor, buy};
        };

        g_buy.BuildPlan(scarceFn, fairFn, vendorFn);
        g_lastProfile.tBuyScan = NowMs() - t3;

        g_lastProfile.plannedPosts = g.postQueue.Size();
    }
}

// ---------------- WorldScript ----------------
class ModDynamicAHWorld final : public WorldScript
{
public:
    ModDynamicAHWorld() : WorldScript("ModDynamicAHWorld") {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        using namespace ModDynamicAH;

        // seller / common options
        g.enableSeller = sConfigMgr->GetOption<bool>(CFG_ENABLE_SELLER, true);
        g.dryRun = sConfigMgr->GetOption<bool>(CFG_DRYRUN, true);
        g.intervalMin = sConfigMgr->GetOption<uint32>(CFG_INTERVAL_MIN, 30);
        g.maxRandomPerCycle = sConfigMgr->GetOption<uint32>(CFG_MAX_RANDOM, 50);
        g.minPriceCopper = sConfigMgr->GetOption<uint32>(CFG_MIN_PRICE, 10000);
        g.blockTrashAndCommon = sConfigMgr->GetOption<bool>(CFG_BLOCK_TRASH, true);
        g.allowQuality[0] = sConfigMgr->GetOption<bool>(CFG_ALLOW_Q_POOR, false);
        g.allowQuality[1] = sConfigMgr->GetOption<bool>(CFG_ALLOW_Q_NORMAL, false);
        g.allowQuality[2] = sConfigMgr->GetOption<bool>(CFG_ALLOW_Q_UNCOMMON, true);
        g.allowQuality[3] = sConfigMgr->GetOption<bool>(CFG_ALLOW_Q_RARE, true);
        g.allowQuality[4] = sConfigMgr->GetOption<bool>(CFG_ALLOW_Q_EPIC, true);
        g.allowQuality[5] = sConfigMgr->GetOption<bool>(CFG_ALLOW_Q_LEGENDARY, false);
        g.neverBuyAboveVendorBuyPrice = sConfigMgr->GetOption<bool>(CFG_NEVER_ABOVE_VENDOR_BUY, true);

        g.ownerAlliance = sConfigMgr->GetOption<uint32>(CFG_SELLER_OWNER_ALLI, g.ownerAlliance);
        g.ownerHorde = sConfigMgr->GetOption<uint32>(CFG_SELLER_OWNER_HORDE, g.ownerHorde);
        g.ownerNeutral = sConfigMgr->GetOption<uint32>(CFG_SELLER_OWNER_NEUT, g.ownerNeutral);

        std::string allowCsv = sConfigMgr->GetOption<std::string>(CFG_WHITE_ALLOW, "");
        g.whiteAllow = ParseCsvU32(allowCsv);

        // context
        g.contextEnabled = sConfigMgr->GetOption<bool>(CFG_CONTEXT_ENABLED, true);
        g.contextMaxPerBracket = sConfigMgr->GetOption<uint32>(CFG_CONTEXT_MAX_PER_BRACKET, 4u);
        g.contextWeightBoost = sConfigMgr->GetOption<float>(CFG_CONTEXT_WEIGHT_BOOST, 1.5f);
        g.contextSkipVendor = sConfigMgr->GetOption<bool>(CFG_CONTEXT_VENDOR_SKIP, true);

        // scarcity
        g.scarcityEnabled = sConfigMgr->GetOption<bool>(CFG_SCARCITY_ENABLED, true);
        g.scarcityPriceBoostMax = sConfigMgr->GetOption<float>(CFG_SCARCITY_PRICE_BOOST_MAX, 0.30f);
        g.scarcityPerItemPerTickCap = sConfigMgr->GetOption<uint32>(CFG_SCARCITY_PER_TICK_ITEM_CAP, 1);

        // vendor floor
        g.vendorMinMarkup = sConfigMgr->GetOption<float>(CFG_VENDOR_MIN_MARKUP, 0.25f);
        g.vendorConsiderBuyPrice = sConfigMgr->GetOption<bool>(CFG_VENDOR_CONSIDER_BUYPRICE, true);
        g.vendorSoldCache.clear();

        // stacks & counts
        g.stDefault = sConfigMgr->GetOption<uint32>(CFG_STACK_DEFAULT, 20u);
        g.stCloth = sConfigMgr->GetOption<uint32>(CFG_STACK_CLOTH, g.stDefault);
        g.stOre = sConfigMgr->GetOption<uint32>(CFG_STACK_ORE, g.stDefault);
        g.stBar = sConfigMgr->GetOption<uint32>(CFG_STACK_BAR, g.stDefault);
        g.stHerb = sConfigMgr->GetOption<uint32>(CFG_STACK_HERB, g.stDefault);
        g.stLeather = sConfigMgr->GetOption<uint32>(CFG_STACK_LEATHER, g.stDefault);
        g.stDust = sConfigMgr->GetOption<uint32>(CFG_STACK_DUST, g.stDefault);
        g.stGem = sConfigMgr->GetOption<uint32>(CFG_STACK_GEM, g.stDefault);
        g.stStone = sConfigMgr->GetOption<uint32>(CFG_STACK_STONE, g.stDefault);
        g.stMeat = sConfigMgr->GetOption<uint32>(CFG_STACK_MEAT, g.stDefault);
        g.stBandage = sConfigMgr->GetOption<uint32>(CFG_STACK_BANDAGE, g.stDefault);
        g.stPotion = sConfigMgr->GetOption<uint32>(CFG_STACK_POTION, 5u);
        g.stInk = sConfigMgr->GetOption<uint32>(CFG_STACK_INK, 10u);
        g.stPigment = sConfigMgr->GetOption<uint32>(CFG_STACK_PIGMENT, 20u);
        g.stFish = sConfigMgr->GetOption<uint32>(CFG_STACK_FISH, 20u);
        g.stacksLow = sConfigMgr->GetOption<uint32>(CFG_STACKS_LOW, 2u);
        g.stacksMid = sConfigMgr->GetOption<uint32>(CFG_STACKS_MID, 3u);
        g.stacksHigh = sConfigMgr->GetOption<uint32>(CFG_STACKS_HIGH, 2u);

        // loop
        g.debugContextLogs = sConfigMgr->GetOption<bool>(CFG_DEBUG_CONTEXT_LOGS, false);
        g.loopEnabled = sConfigMgr->GetOption<bool>(CFG_LOOP_ENABLED, true);

        // caps
        g.caps.InitDefaults();
        g.caps.enabled = sConfigMgr->GetOption<bool>(CFG_CAP_ENABLED, true);
        g.caps.totalPerCycleLimit = sConfigMgr->GetOption<uint32>(CFG_CAP_TOTAL, 150u);
        g.caps.perHouseLimit[0] = sConfigMgr->GetOption<uint32>(CFG_CAP_HOUSE_ALLI, 80u);
        g.caps.perHouseLimit[1] = sConfigMgr->GetOption<uint32>(CFG_CAP_HOUSE_HORDE, 80u);
        g.caps.perHouseLimit[2] = sConfigMgr->GetOption<uint32>(CFG_CAP_HOUSE_NEUT, 120u);
        auto loadFam = [&](Family f, char const *key, uint32 def)
        { g.caps.familyLimit[(size_t)f] = sConfigMgr->GetOption<uint32>(key, def); };
        auto FAM = [](char const *name) -> std::string
        { return std::string(ModDynamicAH::CFG_CAP_FAMILY_PREFIX) + name; };
        loadFam(Family::Herb, FAM("Herb").c_str(), 60u);
        loadFam(Family::Ore, FAM("Ore").c_str(), 50u);
        loadFam(Family::Bar, FAM("Bar").c_str(), 50u);
        loadFam(Family::Cloth, FAM("Cloth").c_str(), 60u);
        loadFam(Family::Leather, FAM("Leather").c_str(), 40u);
        loadFam(Family::Dust, FAM("Dust").c_str(), 40u);
        loadFam(Family::Stone, FAM("Stone").c_str(), 40u);
        loadFam(Family::Meat, FAM("Meat").c_str(), 20u);
        loadFam(Family::Fish, FAM("Fish").c_str(), 20u);
        loadFam(Family::Gem, FAM("Gem").c_str(), 30u);
        loadFam(Family::Bandage, FAM("Bandage").c_str(), 10u);
        loadFam(Family::Potion, FAM("Potion").c_str(), 20u);
        loadFam(Family::Ink, FAM("Ink").c_str(), 20u);
        loadFam(Family::Pigment, FAM("Pigment").c_str(), 20u);
        loadFam(Family::Other, FAM("Other").c_str(), 80u);

        // ----- Economy (Gold-per-Quest) -----
        g.avgGoldPerQuest = sConfigMgr->GetOption<float>(CFG_ECON_GOLD_PER_QUEST, 10.0f);

        auto loadQpf = [&](Family f, const char *key, uint32 def)
        { g.questsPerFamily[(size_t)f] = sConfigMgr->GetOption<uint32>(key, def); };

        auto ECON = [](const char *fam)
        { return std::string(CFG_ECON_QPF_PREFIX) + fam; };

        loadQpf(Family::Herb, ECON("Herb").c_str(), 1u);
        loadQpf(Family::Ore, ECON("Ore").c_str(), 1u);
        loadQpf(Family::Bar, ECON("Bar").c_str(), 1u);
        loadQpf(Family::Cloth, ECON("Cloth").c_str(), 1u);
        loadQpf(Family::Leather, ECON("Leather").c_str(), 1u);
        loadQpf(Family::Dust, ECON("Dust").c_str(), 1u);
        loadQpf(Family::Essence, ECON("Essence").c_str(), 1u);
        loadQpf(Family::Shard, ECON("Shard").c_str(), 1u);
        loadQpf(Family::Stone, ECON("Stone").c_str(), 1u);
        loadQpf(Family::Meat, ECON("Meat").c_str(), 1u);
        loadQpf(Family::Fish, ECON("Fish").c_str(), 1u);
        loadQpf(Family::Gem, ECON("Gem").c_str(), 1u);
        loadQpf(Family::Bandage, ECON("Bandage").c_str(), 1u);
        loadQpf(Family::Potion, ECON("Potion").c_str(), 1u);
        loadQpf(Family::Ink, ECON("Ink").c_str(), 1u);
        loadQpf(Family::Pigment, ECON("Pigment").c_str(), 1u);
        loadQpf(Family::Other, ECON("Other").c_str(), 1u);

        // ---- BUY SIDE: load into engine ----
        BuyEngineConfig bec;
        bec.enabled = sConfigMgr->GetOption<bool>(CFG_BUY_ENABLED, false);
        uint32 budgetGold = sConfigMgr->GetOption<uint32>(CFG_BUY_BUDGET_GOLD, 50u);
        bec.budgetCopper = budgetGold * 10000u;
        bec.minMargin = sConfigMgr->GetOption<float>(CFG_BUY_MIN_MARGIN, 0.15f);
        bec.perItemPerCycleCap = sConfigMgr->GetOption<uint32>(CFG_BUY_PER_ITEM_CAP, 2u);
        bec.maxScanRows = sConfigMgr->GetOption<uint32>(CFG_BUY_MAX_SCAN_ROWS, 2000u);
        bec.blockTrashAndCommon = sConfigMgr->GetOption<bool>(CFG_BUY_BLOCK_TRASH_COMMON, true);
        bec.vendorConsiderBuyPrice = g.vendorConsiderBuyPrice;
        bec.neverAboveVendorBuyPrice = g.neverBuyAboveVendorBuyPrice;
        bec.minPriceCopper = g.minPriceCopper;
        bec.scarcityEnabled = g.scarcityEnabled;
        bec.onlineCount = 0; // filled each cycle implicitly

        g_buy.SetConfig(bec);
        g_buy.SetFilters(g.allowQuality, g.whiteAllow);

        // reset per-cycle
        g.tickPlanCounts.clear();
        g.cycle.Clear();
        g.caps.ResetCounts();
        g.nextRunMs = NowMs() + 5000;

        g.mulDust = sConfigMgr->GetOption<float>(CFG_PRICE_MUL_DUST, 1.0f);
        g.mulEssence = sConfigMgr->GetOption<float>(CFG_PRICE_MUL_ESSENCE, 1.25f);
        g.mulShard = sConfigMgr->GetOption<float>(CFG_PRICE_MUL_SHARD, 2.0f);
        g.mulElemental = sConfigMgr->GetOption<float>(CFG_PRICE_MUL_ELEMENTAL, 3.0f);
        g.mulRareRaw = sConfigMgr->GetOption<float>(CFG_PRICE_MUL_RARERAW, 3.0f);

        static bool catInit = false;
        if (!catInit)
        {
            InitCategorySets();
            catInit = true;
        }

        LOG_INFO("mod.dynamicah",
                 "ModDynamicAH configured: seller={} every {}m; dryRun={} minPrice={}c; context={}; scarcity={} boostMax={} cap/tick={}; "
                 "vendor: minMarkup={} considerBuyPrice={}; owners A/H/N: {}/{}/{}; "
                 "Buy: enabled={} budget={}g minMargin={} perItemCap={} scanLimit={} blockTrashCommon={}",
                 g.enableSeller, g.intervalMin, g.dryRun, g.minPriceCopper,
                 g.contextEnabled,
                 g.scarcityEnabled, g.scarcityPriceBoostMax, g.scarcityPerItemPerTickCap,
                 g.vendorMinMarkup, g.vendorConsiderBuyPrice,
                 g.ownerAlliance, g.ownerHorde, g.ownerNeutral,
                 bec.enabled, (bec.budgetCopper / 10000u), bec.minMargin, bec.perItemPerCycleCap, bec.maxScanRows, bec.blockTrashAndCommon);
    }

    void OnUpdate(uint32 /*diff*/) override
    {
        uint64 now = uint64(GameTime::GetGameTimeMS().count());

        // Only plan/apply automatically if loop is enabled
        if (g.loopEnabled)
        {
            if (now >= g.nextRunMs)
            {
                DoOneCycle();
                g.nextRunMs = now + uint64(g.intervalMin) * MINUTE * IN_MILLISECONDS;
            }

            // Auto-apply small batches only while loop is on
            ApplyPlanOnWorld(10);
            g_buy.Apply(10, /*dryRun=*/g.dryRun, /*handler=*/nullptr);
        }
    }
};

// ---------------- .dah command ----------------
class dynamic_ah_commandscript final : public CommandScript
{
public:
    dynamic_ah_commandscript() : CommandScript("dynamic_ah_commandscript") {}

    Acore::ChatCommands::ChatCommandTable GetCommands() const override
    {
        using namespace Acore::ChatCommands;

        static ChatCommandTable buySub =
            {
                {"", HandleBuyShow, SEC_ADMINISTRATOR, Console::Yes},
                {"enable", HandleBuyEnable, SEC_ADMINISTRATOR, Console::Yes},
                {"fund", HandleBuyFund, SEC_ADMINISTRATOR, Console::Yes},
                {"budget", HandleBuyBudget, SEC_ADMINISTRATOR, Console::Yes},
                {"margin", HandleBuyMargin, SEC_ADMINISTRATOR, Console::Yes},
                {"peritem", HandleBuyPerItem, SEC_ADMINISTRATOR, Console::Yes},
                {"once", HandleBuyOnce, SEC_ADMINISTRATOR, Console::Yes},
            };

        static ChatCommandTable capsSetSub =
            {
                {"total", HandleCapsSetTotal, SEC_ADMINISTRATOR, Console::Yes},
                {"house", HandleCapsSetHouse, SEC_ADMINISTRATOR, Console::Yes},
                {"family", HandleCapsSetFamily, SEC_ADMINISTRATOR, Console::Yes},
            };

        static ChatCommandTable capsSub =
            {
                {"", HandleCapsShow, SEC_ADMINISTRATOR, Console::Yes},
                {"show", HandleCapsShow, SEC_ADMINISTRATOR, Console::Yes},
                {"enable", HandleCapsEnable, SEC_ADMINISTRATOR, Console::Yes},
                {"set", capsSetSub},
                {"resetcounts", HandleCapsResetCounts, SEC_ADMINISTRATOR, Console::Yes},
                {"defaults", HandleCapsDefaults, SEC_ADMINISTRATOR, Console::Yes},
            };

        static ChatCommandTable dahCommandTable =
            {
                {"plan", HandlePlan, SEC_ADMINISTRATOR, Console::Yes},
                {"run", HandleRun, SEC_ADMINISTRATOR, Console::Yes},
                {"loop", HandleLoop, SEC_ADMINISTRATOR, Console::Yes},
                {"dryrun", HandleDryRun, SEC_ADMINISTRATOR, Console::Yes},
                {"interval", HandleInterval, SEC_ADMINISTRATOR, Console::Yes},
                {"queue", HandleQueue, SEC_ADMINISTRATOR, Console::Yes},
                {"context", HandleContextCmd, SEC_ADMINISTRATOR, Console::Yes},
                {"clear", HandleClear, SEC_ADMINISTRATOR, Console::Yes},
                {"status", HandleStatus, SEC_ADMINISTRATOR, Console::Yes},
                {"price", HandlePriceCmd, SEC_ADMINISTRATOR, Console::Yes},
                {"buy", buySub},
                {"caps", capsSub},
                {"setup", HandleSetup, SEC_ADMINISTRATOR, Console::Yes},
            };

        static ChatCommandTable root = {{"dah", dahCommandTable}};
        return root;
    }

private:
    static bool HandleDryRun(ChatHandler *handler, Optional<uint32> onOff)
    {
        if (!onOff)
        {
            handler->PSendSysMessage("ModDynamicAH: dryRun is currently {}", g.dryRun ? "1 (DRY)" : "0 (LIVE)");
            return true;
        }

        g.dryRun = (*onOff != 0);

        handler->PSendSysMessage("ModDynamicAH: dryRun set to {} ({})",
                                 g.dryRun ? "1" : "0",
                                 g.dryRun ? "DRY-RUN (no real posts/buys)" : "LIVE (will post/buy)");

        LOG_INFO("mod.dynamicah", "CMD dryrun -> {}", g.dryRun ? "DRY" : "LIVE");
        return true;
    }

    static bool HandlePlan(ChatHandler *handler)
    {
        g.tickPlanCounts.clear();
        g.cycle.Clear();
        g.caps.ResetCounts();

        uint64 t0 = NowMs();
        BuildScarcityCache();
        uint64 tScar = NowMs() - t0;

        BuildContextPlan();
        BuildRandomPlan();

        // BUY
        g_buy.ResetCycle();
        g_buy.SetFilters(g.allowQuality, g.whiteAllow);

        auto fairFn = [&](uint32_t itemId, uint32_t active) -> PricingResult
        {
            ItemTemplate const *tmpl = sObjectMgr->GetItemTemplate(itemId);
            PricingInputs pin{tmpl, active, g.cycle.onlineCount, g.minPriceCopper};
            return PricePolicy::Compute(pin, Family::Other);
        };
        auto scarceFn = [&](uint32_t itemId, AuctionHouseId house) -> uint32_t
        { return ScarcityCount(itemId, house); };
        auto vendorFn = [&](uint32_t itemId) -> std::pair<bool, uint32_t>
        {
            ItemTemplate const *tmpl = sObjectMgr->GetItemTemplate(itemId);
            bool isVendor = (VendorStockType(itemId, tmpl) != 0);
            uint32 buy = (tmpl ? tmpl->BuyPrice : 0);
            return {isVendor, buy};
        };

        g_buy.BuildPlan(scarceFn, fairFn, vendorFn);

        handler->PSendSysMessage("ModDynamicAH: Plans built. Posts: {} Buys: {} (scarcity cache {} ms)",
                                 std::to_string(g.postQueue.Size()), std::to_string(g_buy.QueueSize()), std::to_string(tScar));
        LOG_INFO("mod.dynamicah", "CMD plan: posts={} buys={} scarcity_ms={}",
                 g.postQueue.Size(), g_buy.QueueSize(), static_cast<unsigned long long>(tScar));

        return true;
    }

    static bool HandleRun(ChatHandler *handler)
    {
        uint32 beforeP = g.postQueue.Size();
        size_t beforeB = g_buy.QueueSize();

        ApplyPlanOnWorld(100, handler);
        g_buy.Apply(100, /*dryRun=*/g.dryRun, handler);

        handler->PSendSysMessage("ModDynamicAH: Applied ({}). Posted={}, Queue left={} | BuysApplied(dry) ~{}, BuyQueue left={}",
                                 g.dryRun ? "dry-run" : "live",
                                 std::to_string((beforeP > g.postQueue.Size()) ? (beforeP - g.postQueue.Size()) : 0),
                                 std::to_string(g.postQueue.Size()),
                                 std::to_string((beforeB > g_buy.QueueSize()) ? (beforeB - g_buy.QueueSize()) : 0),
                                 std::to_string(g_buy.QueueSize()));
        uint32 posted = (beforeP > g.postQueue.Size()) ? (beforeP - g.postQueue.Size()) : 0u;
        uint32 buysApplied = (beforeB > g_buy.QueueSize()) ? (static_cast<uint32>(beforeB - g_buy.QueueSize())) : 0u;
        LOG_INFO("mod.dynamicah",
                 "CMD run: mode={} posted={} postQ_left={} buys_applied={} buyQ_left={}",
                 (g.dryRun ? "dry-run" : "live"),
                 posted,
                 g.postQueue.Size(),
                 buysApplied,
                 g_buy.QueueSize());

        return true;
    }

    static bool HandleLoop(ChatHandler *handler, Optional<uint32> onOffOpt)
    {
        if (!onOffOpt)
        {
            handler->PSendSysMessage("Usage: .dah loop <0|1> (current: {})", g.loopEnabled ? "1" : "0");
            return true;
        }
        g.loopEnabled = (*onOffOpt != 0);
        if (g.loopEnabled)
            g.nextRunMs = NowMs() + 1000;
        handler->PSendSysMessage("ModDynamicAH: loop {}", g.loopEnabled ? "enabled" : "disabled");
        return true;
    }

    static bool HandleInterval(ChatHandler *handler, Optional<uint32> minutesOpt)
    {
        if (!minutesOpt)
        {
            handler->PSendSysMessage("Usage: .dah interval <minutes> (current: {}m)", std::to_string(g.intervalMin));
            return true;
        }
        g.intervalMin = *minutesOpt ? *minutesOpt : 1;
        g.nextRunMs = NowMs() + 1000;
        handler->PSendSysMessage("ModDynamicAH: interval set to {} minutes", std::to_string(g.intervalMin));
        return true;
    }

    static bool HandleQueue(ChatHandler *handler)
    {
        handler->PSendSysMessage("ModDynamicAH: postQueue={} buyQueue={} budgetUsed={}/{}c",
                                 std::to_string(g.postQueue.Size()),
                                 std::to_string(g_buy.QueueSize()),
                                 std::to_string(g_buy.BudgetUsed()),
                                 std::to_string(g_buy.BudgetLimit()));
        LOG_INFO("mod.dynamicah", "CMD queue: postQ={} buyQ={} budgetUsed={}/{}",
                 g.postQueue.Size(),
                 g_buy.QueueSize(),
                 static_cast<unsigned long long>(g_buy.BudgetUsed()),
                 static_cast<unsigned long long>(g_buy.BudgetLimit()));

        return true;
    }
    static bool HandlePriceCmd(ChatHandler *handler, Optional<std::string> catOpt, Optional<uint32> pctOpt)
    {
        if (!catOpt)
        {
            handler->PSendSysMessage("Price multipliers (percent): dust={} essence={} shard={} elemental={} rareRaw={}",
                                     int(g.mulDust * 100), int(g.mulEssence * 100), int(g.mulShard * 100),
                                     int(g.mulElemental * 100), int(g.mulRareRaw * 100));
            handler->PSendSysMessage("Usage: .dah price <dust|essence|shard|elemental|rareraw> <percent>");
            return true;
        }
        if (!pctOpt)
        {
            handler->PSendSysMessage("Missing percent value.");
            return true;
        }

        double v = std::max(10.0, std::min<double>(*pctOpt, 1000.0)) / 100.0;

        std::string c = *catOpt;
        std::transform(c.begin(), c.end(), c.begin(), ::tolower);
        if (c == "dust")
            g.mulDust = v;
        else if (c == "essence")
            g.mulEssence = v;
        else if (c == "shard")
            g.mulShard = v;
        else if (c == "elemental")
            g.mulElemental = v;
        else if (c == "rareraw")
            g.mulRareRaw = v;
        else
        {
            handler->PSendSysMessage("Unknown category.");
            return true;
        }

        handler->PSendSysMessage("Multiplier for {} set to {:.2f} ({}%)", c, v, int(v * 100));
        LOG_INFO("mod.dynamicah", "CMD price {} -> {:.2f}", c, v);
        return true;
    }

    static bool HandleClear(ChatHandler *handler)
    {
        ApplyPlanOnWorld(1000000, handler);
        g_buy.Apply(1000000, /*dryRun=*/g.dryRun, handler);
        handler->PSendSysMessage("ModDynamicAH: applied/cleared pending posts & buys (dry-run={}): postQ={} buyQ={}",
                                 g.dryRun ? "1" : "0",
                                 std::to_string(g.postQueue.Size()),
                                 std::to_string(g_buy.QueueSize()));
        LOG_INFO("mod.dynamicah", "CMD clear: postQ_left={} buyQ_left={} mode={}",
                 g.postQueue.Size(),
                 g_buy.QueueSize(),
                 g.dryRun ? "dry-run" : "live");
        return true;
    }

    static bool HandleSetup(ChatHandler *handler)
    {
        DoSetup(handler);
        return true;
    }

    static bool HandleStatus(ChatHandler *handler)
    {
        handler->PSendSysMessage(
            "ModDynamicAH: seller={} dryRun={} interval={}m | owners A/H/N: {}/{}/{} | caps enabled={} totalCap={} | context={} perPlayer={} | postQ={} buyQ={}",
            g.enableSeller ? "1" : "0",
            g.dryRun ? "1" : "0",
            std::to_string(g.intervalMin),
            std::to_string(g.ownerAlliance),
            std::to_string(g.ownerHorde),
            std::to_string(g.ownerNeutral),
            g.caps.enabled ? "1" : "0",
            std::to_string(g.caps.totalPerCycleLimit),
            g.contextEnabled ? "1" : "0",
            std::to_string(g.postQueue.Size()),
            std::to_string(g_buy.QueueSize()));
        LOG_INFO("mod.dynamicah",
                 "CMD status: seller={} dryRun={} interval={}m owners A/H/N={}/{}/{} caps={} totalCap={} context={} postQ={} buyQ={}",
                 g.enableSeller, g.dryRun, g.intervalMin,
                 g.ownerAlliance, g.ownerHorde, g.ownerNeutral,
                 g.caps.enabled, g.caps.totalPerCycleLimit,
                 g.contextEnabled,
                 g.postQueue.Size(), g_buy.QueueSize());

        return true;
    }

    // ---- BUY command delegates ----
    static bool HandleBuyFund(ChatHandler *handler, Optional<uint32> goldOpt, Optional<std::string> houseOpt)
    {
        if (!goldOpt)
        {
            handler->PSendSysMessage("Usage: .dah buy fund <gold> [alliance|horde|neutral|all]");
            return true;
        }
        uint32 gold = *goldOpt;
        uint32 addCopper = gold * 10000u;

        // Build list of target owner GUIDs based on optional house argument
        struct Target
        {
            const char *name;
            uint32 guid;
        };
        std::vector<Target> targets;

        auto pushIf = [&](bool cond, const char *n, uint32 guid)
        {
            if (cond && guid)
                targets.push_back({n, guid});
        };

        std::string which = houseOpt ? *houseOpt : "all";
        std::transform(which.begin(), which.end(), which.begin(), ::tolower);

        bool wantAll = (which == "all" || which == "a");
        bool wantAlli = wantAll || which == "alliance" || which == "alli" || which == "ally";
        bool wantHorde = wantAll || which == "horde" || which == "h";
        bool wantNeutral = wantAll || which == "neutral" || which == "n";

        pushIf(wantAlli, "Alliance", g.ownerAlliance);
        pushIf(wantHorde, "Horde", g.ownerHorde);
        pushIf(wantNeutral, "Neutral", g.ownerNeutral);

        if (targets.empty())
        {
            handler->PSendSysMessage("ModDynamicAH[BUY]: No owner GUIDs configured (use .dah setup or set ModDynamicAH.Owner.*).");
            return true;
        }

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        for (auto const &t : targets)
        {
            CharacterDatabasePreparedStatement *stmt = CharacterDatabase.GetPreparedStatement(CHAR_UDP_CHAR_MONEY_ACCUMULATIVE);
            stmt->SetData(0, int32(addCopper)); // add money
            stmt->SetData(1, t.guid);           // guid
            trans->Append(stmt);
        }
        CharacterDatabase.CommitTransaction(trans);

        // Echo result
        for (auto const &t : targets)
            handler->PSendSysMessage("ModDynamicAH[BUY]: Funded {}g to {} owner (guid={})", std::to_string(gold), t.name, std::to_string(t.guid));

        LOG_INFO("mod.dynamicah", "[BUY] Funded {}g to owners: {}{}{} (A={} H={} N={})",
                 gold,
                 wantAlli ? "Alliance " : "",
                 wantHorde ? "Horde " : "",
                 wantNeutral ? "Neutral" : "",
                 g.ownerAlliance, g.ownerHorde, g.ownerNeutral);

        return true;
    }
    static bool HandleCapsShow(ChatHandler *handler)
    {
        auto yesno = [](bool b)
        { return b ? "1" : "0"; };

        // Summary
        handler->PSendSysMessage("ModDynamicAH[CAPS]: enabled={} totalLimit={} planned={}",
                                 yesno(g.caps.enabled),
                                 std::to_string(g.caps.totalPerCycleLimit),
                                 std::to_string(g.caps.totalPlanned));

        // Per-house
        handler->PSendSysMessage("ModDynamicAH[CAPS]: house Alliance {}/{}  Horde {}/{}  Neutral {}/{}",
                                 std::to_string(g.caps.perHousePlanned[0]), std::to_string(g.caps.perHouseLimit[0]),
                                 std::to_string(g.caps.perHousePlanned[1]), std::to_string(g.caps.perHouseLimit[1]),
                                 std::to_string(g.caps.perHousePlanned[2]), std::to_string(g.caps.perHouseLimit[2]));

        // Families
        std::string line;
        for (size_t i = 0; i < (size_t)Family::COUNT; ++i)
        {
            const char *name = FamilyName(static_cast<Family>(i));
            if (!line.empty())
                line += " | ";
            line += fmt::format("{} {}/{}", name, g.caps.familyPlanned[i], g.caps.familyLimit[i]);
        }
        handler->PSendSysMessage("ModDynamicAH[CAPS]: families: {}", line);

        LOG_INFO("mod.dynamicah",
                 "CMD caps show: enabled={} total {}/{} | house A {}/{} H {}/{} N {}/{} | {}",
                 g.caps.enabled,
                 g.caps.totalPlanned, g.caps.totalPerCycleLimit,
                 g.caps.perHousePlanned[0], g.caps.perHouseLimit[0],
                 g.caps.perHousePlanned[1], g.caps.perHouseLimit[1],
                 g.caps.perHousePlanned[2], g.caps.perHouseLimit[2],
                 line);
        return true;
    }

    static bool HandleCapsEnable(ChatHandler *handler, Optional<uint32> onOff)
    {
        if (!onOff)
        {
            handler->PSendSysMessage("Usage: .dah caps enable <0|1> (current: {})", g.caps.enabled ? "1" : "0");
            return true;
        }
        g.caps.enabled = (*onOff != 0);
        handler->PSendSysMessage("ModDynamicAH[CAPS]: {}", g.caps.enabled ? "enabled" : "disabled");
        LOG_INFO("mod.dynamicah", "CMD caps enable: {}", g.caps.enabled);
        return true;
    }

    static bool HandleCapsResetCounts(ChatHandler *handler)
    {
        g.caps.ResetCounts();
        handler->PSendSysMessage("ModDynamicAH[CAPS]: counts reset (limits unchanged)");
        LOG_INFO("mod.dynamicah", "CMD caps resetcounts");
        return true;
    }

    static bool HandleCapsSetTotal(ChatHandler *handler, uint32 value)
    {
        g.caps.totalPerCycleLimit = value;
        handler->PSendSysMessage("ModDynamicAH[CAPS]: totalPerCycleLimit = {}", std::to_string(value));
        LOG_INFO("mod.dynamicah", "CMD caps set total {}", value);
        return true;
    }

    static bool HandleCapsSetHouse(ChatHandler *handler, std::string which, uint32 value)
    {
        std::transform(which.begin(), which.end(), which.begin(), ::tolower);
        int idx = -1;
        if (which == "alliance" || which == "alli" || which == "ally" || which == "a")
            idx = 0;
        else if (which == "horde" || which == "h")
            idx = 1;
        else if (which == "neutral" || which == "n")
            idx = 2;

        if (idx < 0)
        {
            handler->PSendSysMessage("Unknown house '{}'. Expected alliance|horde|neutral.", which);
            return true;
        }

        g.caps.perHouseLimit[idx] = value;
        handler->PSendSysMessage("ModDynamicAH[CAPS]: house {} limit = {}",
                                 (idx == 0 ? "Alliance" : idx == 1 ? "Horde"
                                                                   : "Neutral"),
                                 std::to_string(value));
        LOG_INFO("mod.dynamicah", "CMD caps set house {} -> {}", which, value);
        return true;
    }

    static bool HandleCapsSetFamily(ChatHandler *handler, std::string famName, uint32 value)
    {
        std::transform(famName.begin(), famName.end(), famName.begin(), ::tolower);
        Family fam;
        if (!ParseFamily(famName, fam))
        {
            handler->PSendSysMessage("Unknown family '{}'.", famName);
            return true;
        }

        size_t i = static_cast<size_t>(fam);
        g.caps.familyLimit[i] = value;
        handler->PSendSysMessage("ModDynamicAH[CAPS]: family {} limit = {}", FamilyName(fam), std::to_string(value));
        LOG_INFO("mod.dynamicah", "CMD caps set family {} -> {}", FamilyName(fam), value);
        return true;
    }

    static bool HandleCapsDefaults(ChatHandler *handler)
    {
        // Restore default family limits and zero counters
        g.caps.InitDefaults();
        g.caps.ResetCounts();
        handler->PSendSysMessage("ModDynamicAH[CAPS]: family limits restored to defaults; counts reset");
        LOG_INFO("mod.dynamicah", "CMD caps defaults (family limits reset to module defaults)");
        return true;
    }

    static bool HandleCapsSet(ChatHandler *handler, Optional<std::string> domainOpt, Optional<std::string> nameOpt, Optional<uint32> valueOpt)
    {
        if (!domainOpt || !valueOpt)
        {
            handler->PSendSysMessage("Usage:");
            handler->PSendSysMessage(".dah caps set total <N>");
            handler->PSendSysMessage(".dah caps set house <alliance|horde|neutral> <N>");
            handler->PSendSysMessage(".dah caps set family <herb|ore|bar|cloth|leather|dust|stone|meat|fish|gem|bandage|potion|ink|pigment|other> <N>");
            return true;
        }

        std::string domain = *domainOpt;
        std::transform(domain.begin(), domain.end(), domain.begin(), ::tolower);

        uint32 val = *valueOpt;

        if (domain == "total")
        {
            g.caps.totalPerCycleLimit = val;
            handler->PSendSysMessage("ModDynamicAH[CAPS]: totalPerCycleLimit = {}", std::to_string(val));
            LOG_INFO("mod.dynamicah", "CMD caps set total {}", val);
            return true;
        }

        if (!nameOpt)
        {
            handler->PSendSysMessage("Missing name. Usage:");
            handler->PSendSysMessage(".dah caps set house <alliance|horde|neutral> <N>");
            handler->PSendSysMessage(".dah caps set family <name> <N>");
            return true;
        }

        std::string name = *nameOpt;
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);

        if (domain == "house")
        {
            int idx = -1;
            if (name == "alliance" || name == "alli" || name == "ally" || name == "a")
                idx = 0;
            else if (name == "horde" || name == "h")
                idx = 1;
            else if (name == "neutral" || name == "n")
                idx = 2;

            if (idx < 0)
            {
                handler->PSendSysMessage("Unknown house '{}'. Expected alliance|horde|neutral.", name);
                return true;
            }

            g.caps.perHouseLimit[idx] = val;
            handler->PSendSysMessage("ModDynamicAH[CAPS]: house {} limit = {}",
                                     (idx == 0 ? "Alliance" : idx == 1 ? "Horde"
                                                                       : "Neutral"),
                                     std::to_string(val));
            LOG_INFO("mod.dynamicah", "CMD caps set house {} -> {}", name, val);
            return true;
        }

        if (domain == "family")
        {
            Family fam;
            if (!ParseFamily(name, fam))
            {
                handler->PSendSysMessage("Unknown family '{}'.", name);
                return true;
            }
            size_t i = static_cast<size_t>(fam);
            g.caps.familyLimit[i] = val;

            handler->PSendSysMessage("ModDynamicAH[CAPS]: family {} limit = {}", FamilyName(fam), std::to_string(val));
            LOG_INFO("mod.dynamicah", "CMD caps set family {} -> {}", FamilyName(fam), val);
            return true;
        }

        handler->PSendSysMessage("Unknown domain '{}'. Use total | house | family.", domain);
        return true;
    }

    static bool HandleBuyShow(ChatHandler *handler)
    {
        g_buy.CmdShow(handler);
        return true;
    }
    static bool HandleBuyEnable(ChatHandler *handler, Optional<uint32> onOff)
    {
        if (!onOff)
        {
            handler->PSendSysMessage("Usage: .dah buy enable <0|1>");
            return true;
        }
        g_buy.CmdEnable(handler, (*onOff != 0));
        return true;
    }
    static bool HandleBuyBudget(ChatHandler *handler, Optional<uint32> goldOpt)
    {
        if (!goldOpt)
        {
            g_buy.CmdShow(handler);
            handler->PSendSysMessage("Usage: .dah buy budget <gold>");
            return true;
        }
        g_buy.CmdBudget(handler, *goldOpt);
        return true;
    }
    static bool HandleBuyMargin(ChatHandler *handler, Optional<uint32> pctOpt)
    {
        if (!pctOpt)
        {
            g_buy.CmdShow(handler);
            handler->PSendSysMessage("Usage: .dah buy margin <percent>");
            return true;
        }
        g_buy.CmdMargin(handler, *pctOpt);
        return true;
    }
    static bool HandleBuyPerItem(ChatHandler *handler, Optional<uint32> capOpt)
    {
        if (!capOpt)
        {
            g_buy.CmdShow(handler);
            handler->PSendSysMessage("Usage: .dah buy peritem <N>");
            return true;
        }
        g_buy.CmdPerItem(handler, *capOpt);
        return true;
    }
    static bool HandleBuyOnce(ChatHandler *handler)
    {
        auto fairFn = [&](uint32_t itemId, uint32_t active) -> PricingResult
        {
            ItemTemplate const *tmpl = sObjectMgr->GetItemTemplate(itemId);
            PricingInputs pin{tmpl, active, g.cycle.onlineCount, g.minPriceCopper};
            return PricePolicy::Compute(pin, Family::Other);
        };
        auto scarceFn = [&](uint32_t itemId, AuctionHouseId house) -> uint32_t
        { return ScarcityCount(itemId, house); };
        auto vendorFn = [&](uint32_t itemId) -> std::pair<bool, uint32_t>
        {
            ItemTemplate const *tmpl = sObjectMgr->GetItemTemplate(itemId);
            bool isVendor = (VendorStockType(itemId, tmpl) != 0);
            uint32 buy = (tmpl ? tmpl->BuyPrice : 0);
            return {isVendor, buy};
        };

        g_buy.CmdOnce(handler, scarceFn, fairFn, vendorFn);
        return true;
    }

    static bool HandleContextCmd(ChatHandler *handler, Optional<std::string> keyOpt, Optional<uint32> valOpt)
    {
        {
            // ----- Show current settings -----
            if (!keyOpt)
            {
                handler->PSendSysMessage("ModDynamicAH context settings:");
                handler->PSendSysMessage("maxPerBracket = {}", std::to_string(g.contextMaxPerBracket));
                handler->PSendSysMessage("weightBoost   = {:.2f}", g.contextWeightBoost);
                handler->PSendSysMessage("skipVendorMat = {}", g.contextSkipVendor ? "1" : "0");
                handler->PSendSysMessage("debugLogs     = {}", g.debugContextLogs ? "1" : "0");
                handler->PSendSysMessage("Usage:");
                handler->PSendSysMessage(".dah context maxperbracket <N>");
                handler->PSendSysMessage(".dah context weightboost <float>");
                handler->PSendSysMessage(".dah context skipvendor <0|1>");
                handler->PSendSysMessage(".dah context debug <0|1>");
                return true;
            }

            std::string k = *keyOpt;
            std::transform(k.begin(), k.end(), k.begin(), ::tolower);

            // ----- maxperbracket -----
            if (k == "maxperbracket" && valOpt)
            {
                g.contextMaxPerBracket = std::max<uint32>(1u, *valOpt);
                handler->PSendSysMessage("maxPerBracket set to {}", std::to_string(g.contextMaxPerBracket));
                LOG_INFO("mod.dynamicah", "CMD context maxPerBracket {}", g.contextMaxPerBracket);
                return true;
            }

            // ----- weightboost -----
            if (k == "weightboost" && valOpt)
            {
                g.contextWeightBoost = std::max(0.1, double(*valOpt) / 100.0); // valOpt is integer percent
                handler->PSendSysMessage("weightBoost set to {:.2f}", g.contextWeightBoost);
                LOG_INFO("mod.dynamicah", "CMD context weightBoost {:.2f}", g.contextWeightBoost);
                return true;
            }

            // ----- skipvendor -----
            if (k == "skipvendor" && valOpt)
            {
                g.contextSkipVendor = (*valOpt != 0);
                handler->PSendSysMessage("skipVendorMat set to {}", g.contextSkipVendor ? "1" : "0");
                LOG_INFO("mod.dynamicah", "CMD context skipVendorMat {}", g.contextSkipVendor);
                return true;
            }

            // ----- debug -----
            if (k == "debug" && valOpt)
            {
                g.debugContextLogs = (*valOpt != 0);
                handler->PSendSysMessage("debugContextLogs set to {}", g.debugContextLogs ? "1" : "0");
                LOG_INFO("mod.dynamicah", "CMD context debug {}", g.debugContextLogs);
                return true;
            }

            handler->PSendSysMessage("Unknown option for .dah context. Type .dah context for help.");
            return true;
        }
    }
};

// ---------------- Registration ----------------
void AddDynamicAhScripts()
{
    new ModDynamicAHWorld();
    new dynamic_ah_commandscript();
}
