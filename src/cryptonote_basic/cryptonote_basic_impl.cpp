// Copyright (c) 2014-2018, The Monero Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers

#include "include_base_utils.h"
using namespace epee;

#include "cryptonote_basic_impl.h"
#include "string_tools.h"
#include "serialization/binary_utils.h"
#include "serialization/vector.h"
#include "cryptonote_format_utils.h"
#include "cryptonote_config.h"
#include "misc_language.h"
#include "common/base58.h"
#include "crypto/hash.h"
#include "common/int-util.h"
#include "common/dns_utils.h"

#undef AEON_DEFAULT_LOG_CATEGORY
#define AEON_DEFAULT_LOG_CATEGORY "cn"

namespace cryptonote {

  struct integrated_address {
    account_public_address adr;
    crypto::hash8 payment_id;

    BEGIN_SERIALIZE_OBJECT()
      FIELD(adr)
      FIELD(payment_id)
    END_SERIALIZE()

    BEGIN_KV_SERIALIZE_MAP()
      KV_SERIALIZE(adr)
      KV_SERIALIZE(payment_id)
    END_KV_SERIALIZE_MAP()
  };

  /************************************************************************/
  /* Cryptonote helper functions                                          */
  /************************************************************************/
  //-----------------------------------------------------------------------------------------------
  int get_emission_speed(uint8_t version, uint64_t height, bool testnet) 
  {

    if (testnet && height > 1)
        return AFTER_HARDFORK_SPEED_FACTOR;
    
    if (height >= HARDFORK_1_HEIGHT)
      return AFTER_HARDFORK_SPEED_FACTOR;

    return HARDFORK_1_OLD_SPEED_FACTOR;
  }
  //-----------------------------------------------------------------------------------------------
  size_t get_min_block_size(uint8_t version)
  {
    size_t min_block_size = CRYPTONOTE_BLOCK_GRANTED_FULL_REWARD_ZONE_V1;

    if (version == 1) {
      min_block_size = CRYPTONOTE_BLOCK_GRANTED_FULL_REWARD_ZONE_V1;
    } else if (version == 2) {
      min_block_size = CRYPTONOTE_BLOCK_GRANTED_FULL_REWARD_ZONE_V2;
    } 

    return min_block_size;
  }
  //-----------------------------------------------------------------------------------------------
  uint64_t get_base_reward(uint8_t version, uint64_t height, uint64_t already_generated_coins, int speed) 
  {
    uint64_t base_reward = (MONEY_SUPPLY - already_generated_coins) >> speed;
  
    if (version == 1)
    {
      //int target_minutes = DIFFICULTY_TARGET/60;
      int target_minutes = (height < HARDFORK_1_HEIGHT ? HARDFORK_1_OLD_TARGET : DIFFICULTY_TARGET) / 60;
      if(base_reward < FINAL_SUBSIDY_PER_MINUTE * target_minutes)
        base_reward = FINAL_SUBSIDY_PER_MINUTE * target_minutes;
    }

    return base_reward;
  }
  //-----------------------------------------------------------------------------------------------
  size_t get_max_block_size()
  {
    return CRYPTONOTE_MAX_BLOCK_SIZE;
  }
  //-----------------------------------------------------------------------------------------------
  size_t get_max_tx_size()
  {
    return CRYPTONOTE_MAX_TX_SIZE;
  }
  bool get_block_reward(size_t median_size, size_t current_block_size, 
    uint64_t already_generated_coins, uint64_t &reward, 
    uint8_t version, uint64_t height, bool testnet) {
  
    int speed = get_emission_speed(version, height, testnet);
    uint64_t base_reward = get_base_reward(version, height, already_generated_coins, speed);
    uint64_t full_reward_zone = get_min_block_size(version);

    /* This will speed things up to catch up to AEON */
    if(height == 1 && testnet) {
        uint64_t current_aeon_coins = static_cast<uint64_t>(15372295795843000000U);
        base_reward = (current_aeon_coins - already_generated_coins);
        LOG_PRINT_L4("Reward generated. base=" << print_money(base_reward) << "(Money Supply: "  << print_money(MONEY_SUPPLY) << " Minus Already Generated Coins: " << print_money(already_generated_coins)
        << "), values: money_supply=" << print_money(MONEY_SUPPLY) << ", already_generated_coins=" << print_money(already_generated_coins));
    }
    //make it soft  
    if (median_size < full_reward_zone) {
        median_size = full_reward_zone;
    }

    if (current_block_size <= median_size) {
        reward = base_reward;
        return true;
    }

    if(current_block_size > 2 * median_size) {
        MERROR("Block cumulative size is too big: " << current_block_size << ", expected less than " << 2 * median_size);
        return false;
    }

    assert(median_size < std::numeric_limits<uint32_t>::max());
    assert(current_block_size < std::numeric_limits<uint32_t>::max());

    uint64_t product_hi;
    // BUGFIX: 32-bit saturation bug (e.g. ARM7), the result was being
    // treated as 32-bit by default.
    uint64_t multiplicand = 2 * median_size - current_block_size;
    multiplicand *= current_block_size;
    uint64_t product_lo = mul128(base_reward, multiplicand, &product_hi);

    uint64_t reward_hi;
    uint64_t reward_lo;

    div128_32(product_hi, product_lo, static_cast<uint32_t>(median_size), &reward_hi, &reward_lo);
    div128_32(reward_hi, reward_lo, static_cast<uint32_t>(median_size), &reward_hi, &reward_lo);

    assert(0 == reward_hi);
    assert(reward_lo < base_reward);

    reward = reward_lo;

    return true;

}
  
  //------------------------------------------------------------------------------------
  uint8_t get_account_address_checksum(const public_address_outer_blob& bl)
  {
    const unsigned char* pbuf = reinterpret_cast<const unsigned char*>(&bl);
    uint8_t summ = 0;
    for(size_t i = 0; i!= sizeof(public_address_outer_blob)-1; i++)
      summ += pbuf[i];

    return summ;
  }
  //------------------------------------------------------------------------------------
  uint8_t get_account_integrated_address_checksum(const public_integrated_address_outer_blob& bl)
  {
    const unsigned char* pbuf = reinterpret_cast<const unsigned char*>(&bl);
    uint8_t summ = 0;
    for(size_t i = 0; i!= sizeof(public_integrated_address_outer_blob)-1; i++)
      summ += pbuf[i];

    return summ;
  }
  //-----------------------------------------------------------------------
  std::string get_account_address_as_str(
      bool testnet
    , bool subaddress
    , account_public_address const & adr
    )
  {
    uint64_t address_prefix = testnet ?
      (subaddress ? config::testnet::CRYPTONOTE_PUBLIC_SUBADDRESS_BASE58_PREFIX : config::testnet::CRYPTONOTE_PUBLIC_ADDRESS_BASE58_PREFIX) :
      (subaddress ? config::CRYPTONOTE_PUBLIC_SUBADDRESS_BASE58_PREFIX : config::CRYPTONOTE_PUBLIC_ADDRESS_BASE58_PREFIX);

    return tools::base58::encode_addr(address_prefix, t_serializable_object_to_blob(adr));
  }
  //-----------------------------------------------------------------------
  std::string get_account_integrated_address_as_str(
      bool testnet
    , account_public_address const & adr
    , crypto::hash8 const & payment_id
    )
  {
    uint64_t integrated_address_prefix = testnet ? config::testnet::CRYPTONOTE_PUBLIC_INTEGRATED_ADDRESS_BASE58_PREFIX : config::CRYPTONOTE_PUBLIC_INTEGRATED_ADDRESS_BASE58_PREFIX;

    integrated_address iadr = {
      adr, payment_id
    };
    return tools::base58::encode_addr(integrated_address_prefix, t_serializable_object_to_blob(iadr));
  }
  //-----------------------------------------------------------------------
  bool is_coinbase(const transaction& tx)
  {
    if(tx.vin.size() != 1)
      return false;

    if(tx.vin[0].type() != typeid(txin_gen))
      return false;

    return true;
  }
  //-----------------------------------------------------------------------
  bool get_account_address_from_str(
      address_parse_info& info
    , bool testnet
    , std::string const & str
    )
  {
    uint64_t address_prefix = testnet ?
      config::testnet::CRYPTONOTE_PUBLIC_ADDRESS_BASE58_PREFIX : config::CRYPTONOTE_PUBLIC_ADDRESS_BASE58_PREFIX;
    uint64_t integrated_address_prefix = testnet ?
      config::testnet::CRYPTONOTE_PUBLIC_INTEGRATED_ADDRESS_BASE58_PREFIX : config::CRYPTONOTE_PUBLIC_INTEGRATED_ADDRESS_BASE58_PREFIX;
    uint64_t subaddress_prefix = testnet ?
      config::testnet::CRYPTONOTE_PUBLIC_SUBADDRESS_BASE58_PREFIX : config::CRYPTONOTE_PUBLIC_SUBADDRESS_BASE58_PREFIX;

    if (2 * sizeof(public_address_outer_blob) != str.size())
    {
      blobdata data;
      uint64_t prefix;
      if (!tools::base58::decode_addr(str, prefix, data))
      {
        LOG_PRINT_L2("Invalid address format");
        return false;
      }

      if (integrated_address_prefix == prefix)
      {
        info.is_subaddress = false;
        info.has_payment_id = true;
      }
      else if (address_prefix == prefix)
      {
        info.is_subaddress = false;
        info.has_payment_id = false;
      }
      else if (subaddress_prefix == prefix)
      {
        info.is_subaddress = true;
        info.has_payment_id = false;
      }
      else {
        LOG_PRINT_L1("Wrong address prefix: " << prefix << ", expected " << address_prefix 
          << " or " << integrated_address_prefix
          << " or " << subaddress_prefix);
        return false;
      }

      if (info.has_payment_id)
      {
        integrated_address iadr;
        if (!::serialization::parse_binary(data, iadr))
        {
          LOG_PRINT_L1("Account public address keys can't be parsed");
          return false;
        }
        info.address = iadr.adr;
        info.payment_id = iadr.payment_id;
      }
      else
      {
        if (!::serialization::parse_binary(data, info.address))
        {
          LOG_PRINT_L1("Account public address keys can't be parsed");
          return false;
        }
      }

      if (!crypto::check_key(info.address.m_spend_public_key) || !crypto::check_key(info.address.m_view_public_key))
      {
        LOG_PRINT_L1("Failed to validate address keys");
        return false;
      }
    }
    else
    {
      // Old address format
      std::string buff;
      if(!string_tools::parse_hexstr_to_binbuff(str, buff))
        return false;

      if(buff.size()!=sizeof(public_address_outer_blob))
      {
        LOG_PRINT_L1("Wrong public address size: " << buff.size() << ", expected size: " << sizeof(public_address_outer_blob));
        return false;
      }

      public_address_outer_blob blob = *reinterpret_cast<const public_address_outer_blob*>(buff.data());


      if(blob.m_ver > CRYPTONOTE_PUBLIC_ADDRESS_TEXTBLOB_VER)
      {
        LOG_PRINT_L1("Unknown version of public address: " << blob.m_ver << ", expected " << CRYPTONOTE_PUBLIC_ADDRESS_TEXTBLOB_VER);
        return false;
      }

      if(blob.check_sum != get_account_address_checksum(blob))
      {
        LOG_PRINT_L1("Wrong public address checksum");
        return false;
      }

      //we success
      info.address = blob.m_address;
      info.is_subaddress = false;
      info.has_payment_id = false;
    }

    return true;
  }
  //--------------------------------------------------------------------------------
  bool get_account_address_from_str_or_url(
      address_parse_info& info
    , bool testnet
    , const std::string& str_or_url
    , std::function<std::string(const std::string&, const std::vector<std::string>&, bool)> dns_confirm
    )
  {
    if (get_account_address_from_str(info, testnet, str_or_url))
      return true;
    bool dnssec_valid;
    std::string address_str = tools::dns_utils::get_account_address_as_str_from_url(str_or_url, dnssec_valid, dns_confirm);
    return !address_str.empty() &&
      get_account_address_from_str(info, testnet, address_str);
  }
  //--------------------------------------------------------------------------------
  bool operator ==(const cryptonote::transaction& a, const cryptonote::transaction& b) {
    return cryptonote::get_transaction_hash(a) == cryptonote::get_transaction_hash(b);
  }

  bool operator ==(const cryptonote::block& a, const cryptonote::block& b) {
    return cryptonote::get_block_hash(a) == cryptonote::get_block_hash(b);
  }
}

//--------------------------------------------------------------------------------
bool parse_hash256(const std::string str_hash, crypto::hash& hash)
{
  std::string buf;
  bool res = epee::string_tools::parse_hexstr_to_binbuff(str_hash, buf);
  if (!res || buf.size() != sizeof(crypto::hash))
  {
    std::cout << "invalid hash format: <" << str_hash << '>' << std::endl;
    return false;
  }
  else
  {
    buf.copy(reinterpret_cast<char *>(&hash), sizeof(crypto::hash));
    return true;
  }
}
