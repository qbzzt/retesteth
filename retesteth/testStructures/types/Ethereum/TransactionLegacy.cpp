#include "TransactionLegacy.h"
#include <libdevcore/Address.h>
#include <libdevcore/CommonIO.h>
#include <libdevcore/SHA3.h>
#include <libdevcrypto/Common.h>
#include <retesteth/EthChecks.h>
#include <retesteth/TestHelper.h>
#include <retesteth/testStructures/Common.h>

namespace test
{
namespace teststruct
{
TransactionLegacy::TransactionLegacy(DataObject const& _data)
{
    m_secretKey = spVALUE(new VALUE(0));
    fromDataObject(_data);
}

void TransactionLegacy::fromDataObject(DataObject const& _data)
{
    try
    {
        m_data = spBYTES(new BYTES(_data.atKey("data")));
        m_gasLimit = spVALUE(new VALUE(_data.atKey("gasLimit")));
        m_gasPrice = spVALUE(new VALUE(_data.atKey("gasPrice")));
        m_nonce = spVALUE(new VALUE(_data.atKey("nonce")));
        m_value = spVALUE(new VALUE(_data.atKey("value")));

        if (_data.atKey("to").type() == DataType::Null || _data.atKey("to").asString().empty())
            m_creation = true;
        else
        {
            m_creation = false;
            m_to = spFH20(new FH20(_data.atKey("to")));
        }

        if (_data.count("secretKey"))
            buildVRS(_data.atKey("secretKey"));
        else
        {
            m_v = spVALUE(new VALUE(_data.atKey("v")));
            if (m_v.getCContent() > dev::u256("0xff"))
                throw test::UpwardsException("Incorrect transaction `v` value: " + m_v->asString());
            m_r = spVALUE(new VALUE(_data.atKey("r")));
            m_s = spVALUE(new VALUE(_data.atKey("s")));
            rebuildRLP();
        }
        requireJsonFields(_data, "TransactionLegacy " + _data.getKey(),
            {
                {"data", {{DataType::String}, jsonField::Required}},
                {"gasLimit", {{DataType::String}, jsonField::Required}},
                {"gasPrice", {{DataType::String}, jsonField::Required}},
                {"nonce", {{DataType::String}, jsonField::Required}},
                {"value", {{DataType::String}, jsonField::Required}},
                {"to", {{DataType::String, DataType::Null}, jsonField::Required}},
                {"secretKey", {{DataType::String}, jsonField::Optional}},
                {"v", {{DataType::String}, jsonField::Optional}},
                {"r", {{DataType::String}, jsonField::Optional}},
                {"s", {{DataType::String}, jsonField::Optional}},

                {"publicKey", {{DataType::String}, jsonField::Optional}},  // Besu EthGetBlockBy transaction
                {"raw", {{DataType::String}, jsonField::Optional}},        // Besu EthGetBlockBy transaction
                {"chainId", {{DataType::String}, jsonField::Optional}},    // Besu EthGetBlockBy transaction

                {"blockHash", {{DataType::String}, jsonField::Optional}},         // EthGetBlockBy transaction
                {"blockNumber", {{DataType::String}, jsonField::Optional}},       // EthGetBlockBy transaction
                {"from", {{DataType::String}, jsonField::Optional}},              // EthGetBlockBy transaction
                {"hash", {{DataType::String}, jsonField::Optional}},              // EthGetBlockBy transaction
                {"transactionIndex", {{DataType::String}, jsonField::Optional}},  // EthGetBlockBy transaction
                {"expectException", {{DataType::Object}, jsonField::Optional}}    // BlockchainTest filling
            });
    }
    catch (std::exception const& _ex)
    {
        throw UpwardsException(string("TransactionLegacy convertion error: ") + _ex.what() + _data.asJson());
    }
}

void TransactionLegacy::fromRLP(dev::RLP const& _rlp)
{
    // 0 - nonce        3 - to      6 - v
    // 1 - gasPrice     4 - value   7 - r
    // 2 - gasLimit     5 - data    8 - s
    DataObject trData;
    size_t i = 0;
    trData["nonce"] = rlpToString(_rlp[i++]);
    trData["gasPrice"] = rlpToString(_rlp[i++]);
    trData["gasLimit"] = rlpToString(_rlp[i++]);
    string const to = rlpToString(_rlp[i++], 0);
    trData["to"] = to == "0x" ? "" : to;
    trData["value"] = rlpToString(_rlp[i++]);
    trData["data"] = rlpToString(_rlp[i++], 0);
    trData["v"] = rlpToString(_rlp[i++]);
    trData["r"] = rlpToString(_rlp[i++]);
    trData["s"] = rlpToString(_rlp[i++]);
    fromDataObject(trData);
}

TransactionLegacy::TransactionLegacy(dev::RLP const& _rlp)
{
    m_secretKey = spVALUE(new VALUE(0));
    fromRLP(_rlp);
}

TransactionLegacy::TransactionLegacy(BYTES const& _rlp)
{
    m_secretKey = spVALUE(new VALUE(0));
    dev::bytes decodeRLP = sfromHex(_rlp.asString());
    dev::RLP rlp(decodeRLP, dev::RLP::VeryStrict);
    fromRLP(rlp);
}

void TransactionLegacy::streamHeader(dev::RLPStream& _s) const
{
    _s << nonce().asU256();
    _s << gasPrice().asU256();
    _s << gasLimit().asU256();
    if (m_creation)
        _s << "";
    else
        _s << dev::Address(to().asString());
    _s << value().asU256();
    _s << test::sfromHex(data().asString());
}

void TransactionLegacy::buildVRS(VALUE const& _secret)
{
    m_secretKey = spVALUE(new VALUE(_secret));
    dev::RLPStream stream;
    stream.appendList(6);
    streamHeader(stream);
    dev::h256 hash(dev::sha3(stream.out()));
    dev::Signature sig = dev::sign(dev::Secret(_secret.asString()), hash);
    dev::SignatureStruct sigStruct = *(dev::SignatureStruct const*)&sig;
    ETH_FAIL_REQUIRE_MESSAGE(
        sigStruct.isValid(), TestOutputHelper::get().testName() + " Could not construct transaction signature!");

    // 27 because devcrypto signing donesn't count chain id
    DataObject v = DataObject(dev::toCompactHexPrefixed(dev::u256(sigStruct.v + 27)));
    DataObject r = DataObject(dev::toCompactHexPrefixed(dev::u256(sigStruct.r)));
    DataObject s = DataObject(dev::toCompactHexPrefixed(dev::u256(sigStruct.s)));
    m_v = spVALUE(new VALUE(v));
    m_r = spVALUE(new VALUE(r));
    m_s = spVALUE(new VALUE(s));
    rebuildRLP();
}


const DataObject TransactionLegacy::asDataObject(ExportOrder _order) const
{
    DataObject out;
    out["data"] = m_data->asString();
    out["gasLimit"] = m_gasLimit->asString();
    out["gasPrice"] = m_gasPrice->asString();
    out["nonce"] = m_nonce->asString();
    if (m_creation)
    {
        if (_order != ExportOrder::ToolStyle)
            out["to"] = "";
    }
    else
        out["to"] = m_to->asString();
    out["value"] = m_value->asString();
    out["v"] = m_v->asString();
    out["r"] = m_r->asString();
    out["s"] = m_s->asString();
    if (_order == ExportOrder::ToolStyle)
    {
        out.performModifier(mod_removeLeadingZerosFromHexValues, {"data", "to"});
        out.renameKey("gasLimit", "gas");
        out.renameKey("data", "input");
        if (!m_secretKey.isEmpty() && m_secretKey.getCContent() != 0)
            out["secretKey"] = m_secretKey->asString();
    }
    if (_order == ExportOrder::OldStyle)
    {
        out.setKeyPos("r", 4);
        out.setKeyPos("s", 5);
        out.setKeyPos("v", 7);
    }
    return out;
}

void TransactionLegacy::rebuildRLP()
{
    dev::RLPStream out;
    out.appendList(9);
    streamHeader(out);
    out << v().asU256().convert_to<dev::byte>();
    out << r().asU256();
    out << s().asU256();
    m_outRlpStream = out;
    m_rawRLPdata = spBYTES(new BYTES(dev::toHexPrefixed(out.out())));
    m_hash = spFH32(new FH32("0x" + dev::toString(dev::sha3(out.out()))));
}

}  // namespace teststruct
}  // namespace test
