/*
 * Copyright (C) 2008-2010 Trinity <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "ObjectMgr.h"
#include "AuctionHouseMgr.h"
#include "Config.h"
#include "Player.h"
#include "WorldSession.h"
#include "GameTime.h"
#include "DatabaseEnv.h"

#include "AuctionHouseBot.h"
#include "AuctionHouseBotCommon.h"

#include <numeric>

using namespace std;

AuctionHouseBot::AuctionHouseBot(uint32 account, uint32 id)
{
    _account = account;
    _id = id;

    _lastrun_a_sec = time(NULL);
    _lastrun_h_sec = time(NULL);
    _lastrun_n_sec = time(NULL);

    _allianceConfig = NULL;
    _hordeConfig = NULL;
    _neutralConfig = NULL;
}

AuctionHouseBot::~AuctionHouseBot()
{
    // Nothing
}

uint32 AuctionHouseBot::getRandomItemId(std::set<uint32> itemSet, std::map<uint32, uint32> &templateIDToAuctionCount, AHBConfig *config)
{
    if (itemSet.empty())
        throw std::runtime_error("Item set is empty.");

    uint32 itemId;
    int attempts = 0;
    const int maxAttempts = 10;

    do {
        // Generate a random index
        int randomIndex = urand(0, itemSet.size() - 1);
        auto it = itemSet.begin();
        std::advance(it, randomIndex);
        itemId = *it;

        // Only check auction count if DuplicatesCount is not zero
        if (config->DuplicatesCount == 0)
            break;

        // Check the auction count for the selected item
        uint32 auctionCount = templateIDToAuctionCount.contains(itemId) ? templateIDToAuctionCount.at(itemId) : 0;

        // If the auction count is within the allowed duplicates, break the loop
        if (auctionCount <= config->DuplicatesCount)
            break;

        // Prevent infinite looping by limiting attempts
        attempts++;
    } while (attempts < maxAttempts);

    return itemId;
}

void AuctionHouseBot::registerAuctionItemID(uint32 itemID, std::map<uint32, uint32> &templateIDToAuctionCount) {
    if (!templateIDToAuctionCount.contains(itemID)) {
        templateIDToAuctionCount.emplace(itemID, 1);
    } else {
        templateIDToAuctionCount[itemID]++;
    }
}

uint32 AuctionHouseBot::getStackCount(AHBConfig *config, uint32 max)
{
    if (max == 1)
    {
        return 1;
    }

    //
    // Organize the stacks in a pseudo random way
    //

    if (config->DivisibleStacks)
    {
        uint32 ret = 0;

        if (max % 5 == 0) // 5, 10, 15, 20
        {
            ret = urand(1, 4) * 5;
        }

        if (max % 4 == 0) // 4, 8, 12, 16
        {
            ret = urand(1, 4) * 4;
        }

        if (max % 3 == 0) // 3, 6, 9, 18
        {
            ret = urand(1, 3) * 3;
        }

        if (ret > max)
        {
            ret = max;
        }

        return ret;
    }

    //
    // More likely to be a whole stack.
    //
    bool wholeStack = frand(0, 1) > 0.35; // TODO: Get this from config
    return wholeStack ? max : urand(1, max);
}

uint32 AuctionHouseBot::getElapsedTime(uint32 timeClass)
{
    switch (timeClass)
    {
    case 2:
        return urand(1, 5) * 600; // SHORT = In the range of one hour

    case 1:
        return urand(1, 23) * 3600; // MEDIUM = In the range of one day

    default:
        return urand(1, 3) * 86400; // LONG = More than one day but less than three
    }
}

uint32 AuctionHouseBot::getAuctionCount(AHBConfig *config, AuctionHouseObject *auctionHouse, ObjectGuid guid)
{
    //
    // All the auctions
    //

    if (!config->ConsiderOnlyBotAuctions)
    {
        return auctionHouse->Getcount();
    }

    //
    // Just the one handled by the bot
    //

    uint32 count = 0;

    for (AuctionHouseObject::AuctionEntryMap::const_iterator itr = auctionHouse->GetAuctionsBegin(); itr != auctionHouse->GetAuctionsEnd(); ++itr)
    {
        AuctionEntry *Aentry = itr->second;

        if (guid == Aentry->owner)
        {
            count++;
            break;
        }
    }

    return count;
}

// =============================================================================
// This routine performs the bidding/buyout operations for the bot.
// =============================================================================

void AuctionHouseBot::Buy(Player *AHBplayer, AHBConfig *config, WorldSession *session)
{
    //
    // Check if disabled.
    //

    if (!config->AHBBuyer)
    {
        return;
    }

    //
    // Retrieve items not owned and not sold/bidded on by the bot.
    //

    QueryResult result = CharacterDatabase.Query("SELECT id FROM auctionhouse WHERE itemowner<>{} AND buyguid<>{}", _id, _id);

    if (!result || result->GetRowCount() == 0)
    {
        return;
    }

    //
    // List all existing auctions in AHs enabled in config.
    //

    AuctionHouseObject *auctionHouse = sAuctionMgr->GetAuctionsMap(config->GetAHFID());
    std::set<uint32> auctionPool;

    do
    {
        uint32 auctionGuid = result->Fetch()->Get<uint32>();
        auctionPool.insert(auctionGuid);
    } while (result->NextRow());

    if (auctionPool.empty())
    {
        if (config->DebugOutBuyer)
        {
            LOG_INFO("module", "AHBot [{}]: no existing auctions found.", _id);
        }

        return;
    }

    //
    // Perform the operation for a maximum amount of bid attempts (defined in config).
    //

    for (uint32 count = 0; count < config->GetBidsPerInterval(); ++count)
    {
        //
        // Pick an auction from the pool randomly.
        //

        uint32 randBid = urand(0, auctionPool.size() - 1);

        std::set<uint32>::iterator it = auctionPool.begin();
        std::advance(it, randBid);

        AuctionEntry *auction = auctionHouse->GetAuction(*it);

        auctionPool.erase(it); // don't bid on the same auction twice

        if (!auction)
        {
            continue;
        }

        //
        // Do not bid on auctions created by bots.
        //

        if (gBotsId.find(auction->owner.GetCounter()) != gBotsId.end())
        {
            continue;
        }

        //
        // Get item information and exclude items with a too high quality.
        //

        Item *pItem = sAuctionMgr->GetAItem(auction->item_guid);

        if (!pItem)
        {
            if (config->DebugOutBuyer)
            {
                LOG_ERROR("module", "AHBot [{}]: item {} doesn't exist, perhaps bought already?", _id, auction->item_guid.ToString());
            }

            continue;
        }

        ItemTemplate const *prototype = sObjectMgr->GetItemTemplate(auction->item_template);

        if (prototype->Quality > AHB_MAX_QUALITY)
        {
            if (config->DebugOutBuyer)
            {
                LOG_INFO("module", "AHBot [{}]: Quality {} not supported.", _id, prototype->Quality);
            }

            continue;
        }

        //
        // Determine current price.
        //

        uint32 currentPrice = auction->bid ? auction->bid : auction->startbid;

        //
        // Determine maximum bid and skip auctions with too high a currentPrice.
        //

        double basePrice = config->UseBuyPrice ? prototype->BuyPrice : prototype->SellPrice;
        double maximumBid = basePrice * pItem->GetCount() * config->GetBuyerPrice(prototype->Quality);

        if (currentPrice > maximumBid)
        {
            if (config->DebugOutBuyer)
            {
                LOG_INFO("module", "AHBot [{}]: Current price too high, skipped.", _id);
            }

            continue;
        }

        //
        // Specific item class maximum bid adjustments.
        //

        switch (prototype->Class)
        {
            // TODO: Add balancing rules for items such as glyphs here.
        default:
            break;
        }

        //
        // Make sure to skip the auction if maximum bid is 0.
        //

        if (maximumBid == 0)
        {
            continue;
        }

        //
        // Determine current price.
        //

        uint32 currentPrice = auction->bid ? auction->bid : auction->startbid;

        //
        // Determine maximum bid and skip auctions with too high a currentPrice.
        //

        double basePrice = config->UseBuyPriceForBuyer ? prototype->BuyPrice : prototype->SellPrice;
        double maximumBid = basePrice * pItem->GetCount() * config->GetBuyerPrice(prototype->Quality);

        if (config->DebugOutBuyer)
        {
            LOG_INFO("module", "-------------------------------------------------");
            LOG_INFO("module", "AHBot [{}]: Info for Auction #{}:", _id, auction->Id);
            LOG_INFO("module", "AHBot [{}]: AuctionHouse: {}", _id, auction->GetHouseId());
            LOG_INFO("module", "AHBot [{}]: Owner: {}", _id, auction->owner.ToString());
            LOG_INFO("module", "AHBot [{}]: Bidder: {}", _id, auction->bidder.ToString());
            LOG_INFO("module", "AHBot [{}]: Starting Bid: {}", _id, auction->startbid);
            LOG_INFO("module", "AHBot [{}]: Current Bid: {}", _id, currentPrice);
            LOG_INFO("module", "AHBot [{}]: Buyout: {}", _id, auction->buyout);
            LOG_INFO("module", "AHBot [{}]: Deposit: {}", _id, auction->deposit);
            LOG_INFO("module", "AHBot [{}]: Expire Time: {}", _id, uint32(auction->expire_time));
            LOG_INFO("module", "AHBot [{}]: Bid Max: {}", _id, maximumBid);
            LOG_INFO("module", "AHBot [{}]: Item GUID: {}", _id, auction->item_guid.ToString());
            LOG_INFO("module", "AHBot [{}]: Item Template: {}", _id, auction->item_template);
            LOG_INFO("module", "AHBot [{}]: Item ID: {}", _id, prototype->ItemId);
            LOG_INFO("module", "AHBot [{}]: Buy Price: {}", _id, prototype->BuyPrice);
            LOG_INFO("module", "AHBot [{}]: Sell Price: {}", _id, prototype->SellPrice);
            LOG_INFO("module", "AHBot [{}]: Bonding: {}", _id, prototype->Bonding);
            LOG_INFO("module", "AHBot [{}]: Quality: {}", _id, prototype->Quality);
            LOG_INFO("module", "AHBot [{}]: Item Level: {}", _id, prototype->ItemLevel);
            LOG_INFO("module", "AHBot [{}]: Ammo Type: {}", _id, prototype->AmmoType);
            LOG_INFO("module", "-------------------------------------------------");
        }

        if (currentPrice > maximumBid)
        {
            if (config->DebugOutBuyer)
            {
                LOG_INFO("module", "AHBot [{}]: Current price too high, skipped.", _id);
            }

            continue;
        }

        //
        // Specific item class maximum bid adjustments.
        //

        switch (prototype->Class)
        {
            // TODO: Add balancing rules for items such as glyphs here.
        default:
            break;
        }

        //
        // Make sure to skip the auction if maximum bid is 0.
        //

        if (maximumBid == 0)
        {
            continue;
        }

        //
        // Calculate our bid.
        //

        double bidRate = static_cast<double>(urand(1, 100)) / 100;
        double bidValue = currentPrice + ((maximumBid - currentPrice) * bidRate);
        uint32 bidPrice = static_cast<uint32>(bidValue);

        //
        // Check our bid is high enough to be valid. If not, correct it to minimum.
        //

        uint32 minimumOutbid = auction->GetAuctionOutBid();
        if ((currentPrice + minimumOutbid) > bidPrice)
        {
            bidPrice = currentPrice + minimumOutbid;
        }

        //
        // Print out debug info.
        //

        if (config->DebugOutBuyer)
        {
            LOG_INFO("module", "-------------------------------------------------");
            LOG_INFO("module", "AHBot [{}]: Bid Rate: {}", _id, bidRate);
            LOG_INFO("module", "AHBot [{}]: Bid Value: {}", _id, bidValue);
            LOG_INFO("module", "AHBot [{}]: Bid Price: {}", _id, bidPrice);
            LOG_INFO("module", "AHBot [{}]: Minimum Outbid: {}", _id, minimumOutbid);
            LOG_INFO("module", "-------------------------------------------------");
        }

        //
        // Check whether we bid or buyout.
        //

        if ((bidPrice < auction->buyout) || (auction->buyout == 0)) // BID
        {

            if (auction->bidder)
            {
                if (auction->bidder != AHBplayer->GetGUID())
                {
                    //
                    // Return money to last bidder.
                    //

                    auto trans = CharacterDatabase.BeginTransaction();
                    sAuctionMgr->SendAuctionOutbiddedMail(auction, bidPrice, session->GetPlayer(), trans);
                    CharacterDatabase.CommitTransaction(trans);
                }
            }

            auction->bidder = AHBplayer->GetGUID();
            auction->bid = bidPrice;

            //
            // Persist auction in database.
            //

            CharacterDatabase.Execute("UPDATE auctionhouse SET buyguid = '{}', lastbid = '{}' WHERE id = '{}'", auction->bidder.GetCounter(), auction->bid, auction->Id);

            if (config->TraceBuyer)
            {
                LOG_INFO("module", "AHBot [{}]: New bid, id={}, ah={}, item={}, start={}, current={}, buyout={}", _id, prototype->ItemId, auction->GetHouseId(), auction->item_template, auction->startbid, currentPrice, auction->buyout);
            }
        }
        else // BUYOUT
        {
            auto trans = CharacterDatabase.BeginTransaction();

            if ((auction->bidder) && (AHBplayer->GetGUID() != auction->bidder))
            {
                //
                // Return money to last bidder.
                //

                sAuctionMgr->SendAuctionOutbiddedMail(auction, auction->buyout, session->GetPlayer(), trans);
            }

            auction->bidder = AHBplayer->GetGUID();
            auction->bid = auction->buyout;

            //
            // Send mails to buyer & seller.
            //

            sAuctionMgr->SendAuctionSuccessfulMail(auction, trans);
            sAuctionMgr->SendAuctionWonMail(auction, trans);

            //
            // Delete the auction.
            //

            auction->DeleteFromDB(trans);

            sAuctionMgr->RemoveAItem(auction->item_guid);
            auctionHouse->RemoveAuction(auction);

            CharacterDatabase.CommitTransaction(trans);

            if (config->TraceBuyer)
            {
                LOG_INFO("module", "AHBot [{}]: Bought , id={}, ah={}, item={}, start={}, current={}, buyout={}", _id, prototype->ItemId, auction->GetHouseId(), auction->item_template, auction->startbid, currentPrice, auction->buyout);
            }
        }
    }
}

uint32 selectRandomOutcome(const std::vector<uint32>& outcomes, const std::vector<uint32>& weights) {
    if (outcomes.size() != weights.size() || outcomes.empty()) {
        throw std::invalid_argument("Outcomes and weights must have the same non-zero size");
    }

    // Compute cumulative weights
    std::vector<uint32> cumulativeWeights(weights.size());
    std::partial_sum(weights.begin(), weights.end(), cumulativeWeights.begin());

    // Generate a random number in the range [0, totalWeight[
    int randomValue = urand(0, cumulativeWeights.back() - 1);

    // Find the index where the randomValue fits in cumulativeWeights
    auto it = std::upper_bound(cumulativeWeights.begin(), cumulativeWeights.end(), randomValue);
    size_t index = std::distance(cumulativeWeights.begin(), it);

    return outcomes[index];
}


// =============================================================================
// This routine performs the selling operations for the bot
// =============================================================================

void AuctionHouseBot::Sell(Player *AHBplayer, AHBConfig *config)
{
    //
    // Check if disabled
    //

    if (!config->AHBSeller)
    {
        return;
    }

    //
    // Check the given limits
    //

    uint32 minAuctionCount = config->GetMinItems();
    uint32 maxAuctionCount = config->GetMaxItems();

    if (maxAuctionCount == 0)
    {
        return;
    }

    //
    // Retrieve the auction house situation
    //

    AuctionHouseEntry const *ahEntry = sAuctionMgr->GetAuctionHouseEntry(config->GetAHFID());

    if (!ahEntry)
    {
        return;
    }

    AuctionHouseObject *auctionHouse = sAuctionMgr->GetAuctionsMap(config->GetAHFID());

    if (!auctionHouse)
    {
        return;
    }

    auctionHouse->Update();

    //
    // Check if we are clear to proceed
    //

    bool aboveMin = false;
    bool aboveMax = false;
    uint32 currentAuctionCount = getAuctionCount(config, auctionHouse, AHBplayer->GetGUID());
    uint32 newAuctionsCount = 0;

    if (currentAuctionCount >= minAuctionCount)
    {
        aboveMin = true;

        if (config->DebugOutSeller)
        {
            LOG_INFO("module", "AHBot [{}]: Auctions above minimum", _id);
        }

        return;
    }

    if (currentAuctionCount >= maxAuctionCount)
    {
        aboveMax = true;

        if (config->DebugOutSeller)
        {
            LOG_INFO("module", "AHBot [{}]: Auctions at or above maximum", _id);
        }

        return;
    }

    if ((maxAuctionCount - currentAuctionCount) >= config->ItemsPerCycle)
    {
        newAuctionsCount = config->ItemsPerCycle;
    }
    else
    {
        newAuctionsCount = (maxAuctionCount - currentAuctionCount);
    }

    //
    // Retrieve the configuration for this run
    //

    struct ItemCounts {
        uint32 CurrentCount;
        uint32 MaxCount;
    };

    static const std::vector<uint32> itemTypes = {
        AHB_GREY_TG, AHB_WHITE_TG, AHB_GREEN_TG, AHB_BLUE_TG, AHB_PURPLE_TG, AHB_ORANGE_TG, AHB_YELLOW_TG, 
        AHB_GREY_I, AHB_WHITE_I, AHB_GREEN_I, AHB_BLUE_I, AHB_PURPLE_I, AHB_ORANGE_I, AHB_YELLOW_I
    }; // index == value

    std::map<uint32, ItemCounts> itemCountsMap;
    std::vector<uint32> missingCounts(itemTypes.size());

    for (size_t i = 0; i < itemTypes.size(); i++) {
        uint32 type = itemTypes[i];
        ItemCounts counts = {
            config->GetItemCounts(type),
            config->GetMaximum(type)
        };

        itemCountsMap[type] = counts;
        missingCounts[i] = config->GetBin(type).size() == 0 ? 0 : counts.MaxCount - counts.CurrentCount;
    }

    if (config->DebugOutSeller)
    {
        std::ostringstream oss;
        for (size_t i = 0; i < missingCounts.size(); ++i) {
            oss << missingCounts[i];
            if (i != missingCounts.size() - 1) {
                oss << ",";
            }
        }
        LOG_DEBUG("module", "AHBot [{}]: Will now randomly create auction items for the following respective missing counts for categories: {}", _id, oss.str());
    }

    //
    // Duplicates handling if relevant
    //
    map<uint32, uint32> templateIDToAuctionCount;
    if (config->DuplicatesCount) {
        for (AuctionHouseObject::AuctionEntryMap::const_iterator itr = auctionHouse->GetAuctionsBegin(); itr != auctionHouse->GetAuctionsEnd(); ++itr)
        {
            AuctionEntry *entry = itr->second;
            registerAuctionItemID(entry->item_template, templateIDToAuctionCount);
        }
    }

    //
    // Loop variables
    //

    uint32 noSold = 0;   // Tracing counter
    uint32 err = 0;      // Tracing counter

    for (uint32 i = 0; i < newAuctionsCount; i++)
    {
        //
        // Make sure at least one item can be added.
        //

        bool allZeroCounts = std::all_of(missingCounts.begin(), missingCounts.end(), [](int count) { return count == 0; });
        if (allZeroCounts) {
            if (config->DebugOutSeller) {
                LOG_INFO("module", "AHBot [{}]: No item bin could be selected: all missing counts are zero.", _id);
            }

            break;
        }

        //
        // Select an item bin according to weights.
        //

        uint32 selectedType = selectRandomOutcome(itemTypes, missingCounts);
        LootIdSet selectedBin = config->GetBin(selectedType);
        uint32 itemID = getRandomItemId(selectedBin, templateIDToAuctionCount, config);

        if (itemID == 0)
        {
            if (config->DebugOutSeller)
            {
                LOG_INFO("module", "AHBot [{}]: No item could be selected from the bins", _id);
            }

            continue;
        }

        missingCounts[selectedType]--;

        //
        // Retrieve information about the selected item
        //

        ItemTemplate const *prototype = sObjectMgr->GetItemTemplate(itemID);

        if (prototype == NULL)
        {
            err++;

            if (config->DebugOutSeller)
            {
                LOG_INFO("module", "AHBot [{}]: could not get prototype of item {}", _id, itemID);
            }

            continue;
        }

        Item *item = Item::CreateItem(itemID, 1, AHBplayer);

        if (item == NULL)
        {
            err++;

            if (config->DebugOutSeller)
            {
                LOG_INFO("module", "AHBot [{}]: could not create item from prototype {}", _id, itemID);
            }

            continue;
        }

        //
        // Start interacting with the item by adding a random property
        //

        item->AddToUpdateQueueOf(AHBplayer);

        uint32 randomPropertyId = Item::GenerateItemRandomPropertyId(itemID);

        if (randomPropertyId != 0)
        {
            item->SetItemRandomProperties(randomPropertyId);
        }

        if (prototype->Quality > AHB_MAX_QUALITY)
        {
            err++;

            if (config->DebugOutSeller)
            {
                LOG_INFO("module", "AHBot [{}]: Quality {} TOO HIGH for item {}", _id, prototype->Quality, itemID);
            }

            item->RemoveFromUpdateQueueOf(AHBplayer);
            continue;
        }

        //
        // Determine the price
        //

        uint64 buyoutPrice = 0;
        uint64 bidPrice = 0;
        uint32 stackCount = 1;

        if (config->SellAtMarketPrice)
        {
            buyoutPrice = config->GetItemPrice(itemID);
        }

        if (buyoutPrice == 0)
        {
            if (config->UseBuyPriceForSeller)
            {
                buyoutPrice = prototype->BuyPrice;
            }
            else
            {
                buyoutPrice = prototype->SellPrice;
            }
        }

        buyoutPrice = buyoutPrice * urand(config->GetMinPrice(prototype->Quality), config->GetMaxPrice(prototype->Quality));
        buyoutPrice = buyoutPrice / 100;

        bidPrice = buyoutPrice * urand(config->GetMinBidPrice(prototype->Quality), config->GetMaxBidPrice(prototype->Quality));
        bidPrice = bidPrice / 100;

        //
        // Determine the stack size
        //

        if (config->GetMaxStack(prototype->Quality) > 1 && item->GetMaxStackCount() > 1)
        {
            stackCount = minValue(getStackCount(config, item->GetMaxStackCount()), config->GetMaxStack(prototype->Quality));
        }
        else if (config->GetMaxStack(prototype->Quality) == 0 && item->GetMaxStackCount() > 1)
        {
            stackCount = getStackCount(config, item->GetMaxStackCount());
        }
        else
        {
            stackCount = 1;
        }

        item->SetCount(stackCount);

        //
        // Determine the auction time
        //

        uint32 etime = getElapsedTime(config->ElapsingTimeClass);

        //
        // Determine the deposit
        //

        uint32 dep = sAuctionMgr->GetAuctionDeposit(ahEntry, etime, item, stackCount);

        //
        // Perform the auction
        //

        auto trans = CharacterDatabase.BeginTransaction();

        AuctionEntry *auctionEntry = new AuctionEntry();
        auctionEntry->Id = sObjectMgr->GenerateAuctionID();
        auctionEntry->houseId = config->GetAHID();
        auctionEntry->item_guid = item->GetGUID();
        auctionEntry->item_template = item->GetEntry();
        auctionEntry->itemCount = item->GetCount();
        auctionEntry->owner = AHBplayer->GetGUID();
        auctionEntry->startbid = bidPrice * stackCount;
        auctionEntry->buyout = buyoutPrice * stackCount;
        auctionEntry->bid = 0;
        auctionEntry->deposit = dep;
        auctionEntry->expire_time = (time_t)etime + time(NULL);
        auctionEntry->auctionHouseEntry = ahEntry;

        item->SaveToDB(trans);
        item->RemoveFromUpdateQueueOf(AHBplayer);
        sAuctionMgr->AddAItem(item);
        auctionHouse->AddAuction(auctionEntry);
        auctionEntry->SaveToDB(trans);
        registerAuctionItemID(auctionEntry->item_template, templateIDToAuctionCount);

        CharacterDatabase.CommitTransaction(trans);

        noSold++;

        if (config->TraceSeller)
        {
            LOG_INFO("module", "AHBot [{}]: New stack ah={}, id={}, stack={}, bid={}, buyout={}", _id, config->GetAHID(), itemID, stackCount, auctionEntry->startbid, auctionEntry->buyout);
        }
    }

    if (config->TraceSeller)
    {
        LOG_INFO("module", "AHBot [{}]: auctionhouse {}, req={}, sold={}, aboveMin={}, aboveMax={}, err={}", _id, config->GetAHID(), newAuctionsCount, noSold, aboveMin, aboveMax, err);
    }
}

// =============================================================================
// Perform an update cycle
// =============================================================================

void AuctionHouseBot::Update()
{
    time_t _newrun = time(NULL);

    //
    // If no configuration is associated, then stop here
    //

    if (!_allianceConfig && !_hordeConfig && !_neutralConfig)
    {
        return;
    }

    //
    // Preprare for operation
    //

    std::string accountName = "AuctionHouseBot" + std::to_string(_account);

    WorldSession _session(_account, std::move(accountName), nullptr, SEC_PLAYER, sWorld->getIntConfig(CONFIG_EXPANSION), 0, LOCALE_enUS, 0, false, false, 0);

    Player _AHBplayer(&_session);
    _AHBplayer.Initialize(_id);

    ObjectAccessor::AddObject(&_AHBplayer);

    //
    // Perform update for the factions markets
    //

    if (!sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION))
    {
        //
        // Alliance
        //

        if (_allianceConfig)
        {
            Sell(&_AHBplayer, _allianceConfig);

            if (((_newrun - _lastrun_a_sec) >= (_allianceConfig->GetBiddingInterval() * MINUTE)) && (_allianceConfig->GetBidsPerInterval() > 0))
            {
                Buy(&_AHBplayer, _allianceConfig, &_session);
                _lastrun_a_sec = _newrun;
            }
        }

        //
        // Horde
        //

        if (_hordeConfig)
        {
            Sell(&_AHBplayer, _hordeConfig);

            if (((_newrun - _lastrun_h_sec) >= (_hordeConfig->GetBiddingInterval() * MINUTE)) && (_hordeConfig->GetBidsPerInterval() > 0))
            {
                Buy(&_AHBplayer, _hordeConfig, &_session);
                _lastrun_h_sec = _newrun;
            }
        }
    }

    //
    // Neutral
    //

    if (_neutralConfig)
    {
        Sell(&_AHBplayer, _neutralConfig);

        if (((_newrun - _lastrun_n_sec) >= (_neutralConfig->GetBiddingInterval() * MINUTE)) && (_neutralConfig->GetBidsPerInterval() > 0))
        {
            Buy(&_AHBplayer, _neutralConfig, &_session);
            _lastrun_n_sec = _newrun;
        }
    }

    ObjectAccessor::RemoveObject(&_AHBplayer);
}

// =============================================================================
// Execute commands coming from the console
// =============================================================================

void AuctionHouseBot::Commands(AHBotCommand command, uint32 ahMapID, uint32 col, char *args)
{
    //
    // Retrieve the auction house configuration
    //

    AHBConfig *config = NULL;

    switch (ahMapID)
    {
    case 2:
        config = _allianceConfig;
        break;
    case 6:
        config = _hordeConfig;
        break;
    default:
        config = _neutralConfig;
        break;
    }

    //
    // Retrive the item quality
    //

    std::string color;

    switch (col)
    {
    case AHB_GREY:
        color = "grey";
        break;
    case AHB_WHITE:
        color = "white";
        break;
    case AHB_GREEN:
        color = "green";
        break;
    case AHB_BLUE:
        color = "blue";
        break;
    case AHB_PURPLE:
        color = "purple";
        break;
    case AHB_ORANGE:
        color = "orange";
        break;
    case AHB_YELLOW:
        color = "yellow";
        break;
    default:
        break;
    }

    //
    // Perform the command
    //

    switch (command)
    {
    case AHBotCommand::buyer:
    {
        char *param1 = strtok(args, " ");
        uint32 state = (uint32)strtoul(param1, NULL, 0);

        if (state == 0)
        {
            _allianceConfig->AHBBuyer = false;
            _hordeConfig->AHBBuyer = false;
            _neutralConfig->AHBBuyer = false;
        }
        else
        {
            _allianceConfig->AHBBuyer = true;
            _hordeConfig->AHBBuyer = true;
            _neutralConfig->AHBBuyer = true;
        }

        break;
    }
    case AHBotCommand::seller:
    {
        char *param1 = strtok(args, " ");
        uint32 state = (uint32)strtoul(param1, NULL, 0);

        if (state == 0)
        {
            _allianceConfig->AHBSeller = false;
            _hordeConfig->AHBSeller = false;
            _neutralConfig->AHBSeller = false;
        }
        else
        {
            _allianceConfig->AHBSeller = true;
            _hordeConfig->AHBSeller = true;
            _neutralConfig->AHBSeller = true;
        }

        break;
    }
    case AHBotCommand::useMarketPrice:
    {
        char *param1 = strtok(args, " ");
        uint32 state = (uint32)strtoul(param1, NULL, 0);

        if (state == 0)
        {
            _allianceConfig->SellAtMarketPrice = false;
            _hordeConfig->SellAtMarketPrice = false;
            _neutralConfig->SellAtMarketPrice = false;
        }
        else
        {
            _allianceConfig->SellAtMarketPrice = true;
            _hordeConfig->SellAtMarketPrice = true;
            _neutralConfig->SellAtMarketPrice = true;
        }

        break;
    }
    case AHBotCommand::ahexpire:
    {
        AuctionHouseObject *auctionHouse = sAuctionMgr->GetAuctionsMap(config->GetAHFID());

        AuctionHouseObject::AuctionEntryMap::iterator itr;
        itr = auctionHouse->GetAuctionsBegin();

        //
        // Iterate through all the autions and if they belong to the bot, make them expired
        //

        while (itr != auctionHouse->GetAuctionsEnd())
        {
            if (itr->second->owner.GetCounter() == _id)
            {
                // Expired NOW.
                itr->second->expire_time = GameTime::GetGameTime().count();

                uint32 id = itr->second->Id;
                uint32 expire_time = itr->second->expire_time;

                CharacterDatabase.Execute("UPDATE auctionhouse SET time = '{}' WHERE id = '{}'", expire_time, id);
            }

            ++itr;
        }

        break;
    }
    case AHBotCommand::minitems:
    {
        char *param1 = strtok(args, " ");
        uint32 minItems = (uint32)strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET minitems = '{}' WHERE auctionhouse = '{}'", minItems, ahMapID);

        config->SetMinItems(minItems);

        break;
    }
    case AHBotCommand::maxitems:
    {
        char *param1 = strtok(args, " ");
        uint32 maxItems = (uint32)strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET maxitems = '{}' WHERE auctionhouse = '{}'", maxItems, ahMapID);

        config->SetMaxItems(maxItems);
        config->CalculatePercents();
        break;
    }
    case AHBotCommand::percentages:
    {
        char *param1 = strtok(args, " ");
        char *param2 = strtok(NULL, " ");
        char *param3 = strtok(NULL, " ");
        char *param4 = strtok(NULL, " ");
        char *param5 = strtok(NULL, " ");
        char *param6 = strtok(NULL, " ");
        char *param7 = strtok(NULL, " ");
        char *param8 = strtok(NULL, " ");
        char *param9 = strtok(NULL, " ");
        char *param10 = strtok(NULL, " ");
        char *param11 = strtok(NULL, " ");
        char *param12 = strtok(NULL, " ");
        char *param13 = strtok(NULL, " ");
        char *param14 = strtok(NULL, " ");

        uint32 greytg = (uint32)strtoul(param1, NULL, 0);
        uint32 whitetg = (uint32)strtoul(param2, NULL, 0);
        uint32 greentg = (uint32)strtoul(param3, NULL, 0);
        uint32 bluetg = (uint32)strtoul(param4, NULL, 0);
        uint32 purpletg = (uint32)strtoul(param5, NULL, 0);
        uint32 orangetg = (uint32)strtoul(param6, NULL, 0);
        uint32 yellowtg = (uint32)strtoul(param7, NULL, 0);
        uint32 greyi = (uint32)strtoul(param8, NULL, 0);
        uint32 whitei = (uint32)strtoul(param9, NULL, 0);
        uint32 greeni = (uint32)strtoul(param10, NULL, 0);
        uint32 bluei = (uint32)strtoul(param11, NULL, 0);
        uint32 purplei = (uint32)strtoul(param12, NULL, 0);
        uint32 orangei = (uint32)strtoul(param13, NULL, 0);
        uint32 yellowi = (uint32)strtoul(param14, NULL, 0);

        //
        // Setup the percentage in the configuration first, so validity test can be performed
        //

        config->SetPercentages(greytg, whitetg, greentg, bluetg, purpletg, orangetg, yellowtg, greyi, whitei, greeni, bluei, purplei, orangei, yellowi);

        //
        // Save the results into the database (after the tests)
        //

        auto trans = WorldDatabase.BeginTransaction();

        trans->Append("UPDATE mod_auctionhousebot SET percentgreytradegoods   = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_GREY_TG), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentwhitetradegoods  = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_WHITE_TG), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentgreentradegoods  = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_GREEN_TG), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentbluetradegoods   = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_BLUE_TG), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentpurpletradegoods = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_PURPLE_TG), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentorangetradegoods = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_ORANGE_TG), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentyellowtradegoods = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_YELLOW_TG), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentgreyitems        = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_GREY_I), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentwhiteitems       = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_WHITE_I), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentgreenitems       = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_GREEN_I), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentblueitems        = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_BLUE_I), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentpurpleitems      = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_PURPLE_I), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentorangeitems      = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_ORANGE_I), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentyellowitems      = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_YELLOW_I), ahMapID);

        WorldDatabase.CommitTransaction(trans);

        break;
    }
    case AHBotCommand::minprice:
    {
        char *param1 = strtok(args, " ");
        uint32 minPrice = (uint32)strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET minprice{} = '{}' WHERE auctionhouse = '{}'", color, minPrice, ahMapID);

        config->SetMinPrice(col, minPrice);

        break;
    }
    case AHBotCommand::maxprice:
    {
        char *param1 = strtok(args, " ");
        uint32 maxPrice = (uint32)strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET maxprice{} = '{}' WHERE auctionhouse = '{}'", color, maxPrice, ahMapID);

        config->SetMaxPrice(col, maxPrice);

        break;
    }
    case AHBotCommand::minbidprice:
    {
        char *param1 = strtok(args, " ");
        uint32 minBidPrice = (uint32)strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET minbidprice{} = '{}' WHERE auctionhouse = '{}'", color, minBidPrice, ahMapID);

        config->SetMinBidPrice(col, minBidPrice);

        break;
    }
    case AHBotCommand::maxbidprice:
    {
        char *param1 = strtok(args, " ");
        uint32 maxBidPrice = (uint32)strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET maxbidprice{} = '{}' WHERE auctionhouse = '{}'", color, maxBidPrice, ahMapID);

        config->SetMaxBidPrice(col, maxBidPrice);

        break;
    }
    case AHBotCommand::maxstack:
    {
        char *param1 = strtok(args, " ");
        uint32 maxStack = (uint32)strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET maxstack{} = '{}' WHERE auctionhouse = '{}'", color, maxStack, ahMapID);

        config->SetMaxStack(col, maxStack);

        break;
    }
    case AHBotCommand::buyerprice:
    {
        char *param1 = strtok(args, " ");
        uint32 buyerPrice = (uint32)strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET buyerprice{} = '{}' WHERE auctionhouse = '{}'", color, buyerPrice, ahMapID);

        config->SetBuyerPrice(col, buyerPrice);

        break;
    }
    case AHBotCommand::bidinterval:
    {
        char *param1 = strtok(args, " ");
        uint32 bidInterval = (uint32)strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET buyerbiddinginterval = '{}' WHERE auctionhouse = '{}'", bidInterval, ahMapID);

        config->SetBiddingInterval(bidInterval);

        break;
    }
    case AHBotCommand::bidsperinterval:
    {
        char *param1 = strtok(args, " ");
        uint32 bidsPerInterval = (uint32)strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET buyerbidsperinterval = '{}' WHERE auctionhouse = '{}'", bidsPerInterval, ahMapID);

        config->SetBidsPerInterval(bidsPerInterval);

        break;
    }
    default:
        break;
    }
}

// =============================================================================
// Initialization of the bot
// =============================================================================

void AuctionHouseBot::Initialize(AHBConfig *allianceConfig, AHBConfig *hordeConfig, AHBConfig *neutralConfig)
{
    //
    // Save the pointer for the configurations
    //

    _allianceConfig = allianceConfig;
    _hordeConfig = hordeConfig;
    _neutralConfig = neutralConfig;

    //
    // Done
    //

    LOG_INFO("module", "AHBot [{}]: initialization complete", uint32(_id));
}
