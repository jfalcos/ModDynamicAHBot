#pragma once

#include <cstdint>
#include <string>

#include "Optional.h"
#include "ModDynamicAHBuy.h"
#include "DynamicAHPlanner.h"
#include "DynamicAHPosting.h"
#include "DynamicAHState.h"

class ChatHandler;

namespace ModDynamicAH
{
    class Service
    {
    public:
        static Service& Instance();

        // lifecycle
        void OnConfigLoad();
        void OnUpdate(uint32 diffMs);

        // runtime controls
        void ToggleLoop(bool enable, ChatHandler* handler = nullptr);
        void SetDryRun(bool dry, ChatHandler* handler = nullptr);
        void SetInterval(uint32 minutes, ChatHandler* handler = nullptr);

        // one-shots
        void PlanOnce(ChatHandler* handler = nullptr);
        void ApplyOnce(ChatHandler* handler = nullptr);
        void ClearQueues(ChatHandler* handler = nullptr);

        // info
        void ShowStatus(ChatHandler* handler);
        void ShowQueue(ChatHandler* handler);

        // accessors
        ModuleState& State() { return state_; }
        DynamicAHPlanner& Planner() { return planner_; }
        DynamicAHPlanner const& Planner() const { return planner_; }
        BuyEngine& Buy() { return buy_; }
        BuyEngine const& Buy() const { return buy_; }

        // admin commands
        void CmdFund(ChatHandler* handler, uint32 gold, std::string const& which);
        void CmdCapsShow(ChatHandler* handler);
        void CmdCapsEnable(ChatHandler* handler, bool enable);
        void CmdCapsReset(ChatHandler* handler);
        void CmdCapsDefaults(ChatHandler* handler);
        void CmdCapsSetTotal(ChatHandler* handler, uint32 value);
        void CmdCapsSetHouse(ChatHandler* handler, std::string which, uint32 value);
        void CmdCapsSetFamily(ChatHandler* handler, std::string famName, uint32 value);
        bool CmdContext(ChatHandler* handler, Optional<std::string> keyOpt, Optional<uint32> valOpt);

    private:
        Service() = default;
        void DoOneCycle();

        ModuleState state_;
        DynamicAHPlanner planner_;
        BuyEngine buy_;
    };
} // namespace ModDynamicAH
