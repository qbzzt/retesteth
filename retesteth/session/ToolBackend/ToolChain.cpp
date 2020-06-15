#include "ToolChainManager.h"
#include <libdevcore/CommonIO.h>
#include <retesteth/Options.h>
#include <retesteth/dataObject/ConvertFile.h>
#include <retesteth/testStructures/Common.h>
#include <retesteth/testStructures/types/BlockchainTests/Filler/BlockchainTestFillerEnv.h>
#include <retesteth/testStructures/types/RPC/ToolResponse.h>
using namespace dev;
using namespace test;
using namespace teststruct;
using namespace dataobject;

namespace
{
// We simulate the client backend side here, so thats why number5 is hardcoded
// Map rewards from non standard forks into standard
static std::map<FORK, FORK> RewardMapForToolBefore5 = {{"FrontierToHomesteadAt5", "Frontier"},
    {"HomesteadToEIP150At5", "Homestead"}, {"EIP158ToByzantiumAt5", "EIP158"}, {"HomesteadToDaoAt5", "Homestead"},
    {"ByzantiumToConstantinopleFixAt5", "Byzantium"}};
static std::map<FORK, FORK> RewardMapForToolAfter5 = {{"FrontierToHomesteadAt5", "Homestead"},
    {"HomesteadToEIP150At5", "EIP150"}, {"EIP158ToByzantiumAt5", "Byzantium"}, {"HomesteadToDaoAt5", "Homestead"},
    {"ByzantiumToConstantinopleFixAt5", "ConstantinopleFix"}};

std::tuple<VALUE, FORK> prepareReward(SealEngine _engine, FORK const& _fork, VALUE const& _blockNumber = VALUE(0))
{
    if (_engine == SealEngine::Ethash)
        ETH_WARNING_TEST("t8ntool backend treat Ethash as NoProof!", 6);

    // Setup mining rewards
    std::map<FORK, spVALUE> const& rewards = Options::get().getDynamicOptions().getCurrentConfig().getRewardMap();
    if (rewards.count(_fork))
        return {rewards.at(_fork).getCContent(), _fork};
    else
    {
        if (_blockNumber < 5)
        {
            assert(RewardMapForToolBefore5.count(_fork));
            auto const& trFork = RewardMapForToolBefore5.at(_fork);
            assert(rewards.count(trFork));
            return {rewards.at(trFork).getCContent(), trFork};
        }
        else
        {
            assert(RewardMapForToolAfter5.count(_fork));
            auto const& trFork = RewardMapForToolAfter5.at(_fork);
            assert(rewards.count(trFork));
            return {rewards.at(trFork).getCContent(), trFork};
        }
    }
}

// Because tool report incomplete state. restore missing fields with zeros
// Also remove leading zeros in storage
State restoreFullState(DataObject const& _toolState)
{
    DataObject fullState;
    for (auto const& accTool : _toolState.getSubObjects())
    {
        DataObject acc;
        acc["balance"] = accTool.count("balance") ? accTool.atKey("balance").asString() : "0x00";
        acc["nonce"] = accTool.count("nonce") ? accTool.atKey("nonce").asString() : "0x00";
        acc["code"] = accTool.count("code") ? accTool.atKey("code").asString() : "0x";
        acc["storage"] = accTool.count("storage") ? accTool.atKey("storage") : DataObject(DataType::Object);
        for (auto& storageRecord : acc.atKeyUnsafe("storage").getSubObjectsUnsafe())
        {
            storageRecord.performModifier(mod_removeLeadingZerosFromHexValuesEVEN);
            storageRecord.performModifier(mod_removeLeadingZerosFromHexKeysEVEN);
        }
        fullState[accTool.getKey()] = acc;
    }
    return State(fullState);
}
}  // namespace


namespace toolimpl
{
ToolChain::ToolChain(EthereumBlockState const& _genesis, SealEngine _sealEngine, FORK const& _fork, fs::path const& _toolPath)
  : m_engine(_sealEngine), m_fork(new FORK(_fork)), m_toolPath(_toolPath)
{
    // We yet don't know the state root. ask the tool to calculate it
    ToolResponse res = mineBlockOnTool(_genesis, SealEngine::NoReward);

    EthereumBlockState genesisFixed(_genesis.header().asDataObject(), _genesis.state(), FH32::zero());
    genesisFixed.headerUnsafe().setStateRoot(res.stateRoot());
    genesisFixed.headerUnsafe().recalculateHash();
    genesisFixed.addTotalDifficulty(genesisFixed.header().difficulty());
    m_blocks.push_back(genesisFixed);
}

void ToolChain::mineBlock(EthereumBlockState const& _pendingBlock, Mining _req)
{
    ToolResponse const res = mineBlockOnTool(_pendingBlock, m_engine);

    // Pending fixed is pending header corrected by the information returned by tool
    // The tool can reject transactions changing the stateHash, TxRoot, TxReceipts, HeaderHash, GasUsed
    EthereumBlockState pendingFixed(_pendingBlock.header().asDataObject(), res.state(), res.logsHash());

    // Construct a block header with information that we have and what we get from t8ntool
    // The block number is current max block + 1
    pendingFixed.headerUnsafe().setNumber(m_blocks.size());

    // Tool calculated transactions and state
    pendingFixed.headerUnsafe().setStateRoot(res.stateRoot());         // Assign StateHash from the tool
    pendingFixed.headerUnsafe().setGasUsed(res.totalGasUsed());        // Assign GasUsed from the tool
    pendingFixed.headerUnsafe().setTransactionHash(res.txRoot());      // Assign TxRoot from the tool
    pendingFixed.headerUnsafe().setTrReceiptsHash(res.receiptRoot());  // Assign TxReceipt from the tool
    pendingFixed.headerUnsafe().setLogsBloom(res.logsBloom());         // Assign LogsBloom from the
    pendingFixed.headerUnsafe().setStateRoot(res.stateRoot());         // Assign StateHash from the tool

    // Add only those transactions which tool returned a receipt for
    // Some transactions are expected to fail. That should be detected by tests
    for (auto const& tr : _pendingBlock.transactions())
    {
        bool found = false;
        FH32 const trHash = tr.hash();
        for (auto const& trReceipt : res.receipts())
        {
            if (trReceipt.trHash() == trHash)
            {
                found = true;
                pendingFixed.addTransaction(tr);
            }
        }
        if (!found)
        {
            string const message = "t8ntool didn't return a transaction with hash: " + trHash.asString();
            if (_req == Mining::AllowFailTransactions)
                ETH_WARNING_TEST(message, 6);
            else
                throw test::UpwardsException(message);
        }
    }

    // Treat all uncles as valid as t8ntool does not calculate uncles
    // Uncle header validity as well as RLP logic is checked before
    for (auto const& un : _pendingBlock.uncles())
        pendingFixed.addUncle(un);

    // Calculate header hash from header fields (do not recalc tx, un hashes)
    pendingFixed.headerUnsafe().recalculateHash();

    // Blockchain rules
    // Require number from pending block to be equal to actual block number that is imported
    if (_pendingBlock.header().number() != pendingFixed.header().number().asU256())
        throw test::UpwardsException("Block Number from pending block != actual chain height!");

    // Require new block timestamp to be > than the previous block timestamp
    if (lastBlock().header().timestamp() > pendingFixed.header().timestamp())
        throw test::UpwardsException("Block Timestamp from pending block < previous block timestamp!");

    if (_req == Mining::RequireValid)
    {
        if (m_fork.getContent().asString() == "HomesteadToDaoAt5" && pendingFixed.header().number() > 4 &&
            pendingFixed.header().number() < 19 &&
            pendingFixed.header().extraData().asString() != "0x64616f2d686172642d666f726b")
            throw test::UpwardsException("Dao Extra Data required!");

        if (_pendingBlock.header() != pendingFixed.header())
        {
            DataObject const pendingH = _pendingBlock.header().asDataObject();
            DataObject const pendingFixedH = pendingFixed.header().asDataObject();
            string const compare = compareBlockHeaders(pendingH, pendingFixedH);
            throw test::UpwardsException(string("Block from pending block != t8ntool constructed block!\n") +
                                         "rawRLP/Pending header  vs  t8ntool header \n" + compare);
        }
    }

    if (pendingFixed.header().transactionRoot() != res.txRoot())
    {
        ETH_ERROR_MESSAGE(string("ToolChain::mineBlock txRootHash is different to one ruturned by tool \n") +
                          "constructedBlockHash: " + pendingFixed.header().transactionRoot().asString() +
                          "\n toolTransactionRoot: " + res.txRoot().asString());
    }

    VALUE totalDifficulty(0);
    if (m_blocks.size() > 0)
        totalDifficulty = m_blocks.at(m_blocks.size() - 1).totalDifficulty();

    pendingFixed.addTotalDifficulty(totalDifficulty + pendingFixed.header().difficulty());
    m_blocks.push_back(pendingFixed);
}

ToolResponse ToolChain::mineBlockOnTool(EthereumBlockState const& _block, SealEngine _engine)
{
    fs::path tmpDir = test::createUniqueTmpDirectory();

    // env.json file
    fs::path envPath = tmpDir / "env.json";
    BlockchainTestFillerEnv env(_block.header().asDataObject(), m_engine);
    DataObject envData = env.asDataObject();

    // BlockHeader hash information for tool mining
    size_t k = 0;
    for (auto const& bl : m_blocks)
        envData["blockHashes"][fto_string(k++)] = bl.header().hash().asString();
    for (auto const& un : _block.uncles())
    {
        DataObject uncle;

        // -- validate uncle header
        int delta = (int)(_block.header().number() - un.number()).asU256();
        if (delta < 1)
            throw test::UpwardsException("Uncle header delta is < 1");
        if (un.number().asU256() == 0 || un.number().asU256() >= m_blocks.size())
            throw test::UpwardsException("Uncle number is too old or 0!");
        size_t uncleParent = (size_t)un.number().asU256() - 1;
        if (m_blocks.at(uncleParent).header().timestamp().asU256() >= un.timestamp().asU256())
            throw test::UpwardsException("Uncle timestamp is less then its parent block!");
        if (m_fork.getContent().asString() == "HomesteadToDaoAt5" && un.number() > 4 && un.number() < 19 &&
            un.extraData().asString() != "0x64616f2d686172642d666f726b")
            throw test::UpwardsException("Uncle Dao Extra Data required!");

        // ---
        uncle["delta"] = delta;
        uncle["address"] = un.author().asString();
        envData["ommers"].addArrayObject(uncle);
    }

    writeFile(envPath.string(), envData.asJson());

    // alloc.json file
    fs::path allocPath = tmpDir / "alloc.json";
    writeFile(allocPath.string(), _block.state().asDataObject().asJsonNoFirstKey());

    // txs.json file
    fs::path txsPath = tmpDir / "txs.json";
    DataObject txs(DataType::Array);
    for (auto const& tr : _block.transactions())
        txs.addArrayObject(tr.asDataObject(ExportOrder::ToolStyle));
    writeFile(txsPath.string(), txs.asJson());

    // output file
    fs::path outPath = tmpDir / "out.json";
    fs::path outAllocPath = tmpDir / "outAlloc.json";

    // Convert FrontierToHomesteadAt5 -> Homestead if block > 5, and get reward
    auto tupleRewardFork = prepareReward(_engine, m_fork.getContent(), _block.header().number());

    string cmd = m_toolPath.string();
    cmd += " --input.alloc " + allocPath.string();
    cmd += " --input.txs " + txsPath.string();
    cmd += " --input.env " + envPath.string();
    cmd += " --state.fork " + std::get<1>(tupleRewardFork).asString();
    cmd += " --output.result " + outPath.string();
    cmd += " --output.alloc " + outAllocPath.string();
    if (_engine != SealEngine::NoReward)
        cmd += " --state.reward " + std::get<0>(tupleRewardFork).asDecString();

    ETH_TEST_MESSAGE("Alloc:\n" + contentsString(allocPath.string()));
    if (_block.transactions().size())
        ETH_TEST_MESSAGE("Txs:\n" + contentsString(txsPath.string()));
    ETH_TEST_MESSAGE("Env:\n" + contentsString(envPath.string()));
    test::executeCmd(cmd, ExecCMDWarning::NoWarning);
    ETH_TEST_MESSAGE("Res:\n" + contentsString(outPath.string()));
    ETH_TEST_MESSAGE("RAlloc:\n" + contentsString(outAllocPath.string()));
    ETH_TEST_MESSAGE(cmd);

    // Construct block rpc response
    ToolResponse toolResponse(ConvertJsoncppStringToData(contentsString(outPath)));
    DataObject returnState = ConvertJsoncppStringToData(contentsString(outAllocPath));
    toolResponse.attachState(restoreFullState(returnState));
    return toolResponse;
}

void ToolChain::rewindToBlock(size_t _number)
{
    while (m_blocks.size() > _number + 1)
        m_blocks.pop_back();
}

}  // namespace toolimpl
