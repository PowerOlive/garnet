// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use bytes::{BufMut, BytesMut};
use crate::crypto_utils::prf;
use failure::{self, bail};
use num::bigint::{BigUint, RandBigInt};
use rand::OsRng;
use std::sync::{Arc, Mutex};
use time;

pub type Nonce = [u8; 32];

/// Thread-safe nonce generator.
/// According to IEEE Std 802.11-2016, 12.7.5 each STA should be configured with an initial,
/// cryptographic-quality random counter at system boot up time.
#[derive(Debug)]
pub struct NonceReader {
    key_counter: Mutex<BigUint>,
}

// This implementation is a direct result of a test outside of this crate comparing an object which
// owns a NonceReader to an expected value. As a result, many struct in this crate now have to
// derive PartialEq. This should get fixed.
// For now, assume NonceReaders always match as there should only ever be one created.
impl PartialEq for NonceReader {
    fn eq(&self, _other: &NonceReader) -> bool { true }
}

impl NonceReader {
    pub fn new(sta_addr: &[u8]) -> Result<Arc<NonceReader>, failure::Error> {
        // Write time and STA's address to buffer for PRF-256.
        // It's unclear whether or not using PRF has any significant cryptographic advantage.
        // For the time being, follow IEEE's recommendation for nonce generation.
        // IEEE Std 802.11-2016, 12.7.5 recommends using a time in NTP format.
        // Fuchsia has no support for NTP yet; instead use a regular timestamp.
        // TODO(NET-430): Use time in NTP format once Fuchsia added support.
        let mut buf = BytesMut::with_capacity(14);
        buf.put_u64_le(time::precise_time_ns());
        buf.put_slice(sta_addr);
        let k = OsRng::new()?.gen_biguint(256).to_bytes_le();
        let init = prf(&k[..], "Init Counter", &buf[..], 256)?;
        Ok(Arc::new(NonceReader {
            key_counter: Mutex::new(BigUint::from_bytes_le(&init[..])),
        }))
    }

    pub fn next(&self) -> Result<Nonce, failure::Error> {
        match self.key_counter.lock() {
            Err(_) => bail!("NonceReader's lock is poisoned"),
            Ok(mut counter) => {
                *counter += 1u8;

                // Expand nonce if it's less than 32 bytes.
                let mut result = (*counter).to_bytes_le();
                result.resize(32, 0);
                let mut nonce = Nonce::default();
                nonce.copy_from_slice(&result[..]);
                Ok(nonce)
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_next_nonce() {
        let addr: [u8; 6] = [1, 2, 3, 4, 5, 6];
        let rdr = NonceReader::new(&addr[..]).expect("error creating NonceReader");
        let mut previous_nonce = rdr.next().expect("error generating nonce");
        for _ in 0..300 {
            let nonce = rdr.next().expect("error generating nonce");
            let nonce_int = BigUint::from_bytes_le(&nonce[..]);
            let previous_nonce_int = BigUint::from_bytes_le(&previous_nonce[..]);
            assert_eq!(nonce_int.gt(&previous_nonce_int), true);

            previous_nonce = nonce;
        }
    }
}
