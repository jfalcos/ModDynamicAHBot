#pragma once
#include "DatabaseEnv.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ModDynamicAH
{
    struct SelectionConfig
    {
        bool blockTrashAndCommon = true;
        bool allowQuality[6] = {false, false, true, true, true, false}; // Poor..Legendary
        std::unordered_set<uint32> whiteAllowList;                      // itemId whitelist for white quality
        uint32 maxRandomPostsPerCycle = 50;
        uint32 minPriceCopper = 10000;
    };

    struct ItemCandidate
    {
        uint32 itemId = 0;
        ItemTemplate const *tmpl = nullptr;
        uint32 scarcityCount = 0;
    };

    class Selector
    {
    public:
        static std::vector<ItemCandidate> PickRandomSellables(SelectionConfig const &cfg, uint32 wantCount)
        {
            std::unordered_map<uint32, uint32> liveCounts;

            if (QueryResult r = CharacterDatabase.Query(
                    "SELECT ii.itemEntry, COUNT(*) "
                    "FROM auctionhouse ah "
                    "JOIN item_instance ii ON ii.guid = ah.itemguid "
                    "GROUP BY ii.itemEntry"))
            {
                do
                {
                    Field *f = r->Fetch();
                    uint32 entry = f[0].Get<uint32>();
                    uint32 cnt = f[1].Get<uint32>();
                    liveCounts[entry] = cnt;
                } while (r->NextRow());
            }

            std::vector<ItemCandidate> pool;
            if (QueryResult r2 = WorldDatabase.Query(
                    "SELECT entry FROM item_template WHERE BuyPrice > 0 OR SellPrice > 0"))
            {
                do
                {
                    uint32 id = r2->Fetch()[0].Get<uint32>();
                    ItemTemplate const *tmpl = sObjectMgr->GetItemTemplate(id);
                    if (!tmpl)
                        continue;

                    bool okQuality = false;
                    switch (tmpl->Quality)
                    {
                    case ITEM_QUALITY_POOR:
                        okQuality = cfg.allowQuality[0];
                        break;
                    case ITEM_QUALITY_NORMAL:
                        okQuality = cfg.allowQuality[1];
                        break;
                    case ITEM_QUALITY_UNCOMMON:
                        okQuality = cfg.allowQuality[2];
                        break;
                    case ITEM_QUALITY_RARE:
                        okQuality = cfg.allowQuality[3];
                        break;
                    case ITEM_QUALITY_EPIC:
                        okQuality = cfg.allowQuality[4];
                        break;
                    case ITEM_QUALITY_LEGENDARY:
                        okQuality = cfg.allowQuality[5];
                        break;
                    default:
                        okQuality = false;
                        break;
                    }

                    if (!okQuality)
                    {
                        if (!(tmpl->Quality == ITEM_QUALITY_NORMAL && cfg.whiteAllowList.count(id)))
                            continue;
                    }

                    if (cfg.blockTrashAndCommon)
                    {
                        if (tmpl->Quality == ITEM_QUALITY_POOR)
                            continue;
                        if (tmpl->Quality == ITEM_QUALITY_NORMAL && !cfg.whiteAllowList.count(id))
                            continue;
                    }

                    ItemCandidate c;
                    c.itemId = id;
                    c.tmpl = tmpl;
                    auto it = liveCounts.find(id);
                    c.scarcityCount = (it == liveCounts.end()) ? 0u : it->second;
                    pool.emplace_back(std::move(c));
                } while (r2->NextRow());
            }

            std::vector<ItemCandidate> out;
            out.reserve(std::min<uint32>(wantCount, cfg.maxRandomPostsPerCycle));

            for (ItemCandidate const &c : pool)
            {
                if (out.size() >= cfg.maxRandomPostsPerCycle)
                    break;

                if (c.scarcityCount < 5 || (c.itemId % 7 == 0))
                    out.push_back(c);
            }

            if (out.size() > wantCount)
                out.resize(wantCount);

            return out;
        }
    };
}
