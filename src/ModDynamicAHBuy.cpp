#include "ModDynamicAHBuy.h"

#include "Item.h"
#include "ObjectMgr.h"
#include "World.h"
#include "AuctionHouseMgr.h" // AuctionHouseObject, AuctionEntry, sAuctionMgr
#include "SharedDefines.h"   // ITEM_CLASS_TRADE_GOODS

using namespace ModDynamicAH;

// -------------------------------------------------------------------------------------------------
// Filters / helpers
// -------------------------------------------------------------------------------------------------

static std::string MoneyShort(uint32 copper)
{
    uint32 g = copper / 10000;
    uint32 s = (copper / 100) % 100;
    uint32 c = copper % 100;
    std::string out;
    if (g)
        out += fmt::format("{}g", g);
    if (s)
        out += fmt::format("{}s", s);
    if (c || out.empty())
        out += fmt::format("{}c", c);
    return out;
}
void BuyEngine::SetFilters(bool allowQuality[6], std::unordered_set<uint32_t> const &whiteAllow)
{
    for (int i = 0; i < 6; ++i)
        _allowQuality[i] = allowQuality[i];
    _whiteAllow = whiteAllow;
}

void BuyEngine::ResetCycle()
{
    _queue.clear();
    _perItemCount.clear();
    _budgetUsed = 0;
}

bool BuyEngine::_qualityAllowed(uint32_t itemId) const
{
    ItemTemplate const *t = sObjectMgr->GetItemTemplate(itemId);
    if (!t)
        return false;

    // Always allow explicitly allow-listed white/gray items
    if (t->Quality <= ITEM_QUALITY_NORMAL && _whiteAllow.find(itemId) != _whiteAllow.end())
        return true;

    // If it's Trade Goods (profession mats), allow even if it's white/gray.
    // This matches real WoW behavior where profession mats are commonly traded.
    if (t->Class == ITEM_CLASS_TRADE_GOODS)
        return true;

    // If caller configured "block trash & common", then block poor/common that are not allow-listed
    if ((t->Quality <= ITEM_QUALITY_NORMAL) && _cfg.blockTrashAndCommon)
        return false;

    // Otherwise, enforce the quality mask for uncommon+ (and for common when not blocking)
    if (t->Quality < 0 || t->Quality > 5)
        return false;

    return _allowQuality[t->Quality];
}

bool BuyEngine::_passesVendorSafety(uint32_t /*itemId*/, uint32_t unitBuyout, uint32_t vendorBuy) const
{
    if (!_cfg.neverAboveVendorBuyPrice)
        return true;

    if (!_cfg.vendorConsiderBuyPrice)
        return true;

    // If vendorBuy known and > 0, ensure we never buy above it (per unit)
    if (vendorBuy > 0 && unitBuyout > vendorBuy)
        return false;

    return true;
}

// -------------------------------------------------------------------------------------------------
// Planning (scan in-memory auctions; no SQL)
// -------------------------------------------------------------------------------------------------

void BuyEngine::BuildPlan(
    std::function<uint32_t(uint32_t, AuctionHouseId)> scarceFn,
    std::function<PricingResult(uint32_t, uint32_t)> fairFn,
    std::function<std::pair<bool, uint32_t>(uint32_t)> vendorFn)
{
    if (!_cfg.enabled)
    {
        LOG_INFO("mod.dynamicah", "[BUY] Disabled; skipping build");
        return;
    }

    uint32_t scanned = 0, considered = 0, accepted = 0, skipped = 0;
    uint32_t scanLimit = _cfg.maxScanRows ? _cfg.maxScanRows : 1000;

    auto scanHouse = [&](AuctionHouseId houseId)
    {
        AuctionHouseObject *ahObj = sAuctionMgr->GetAuctionsMapByHouseId(houseId);
        if (!ahObj)
            return;

        auto const &map = ahObj->GetAuctions();
        for (auto const &kv : map)
        {
            if (scanned >= scanLimit)
                break;

            AuctionEntry const *A = kv.second;
            if (!A)
                continue;

            ++scanned;

            uint32_t auctionId = A->Id;
            uint32_t itemId = A->item_template; // set on post; present in AuctionEntry
            uint32_t count = A->itemCount ? A->itemCount : 1u;
            uint32_t buyout = A->buyout;     // total stack buyout
            uint32_t startBid = A->startbid; // total stack startBid

            if (!buyout)
            {
                ++skipped;
                _traceWhy(_planEcho, "SKIP", "auc={} item={} reason=no-buyout", auctionId, itemId);
                continue;
            }

            ItemTemplate const *tmpl = sObjectMgr->GetItemTemplate(itemId);
            const char *itemName = tmpl ? tmpl->Name1.c_str() : "unknown";

            if (!tmpl)
            {
                ++skipped;
                _traceWhy(_planEcho, "SKIP", "auc={} item={} reason=no-template", auctionId, itemId);
                continue;
            }

            // Quality filter
            if (!_qualityAllowed(itemId))
            {
                ++skipped;
                _traceWhy(_planEcho, "SKIP",
                          "auc={} item={} '{}' quality={} filtered",
                          auctionId, itemId, itemName, uint32_t(tmpl->Quality));

                continue;
            }

            ++considered;

            // Scarcity for fair price calculation
            uint32_t activeCount = scarceFn ? scarceFn(itemId, houseId) : 0;
            PricingResult fair = fairFn ? fairFn(itemId, activeCount) : PricingResult{0, 0};

            // Compute "fair value" for this stack (use buyout guidance per unit if available)
            uint32_t fairUnit = fair.buyout ? fair.buyout : std::max<uint32_t>(_cfg.minPriceCopper, tmpl->SellPrice * 2);
            uint32_t fairStack = fairUnit * count;

            // Vendor safety
            uint32_t vendorBuy = 0;
            if (vendorFn)
            {
                auto v = vendorFn(itemId);
                vendorBuy = v.second;
            }
            uint32_t unitBuyout = (count ? (buyout / count) : buyout);
            if (!_passesVendorSafety(itemId, unitBuyout, vendorBuy))
            {
                ++skipped;
                _traceWhy(_planEcho, "SKIP",
                          "auc={} item={} '{}' reason=vendor-safety unitBuyout={} ({}) vendorBuy={} ({})",
                          auctionId, itemId, itemName,
                          unitBuyout, MoneyShort(unitBuyout),
                          vendorBuy, MoneyShort(vendorBuy));
                continue;
            }

            // Margin check (how much cheaper vs fair)
            float margin = 0.0f;
            if (fairStack > 0 && buyout < fairStack)
                margin = float(fairStack - buyout) / float(fairStack);

            if (margin < _cfg.minMargin)
            {
                ++skipped;
                _traceWhy(_planEcho, "SKIP",
                          "auc={} item={} '{}' reason=margin-too-small margin={:.1f}% need>={:.1f}% buyout={} ({}) fairStack={} ({})",
                          auctionId, itemId, itemName,
                          margin * 100.0f, _cfg.minMargin * 100.0f,
                          buyout, MoneyShort(buyout),
                          fairStack, MoneyShort(fairStack));

                continue;
            }

            // Per-item per-cycle cap
            uint32_t &plannedForItem = _perItemCount[itemId];
            if (plannedForItem >= _cfg.perItemPerCycleCap)
            {
                ++skipped;
                _traceWhy(_planEcho, "SKIP",
                          "auc={} item={} '{}' reason=per-item-cap cap={}",
                          auctionId, itemId, itemName, _cfg.perItemPerCycleCap);

                continue;
            }

            // Budget check
            if (_budgetUsed + buyout > _cfg.budgetCopper)
            {
                ++skipped;
                _traceWhy(_planEcho, "SKIP",
                          "auc={} item={} '{}' reason=budget-exceeded buyout={} ({}) used={} ({}) limit={} ({})",
                          auctionId, itemId, itemName,
                          buyout, MoneyShort(buyout),
                          _budgetUsed, MoneyShort(uint32(_budgetUsed)),
                          _cfg.budgetCopper, MoneyShort(uint32(_cfg.budgetCopper)));

                continue;
            }

            // Accept
            BuyCandidate bc;
            bc.auctionId = auctionId;
            bc.houseId = houseId;
            bc.itemId = itemId;
            bc.count = count;
            bc.buyout = buyout;
            bc.startBid = startBid;
            bc.vendorBuy = vendorBuy;
            bc.margin = margin;

            _queue.emplace_back(bc);
            ++plannedForItem;
            _budgetUsed += buyout;
            ++accepted;

            _traceWhy(_planEcho, "ACCEPT",
                      "auc={} item={} '{}' x{} unitBuyout={} ({}) fairUnit={} ({}) margin={:.1f}% house={}",
                      auctionId, itemId, itemName, count,
                      unitBuyout, MoneyShort(unitBuyout),
                      fairUnit, MoneyShort(fairUnit),
                      margin * 100.0f, static_cast<uint32_t>(houseId));

            if (scanned >= scanLimit)
                break;
        }
    };

    // Scan all 3 houses until we hit scanLimit
    scanHouse(AuctionHouseId::Alliance);
    if (scanned < scanLimit)
        scanHouse(AuctionHouseId::Horde);
    if (scanned < scanLimit)
        scanHouse(AuctionHouseId::Neutral);

    LOG_INFO("mod.dynamicah", "[BUY] scanned={} considered={} accepted={} skipped={} queue={} budget={}/{}",
             scanned, considered, accepted, skipped,
             _queue.size(),
             static_cast<unsigned long long>(_budgetUsed),
             static_cast<unsigned long long>(_cfg.budgetCopper));
}

// -------------------------------------------------------------------------------------------------
// Apply
// -------------------------------------------------------------------------------------------------

uint32_t BuyEngine::Apply(uint32_t maxToApply, bool dryRun, ChatHandler *handler)
{
    if (!_cfg.enabled)
        return 0;

    _chatLinesThisApply = 0;

    uint32_t applied = 0;
    for (BuyCandidate const &c : _queue)
    {
        if (applied >= maxToApply)
            break;

        if (dryRun)
        {
            _traceWhy(handler, "DRY", "auc={} item={} x{} buyout={} margin={:.1f}% house={} vendorBuy={}",
                      c.auctionId, c.itemId, c.count, c.buyout, c.margin * 100.0f,
                      static_cast<uint32_t>(c.houseId), c.vendorBuy);
        }
        else
        {
            // TODO: implement actual buyout logic with mail handling & gold management.
            _traceWhy(handler, "LIVE-NYI", "would buy auc={} item={} x{} buyout={} margin={:.1f}%",
                      c.auctionId, c.itemId, c.count, c.buyout, c.margin * 100.0f);
        }

        ++applied;
    }

    return applied;
}

// -------------------------------------------------------------------------------------------------
// Commands
// -------------------------------------------------------------------------------------------------

void BuyEngine::CmdShow(ChatHandler *handler) const
{
    if (handler)
    {
        handler->PSendSysMessage(
            "ModDynamicAH[BUY]: enabled={} budget={}/{} cap/item={} minMargin={:.1f}% scanLimit={} debug={} queue={}",
            _cfg.enabled ? "1" : "0",
            static_cast<unsigned long long>(_budgetUsed),
            static_cast<unsigned long long>(_cfg.budgetCopper),
            _cfg.perItemPerCycleCap,
            _cfg.minMargin * 100.0f,
            _cfg.maxScanRows,
            _debug ? "1" : "0",
            _queue.size());
    }
    LOG_INFO("mod.dynamicah",
             "[BUY] enabled={} budget={}/{} cap/item={} minMargin={:.1f}% scanLimit={} debug={} queue={}",
             _cfg.enabled, _budgetUsed, _cfg.budgetCopper, _cfg.perItemPerCycleCap,
             _cfg.minMargin * 100.0f, _cfg.maxScanRows, _debug, _queue.size());
}

void BuyEngine::CmdEnable(ChatHandler *handler, bool enable)
{
    _cfg.enabled = enable;
    if (handler)
        handler->PSendSysMessage("ModDynamicAH[BUY]: {}", enable ? "enabled" : "disabled");
}

void BuyEngine::CmdBudget(ChatHandler *handler, uint32_t gold)
{
    _cfg.budgetCopper = uint64_t(gold) * 10000ull;
    if (handler)
        handler->PSendSysMessage("ModDynamicAH[BUY]: budget set to {} gold", gold);
}

void BuyEngine::CmdMargin(ChatHandler *handler, uint32_t percent)
{
    _cfg.minMargin = std::max(0u, std::min(percent, 95u)) / 100.0f;
    if (handler)
        handler->PSendSysMessage("ModDynamicAH[BUY]: min margin set to {}%", percent);
}

void BuyEngine::CmdPerItem(ChatHandler *handler, uint32_t cap)
{
    _cfg.perItemPerCycleCap = std::max<uint32_t>(1u, cap);
    if (handler)
        handler->PSendSysMessage("ModDynamicAH[BUY]: per-item/cycle cap set to {}", _cfg.perItemPerCycleCap);
}

void BuyEngine::CmdOnce(ChatHandler *handler,
                        std::function<uint32_t(uint32_t, AuctionHouseId)> scarceFn,
                        std::function<PricingResult(uint32_t, uint32_t)> fairFn,
                        std::function<std::pair<bool, uint32_t>(uint32_t)> vendorFn)
{
    ResetCycle();
    // Enable chat echo of plan-phase reasons for this one command
    _planEcho = handler;
    BuildPlan(scarceFn, fairFn, vendorFn);
    _planEcho = nullptr;

    uint32_t did = Apply(50, /*dryRun=*/true, handler);
    if (handler)
        handler->PSendSysMessage("ModDynamicAH[BUY]: dry-run would apply ~{} buys (queue={})", did, _queue.size());
}

void BuyEngine::CmdDebug(ChatHandler *handler, bool on)
{
    _debug = on;
    if (handler)
        handler->PSendSysMessage("ModDynamicAH[BUY]: debug {}", on ? "enabled" : "disabled");
}
