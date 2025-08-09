#include "Chat.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "DynamicAHPosting.h"
#include "DynamicAHState.h"
#include "AuctionHouseMgr.h"
#include "World.h"
#include "GameTime.h"
#include "Log.h"
#include "Item.h"
#include "DatabaseEnv.h"

namespace ModDynamicAH
{

    ObjectGuid DynamicAHPosting::OwnerGuidFor(ModuleState const &s, AuctionHouseId house)
    {
        switch (house)
        {
        case AuctionHouseId::Alliance:
            return ObjectGuid::Create<HighGuid::Player>(s.ownerAlliance);
        case AuctionHouseId::Horde:
            return ObjectGuid::Create<HighGuid::Player>(s.ownerHorde);
        case AuctionHouseId::Neutral:
            return ObjectGuid::Create<HighGuid::Player>(s.ownerNeutral);
        default:
            return ObjectGuid::Empty;
        }
    }

    bool DynamicAHPosting::PostSingleAuction(ModuleState const &ctx,
                                             AuctionHouseId house,
                                             uint32 itemId, uint32 count,
                                             uint32 startBid, uint32 buyout,
                                             uint32 durationSeconds,
                                             ChatHandler *handler)
    {
        ObjectGuid owner = OwnerGuidFor(ctx, house);
        if (!owner)
        {
            if (handler)
                handler->PSendSysMessage("ModDynamicAH: no seller GUID configured; run `.dah setup`.");
            return false;
        }

        Item *item = Item::CreateItem(itemId, count, nullptr);
        if (!item)
        {
            if (handler)
                handler->PSendSysMessage("ModDynamicAH: could not create item {}", itemId);
            return false;
        }
        item->SetOwnerGUID(owner);
        item->SetGuidValue(ITEM_FIELD_CONTAINED, owner);

        AuctionHouseEntry const *ahEntry = AuctionHouseMgr::GetAuctionHouseEntryFromHouse(house);
        if (!ahEntry)
        {
            if (handler)
                handler->PSendSysMessage("ModDynamicAH: invalid auction house entry");
            delete item;
            return false;
        }

        uint32 deposit = AuctionHouseMgr::GetAuctionDeposit(ahEntry, durationSeconds, item, count);

        AuctionEntry *AH = new AuctionEntry;
        AH->Id = sObjectMgr->GenerateAuctionID();
        AH->houseId = house;
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

        AuctionHouseObject *auctionHouse = sAuctionMgr->GetAuctionsMapByHouseId(house);
        sAuctionMgr->AddAItem(item);
        auctionHouse->AddAuction(AH);

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        item->SaveToDB(trans);
        AH->SaveToDB(trans);
        CharacterDatabase.CommitTransaction(trans);

        if (handler)
            handler->PSendSysMessage("Posted item {} x{} id={} start={} buyout={} dur={}s house={}",
                                     itemId, count, AH->Id, startBid, buyout, durationSeconds, (uint32)house);
        return true;
    }

    void DynamicAHPosting::ApplyPlanOnWorld(ModuleState &s, uint32 maxToApply, ChatHandler *handler)
    {
        auto batch = s.postQueue.Drain(maxToApply);
        if (batch.empty())
            return;

        for (auto const &r : batch)
        {
            // If you support dry-run, you can short-circuit here and just log.
            if (!s.dryRun)
                PostSingleAuction(s, r.house, r.itemId, r.count, r.startBid, r.buyout, r.duration, handler);
        }
    }
} // namespace ModDynamicAH
