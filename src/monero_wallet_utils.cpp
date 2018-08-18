//
//  monero_wallet_utils.cpp
//  Copyright (c) 2014-2018, MyMonero.com
//
//  All rights reserved.
//
//  Redistribution and use in source and binary forms, with or without modification, are
//  permitted provided that the following conditions are met:
//
//  1. Redistributions of source code must retain the above copyright notice, this list of
//	conditions and the following disclaimer.
//
//  2. Redistributions in binary form must reproduce the above copyright notice, this list
//	of conditions and the following disclaimer in the documentation and/or other
//	materials provided with the distribution.
//
//  3. Neither the name of the copyright holder nor the names of its contributors may be
//	used to endorse or promote products derived from this software without specific
//	prior written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
//  EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
//  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
//  THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
//  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
//  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
//  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
//  STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
//  THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//
//
#include "monero_wallet_utils.hpp"
#include <boost/algorithm/string.hpp>
#include "cryptonote_basic.h"
#include "device/device.hpp"
#include "cryptonote_basic/account.h"
#include "keccak.h"
//
#include "string_tools.h"
using namespace epee;
//
extern "C" {
	#include "crypto-ops.h"
}
//
using namespace monero_wallet_utils;
using namespace crypto; // for extension
//
// 16 byte seeds
void cn_pad_by_fast_hash__C(const uint8_t *in, size_t inlen, uint8_t *md, int mdlen)
{
	keccak(in, inlen, md, mdlen);
}
inline void cn_pad_by_fast_hash(const uint8_t *indata, std::size_t inlen, uint8_t *outdata, std::size_t outlen)
{
	cn_pad_by_fast_hash__C(indata, inlen, outdata, (int)outlen);
}
void monero_wallet_utils::coerce_valid_sec_key_from(
	const legacy16B_secret_key &legacy16B_mymonero_sec_seed,
	secret_key &dst__sec_seed
) { // cn_fast_hash legacy16B_sec_seed in order to 'pad' it to 256 bits so it can be chopped to ec_scalar
	static_assert(!epee::has_padding<legacy16B_secret_key>(), "potential hash of padding data");
	static_assert(!epee::has_padding<secret_key>(), "writing to struct with extra data");
	cn_pad_by_fast_hash((uint8_t *)&legacy16B_mymonero_sec_seed, sizeof(legacy16B_secret_key),
						(uint8_t *)&dst__sec_seed, sizeof(secret_key));
}
bool monero_wallet_utils::words_to_bytes(
	std::string words, 
	legacy16B_secret_key& dst, 
	std::string &language_name
) {
	std::string s;
	if (!crypto::ElectrumWords::words_to_bytes(words, s, sizeof(dst), true, language_name)) {
		return false;
	}
	if (s.size() != sizeof(dst)) {
		return false;
	}
	memcpy(dst.data, s.data(), sizeof(dst.data));
	return true;
}
bool monero_wallet_utils::bytes_to_words(
	const legacy16B_secret_key& src, 
	std::string& words, 
	const std::string &language_name
) {
	return crypto::ElectrumWords::bytes_to_words(
		src.data, sizeof(src),
		words, language_name
	);
}
//
bool monero_wallet_utils::new_wallet(
    std::string mnemonic_language,
	WalletDescriptionRetVals &retVals,
	cryptonote::network_type nettype
) {
	retVals = {};
	//
	cryptonote::account_base account{}; // this initializes the wallet and should call the default constructor
	crypto::secret_key nonLegacy32B_sec_seed = account.generate();
	//
	const cryptonote::account_keys& keys = account.get_keys();
	std::string address_string = account.get_public_address_str(nettype); // getting the string here instead of leaving it to the consumer b/c get_public_address_str could potentially change in implementation (see TODO) so it's not right to duplicate that here
	//
	std::string mnemonic_string;
	bool r = crypto::ElectrumWords::bytes_to_words(nonLegacy32B_sec_seed, mnemonic_string, std::move(mnemonic_language));
	// ^-- it's OK to directly call ElectrumWords w/ crypto::secret_key as we are generating new wallet, not reading
	if (!r) {
		retVals.did_error = true;
		retVals.err_string = "Unable to create new wallet";
		// TODO: return code of unable to convert seed to mnemonic
		//
		return false;
	}
	retVals.optl__desc = WalletDescription{
		string_tools::pod_to_hex(nonLegacy32B_sec_seed),
		//
		address_string,
		//
		keys.m_spend_secret_key,
		keys.m_view_secret_key,
		keys.m_account_address.m_spend_public_key,
		keys.m_account_address.m_view_public_key,
		//
		mnemonic_string
	};
	return true;
}
//
const uint32_t stable_32B_seed_mnemonic_word_count = 25;
const uint32_t legacy_16B_seed_mnemonic_word_count = 13;

bool monero_wallet_utils::decoded_seed(
	std::string mnemonic_string,
	std::string mnemonic_language,
	MnemonicDecodedSeed_RetVals &retVals
) {
	retVals = {};
	//
	// sanitize inputs
	if (mnemonic_string.empty()) {
		retVals.did_error = true;
		retVals.err_string = "Please enter a valid seed";
		//
		return false;
	}
	boost::algorithm::to_lower(mnemonic_string); // critical
	// TODO: strip wrapping whitespace? anything else?
	//
	std::stringstream stream(mnemonic_string); // to count words…
	unsigned long word_count = std::distance(std::istream_iterator<std::string>(stream), std::istream_iterator<std::string>());
	//	unsigned long word_count = boost::range::distance(boost::algorithm::make_split_iterator(mnemonic_string, boost::algorithm::is_space())); // TODO: get this workin
	//
	crypto::secret_key sec_seed;
	std::string sec_seed_string; // TODO/FIXME: needed this for shared ref outside of if branch below… not intending extra default constructor call but not sure how to get around it yet
	bool from_legacy16B_lw_seed = false;
	if (word_count == stable_32B_seed_mnemonic_word_count) {
		from_legacy16B_lw_seed = false; // to be clear
		bool r = crypto::ElectrumWords::words_to_bytes(mnemonic_string, sec_seed, mnemonic_language);
		if (!r) {
			retVals.did_error = true;
			retVals.err_string = "Invalid 25-word mnemonic";
			//
			return false;
		}
		sec_seed_string = string_tools::pod_to_hex(sec_seed);
	} else if (word_count == legacy_16B_seed_mnemonic_word_count) {
		from_legacy16B_lw_seed = true;
		legacy16B_secret_key legacy16B_sec_seed;
		bool r = words_to_bytes(mnemonic_string, legacy16B_sec_seed, mnemonic_language); // special 16 byte function
		if (!r) {
			retVals.did_error = true;
			retVals.err_string = "Invalid 13-word mnemonic";
			//
			return false;
		}
		coerce_valid_sec_key_from(legacy16B_sec_seed, sec_seed);
		sec_seed_string = string_tools::pod_to_hex(legacy16B_sec_seed); // <- NOTE: we are returning the _LEGACY_ seed as the string… this is important so we don't lose the fact it was 16B/13-word originally!
	} else {
		retVals.did_error = true;
		retVals.err_string = "Please enter a 25- or 13-word secret mnemonic.";
		//
		return false;
	}
	retVals.optl__sec_seed = sec_seed;
	retVals.optl__sec_seed_string = sec_seed_string;
	retVals.optl__mnemonic_string = mnemonic_string; 
	retVals.from_legacy16B_lw_seed = from_legacy16B_lw_seed;
	//
	return true;
}
//
SeedDecodedMnemonic_RetVals monero_wallet_utils::mnemonic_string_from_seed_hex_string(
	const std::string &sec_hexString,
	const std::string &mnemonic_language // aka wordset name
) {
	SeedDecodedMnemonic_RetVals retVals = {};
	//
	std::string mnemonic_string;
	uint32_t sec_hexString_length = sec_hexString.size();
	//
	bool r = false;
	if (sec_hexString_length == sec_seed_hex_string_length) { // normal seed
		crypto::secret_key sec_seed;
		r = string_tools::hex_to_pod(sec_hexString, sec_seed);
		if (!r) {
			retVals.did_error = true;
			retVals.err_string = "Invalid seed";
			return retVals;
		}
		r = crypto::ElectrumWords::bytes_to_words(sec_seed, mnemonic_string, mnemonic_language);
	} else if (sec_hexString_length == legacy16B__sec_seed_hex_string_length) {
		legacy16B_secret_key legacy16B_sec_seed;
		r = string_tools::hex_to_pod(sec_hexString, legacy16B_sec_seed);
		if (!r) {
			retVals.did_error = true;
			retVals.err_string = "Invalid seed";
			return retVals;
		}
		r = bytes_to_words(legacy16B_sec_seed, mnemonic_string, mnemonic_language); // called with the legacy16B version
	} else {
		retVals.did_error = true;
		retVals.err_string = "Invalid seed length";
		return retVals;
	}
	if (!r) {
		retVals.did_error = true;
		retVals.err_string = "Couldn't get mnemonic from hex seed";
		return retVals;
	}
	retVals.mnemonic_string = mnemonic_string; // TODO: should/can we just send retVals.mnemonic_string to bytes_to_words ?
	return retVals;
}
//
bool monero_wallet_utils::wallet_with(
	std::string mnemonic_string,
	std::string mnemonic_language,
	WalletDescriptionRetVals &retVals,
	cryptonote::network_type nettype
) {
	retVals = {};
	//
	MnemonicDecodedSeed_RetVals decodedSeed_retVals;
	bool r = decoded_seed(std::move(mnemonic_string), std::move(mnemonic_language), decodedSeed_retVals);
	if (!r) {
		retVals.did_error = true;
		retVals.err_string = *decodedSeed_retVals.err_string; // TODO: assert?
		return false;
	}
	cryptonote::account_base account{}; // this initializes the wallet and should call the default constructor
	account.generate(
		*decodedSeed_retVals.optl__sec_seed, // is this an extra copy? maybe have consumer pass ref as arg instead
		true/*recover*/,
		false/*two_random*/,
		decodedSeed_retVals.from_legacy16B_lw_seed // assumed set if r
	);
	const cryptonote::account_keys& keys = account.get_keys();
	retVals.optl__desc = WalletDescription{
		*decodedSeed_retVals.optl__sec_seed_string, // assumed non nil if r
		//
		account.get_public_address_str(nettype),
		//
		keys.m_spend_secret_key,
		keys.m_view_secret_key,
		keys.m_account_address.m_spend_public_key,
		keys.m_account_address.m_view_public_key,
		//
		*decodedSeed_retVals.optl__mnemonic_string, // assumed non nil if r; copied for return
	};
	return true;
}

bool monero_wallet_utils::validate_wallet_components_with(
	const WalletComponentsToValidate &inputs,
	WalletComponentsValidationResults &outputs
)
{ // TODO: how can the err_strings be prepared for localization?
	outputs = {};
	bool r = false;
	//
	// Address
	cryptonote::address_parse_info decoded_address_info;
	r = cryptonote::get_account_address_from_str(
		decoded_address_info,
		inputs.nettype,
		inputs.address_string
	);
	if (r == false) {
		outputs.did_error = true;
		outputs.err_string = "Invalid address";
		//
		return false;
	}
	//
	// View key:
	crypto::secret_key sec_viewKey;
	r = string_tools::hex_to_pod(inputs.sec_viewKey_string, sec_viewKey);
	if (r == false) {
		outputs.did_error = true;
		outputs.err_string = "Invalid view key";
		//
		return false;
	}
	// Validate pub key derived from sec view key matches decoded_address-cached pub key
	crypto::public_key expected_pub_viewKey;
	r = crypto::secret_key_to_public_key(sec_viewKey, expected_pub_viewKey);
	if (r == false) {
		outputs.did_error = true;
		outputs.err_string = "Invalid view key";
		//
		return false;
	}
	if (decoded_address_info.address.m_view_public_key != expected_pub_viewKey) {
		outputs.did_error = true;
		outputs.err_string = "View key does not match address";
		//
		return false;
	}
	//
	// View-only vs spend-key/seed
	outputs.isInViewOnlyMode = true; // setting the ground state
	//
	crypto::secret_key sec_spendKey; // may be initialized
	if (inputs.optl__sec_spendKey_string) {
		// First check if spend key content actually exists before passing to valid_sec_key_from - so that a spend key decode error can be treated as a failure instead of detecting empty spend keys too
		if ((*inputs.optl__sec_spendKey_string).empty() == false) {
			r = string_tools::hex_to_pod(*inputs.optl__sec_spendKey_string, sec_spendKey);
			if (r == false) { // this is an actual parse error exit condition
				outputs.did_error = true;
				outputs.err_string = "Invalid spend key";
				//
				return false;
			}
			// Validate pub key derived from sec spend key matches decoded_address_info-cached pub key
			crypto::public_key expected_pub_spendKey;
			r = crypto::secret_key_to_public_key(sec_spendKey, expected_pub_spendKey);
			if (r == false) {
				outputs.did_error = true;
				outputs.err_string = "Invalid spend key";
				//
				return false;
			}
			if (decoded_address_info.address.m_spend_public_key != expected_pub_spendKey) {
				outputs.did_error = true;
				outputs.err_string = "Spend key does not match address";
				//
				return false;
			}
			outputs.isInViewOnlyMode = false;
		}
	}
	if (inputs.optl__sec_seed_string) {
		if ((*inputs.optl__sec_seed_string).empty() == false) {
			unsigned long sec_seed_string_length = (*inputs.optl__sec_seed_string).length();
			crypto::secret_key sec_seed;
			bool from_legacy16B_lw_seed = false;
			if (sec_seed_string_length == sec_seed_hex_string_length) { // normal seed
				from_legacy16B_lw_seed = false; // to be clear
				bool r = string_tools::hex_to_pod((*inputs.optl__sec_seed_string), sec_seed);
				if (!r) {
					outputs.did_error = true;
					outputs.err_string = "Invalid seed";
					//
					return false;
				}
			} else if (sec_seed_string_length == legacy16B__sec_seed_hex_string_length) {
				from_legacy16B_lw_seed = true;
				legacy16B_secret_key legacy16B_sec_seed;
				bool r = string_tools::hex_to_pod((*inputs.optl__sec_seed_string), legacy16B_sec_seed);
				if (!r) {
					outputs.did_error = true;
					outputs.err_string = "Invalid seed";
					//
					return false;
				}
				coerce_valid_sec_key_from(legacy16B_sec_seed, sec_seed);
			}
			cryptonote::account_base expected_account{}; // this initializes the wallet and should call the default constructor
			expected_account.generate(sec_seed, true/*recover*/, false/*two_random*/, from_legacy16B_lw_seed);
			const cryptonote::account_keys& expected_account_keys = expected_account.get_keys();
			// TODO: assert sec_spendKey initialized?
			if (expected_account_keys.m_view_secret_key != sec_viewKey) {
				outputs.did_error = true;
				outputs.err_string = "Private view key does not match generated key";
				//
				return false;
			}
			if (expected_account_keys.m_spend_secret_key != sec_spendKey) {
				outputs.did_error = true;
				outputs.err_string = "Private spend key does not match generated key";
				//
				return false;
			}
			if (expected_account_keys.m_account_address.m_view_public_key != decoded_address_info.address.m_view_public_key) {
				outputs.did_error = true;
				outputs.err_string = "Public view key does not match generated key";
				//
				return false;
			}
			if (expected_account_keys.m_account_address.m_spend_public_key != decoded_address_info.address.m_spend_public_key) {
				outputs.did_error = true;
				outputs.err_string = "Public spend key does not match generated key";
				//
				return false;
			}
			//
			outputs.isInViewOnlyMode = false; // TODO: should this ensure that sec_spendKey is not nil? spendKey should always be available if the seed is…
		}
	}
	outputs.pub_viewKey_string = string_tools::pod_to_hex(decoded_address_info.address.m_view_public_key);
	outputs.pub_spendKey_string = string_tools::pod_to_hex(decoded_address_info.address.m_spend_public_key);
	outputs.isValid = true;
	//
	return true;
}
