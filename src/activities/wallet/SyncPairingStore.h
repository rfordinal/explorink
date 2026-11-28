#pragma once

#include <cstdint>

#include "SyncChannel.h"

// Where the sync channel's long-term state lives: NVS namespace `cpsync`.
//
//   dev_sk   32 bytes   this device's static X25519 private key
//   phones   blob       the paired phones (SyncChannel.h, PairedPhones)
//
// A namespace of its own rather than a key in `cpwallet`, so the wallet wrap and
// the pairings can be forgotten separately (the screen's "forget a phone") and
// together (forgetting the wallet) without one code path knowing the other's
// keys.
//
// **NVS is plaintext flash.** A dumped flash gives a thief this device's static
// key and the list of phones it trusts -- enough to pose as this device to a
// paired phone, or as a paired phone to this device. That is the wallet wrap's
// boundary too (docs/wallet-crypto.md, "The threat boundary, first"), and no
// product text may claim more (docs/sync-channel.md, "Where the keys live").

namespace syncstore {

// Reads the static key, generating one the first time. Generated on the device,
// never sent anywhere except as its public half inside the pairing QR.
bool loadOrCreateStaticKey(uint8_t out[syncnoise::kDhLen]);

// A missing or unparseable table loads as empty, which refuses every sync: the
// safe side (SyncChannel.h, PairedPhones::parse).
void loadTable(syncchannel::PairedPhones& out);

bool saveTable(const syncchannel::PairedPhones& table);

// The PairingSink the channel stores through.
syncchannel::PairingSink sink();

// Forgets the table **and** the static key, so the next pairing is with a new
// device identity and no phone that knew the old one can talk to it. Called when
// the wallet is forgotten (plan invariant 5, wallet::KeyStore::forgetWrap).
void forgetAll();

}  // namespace syncstore
