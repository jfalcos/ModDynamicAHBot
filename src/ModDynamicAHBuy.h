
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <fmt/format.h>

#include "Optional.h"
#include "Chat.h"             // ChatHandler
#include "AuctionHouseMgr.h"  // AuctionHouseId
#include "DynamicAHTypes.h"   // shared enums/aliases for the module
#include "DynamicAHPricing.h" // PricingResult

namespace ModDynamicAH
{
    struct ModuleState;

    
struct BuyCandidate
{
    uint32_t auctionId = 0;
    AuctionHouseId houseId{};
    uint32_t itemId = 0;
    uint32_t count = 1;
    uint32_t buyout = 0;    // total stack price
    uint32_t startBid = 0;  // total stack start bid
    uint32_t vendorBuy = 0; // vendor unit buy price (if any)
    float    margin = 0.0f; // computed margin vs fair
};

    struct BuyEngineConfig
    {
        bool enabled = false;
        uint64_t budgetCopper = 0;
        float    minMargin = 0.15f;           // 15%
        uint32_t perItemPerCycleCap = 2;
        uint32_t maxScanRows = 2000;
        bool     blockTrashAndCommon = true;
        bool     vendorConsiderBuyPrice = true;
        bool     neverAboveVendorBuyPrice = true;
        uint32_t minPriceCopper = 10000;
        bool     scarcityEnabled = true;
        uint32_t onlineCount = 0; // informational
    };

    class BuyEngine
    {
    public:
        using Config = BuyEngineConfig;

        // Logging helpers
        void LogBuyDecision(char const* phase, uint32_t aucId, uint32_t itemId, uint32_t count,
                            uint32_t unitAskCopper, uint32_t fairUnitCopper, double marginPct,
                            uint32_t budgetRemainCopper, char const* reason) const;
        void LogBuyResult(uint32_t aucId, uint32_t itemId, uint32_t count,
                          uint32_t unitPaidCopper, char const* result) const;

        BuyEngine() = default;

        // Configuration
        void SetConfig(BuyEngineConfig const& cfg);
        BuyEngineConfig const& GetConfig() const { return _cfg; }

        // Filters
        void SetFilters(bool allowQuality[6], std::unordered_set<uint32_t> const& whiteAllow);
        void ResetCycle();

        // Planning: functions provided by planner/service to get scarcity, fair price, and vendor price
        void BuildPlan(std::function<std::uint32_t(std::uint32_t, AuctionHouseId)> scarceFn,
                       std::function<ModDynamicAH::PricingResult(std::uint32_t, std::uint32_t)> fairFn,
                       std::function<std::pair<bool, std::uint32_t>(std::uint32_t)> vendorFn);

        // Applying planned buys
        uint32_t Apply(uint32_t maxToApply, bool dryRun, ChatHandler* handler);

        // Admin & debug commands
        void CmdShow(ChatHandler* handler) const;
        void CmdEnable(ChatHandler* handler, bool enable);
        void CmdBudget(ChatHandler* handler, uint32_t gold);
        void CmdMargin(ChatHandler* handler, uint32_t percent);
        void CmdPerItem(ChatHandler* handler, uint32_t cap);
        void CmdOnce(ChatHandler* handler,
                     std::function<std::uint32_t(std::uint32_t, AuctionHouseId)> scarceFn,
                     std::function<ModDynamicAH::PricingResult(std::uint32_t, std::uint32_t)> fairFn,
                     std::function<std::pair<bool, std::uint32_t>(std::uint32_t)> vendorFn);
        void CmdDebug(ChatHandler* handler, bool on);

        // Introspection
        uint32_t QueueSize() const { return (uint32_t)_queue.size(); }
        uint64_t BudgetLimit() const { return _cfg.budgetCopper; }
        uint64_t BudgetUsed() const { return _budgetUsed; }

    
private:
        // Debug trace helper - fmt-style formatting
        template <typename... Args>
        void _traceWhy(ChatHandler* handler, char const* tag, fmt::format_string<Args...> f, Args&&... args) const
        {
            if (!handler) return;
            handler->PSendSysMessage("ModDynamicAH[BUY][{}]: {}", tag, fmt::format(f, std::forward<Args>(args)...));
        }

private:
        // Helpers implemented in .cpp
        bool _qualityAllowed(uint32_t itemId) const;
        bool _passesVendorSafety(uint32_t /*itemId*/, uint32_t unitBuyout, uint32_t vendorBuy) const;
BuyEngineConfig _cfg;
        bool _allowQuality[6] = {false, false, true, true, true, false};
        std::unordered_set<uint32_t> _whiteAllow;

        std::vector<BuyCandidate> _queue;
        std::unordered_map<uint32_t, uint32_t> _perItemCount; // itemId -> planned buys in this cycle
        uint64_t _budgetUsed = 0;

        // Debug
        bool _debug = false;
        mutable uint32_t _chatLinesThisApply = 0;
        static constexpr uint32_t _chatLineCapPerApply = 40;

        // Plan-phase chat echo (only when invoked from .dah buy once)
        ChatHandler* _planEcho = nullptr;
    };

} // namespace ModDynamicAH
