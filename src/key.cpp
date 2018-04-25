// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2017-2018 The Gostcoin Developers
// Copyright (c) 2018- The SPbCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "Crypto.h"
#include "Gost.h"
#include "key.h"

#include <openssl/ecdsa.h>
#include <openssl/rand.h>
#include <openssl/obj_mac.h>

// anonymous namespace with local implementation code (OpenSSL interaction)
namespace {

// Generate a private key from just the secret parameter
int EC_KEY_regenerate_key(EC_KEY *eckey, BIGNUM *priv_key)
{
    int ok = 0;
    BN_CTX *ctx = NULL;
    EC_POINT *pub_key = NULL;

    if (!eckey) return 0;

    const EC_GROUP *group = EC_KEY_get0_group(eckey);

    if ((ctx = BN_CTX_new()) == NULL)
        goto err;

    pub_key = EC_POINT_new(group);

    if (pub_key == NULL)
        goto err;

    if (!EC_POINT_mul(group, pub_key, priv_key, NULL, NULL, ctx))
        goto err;

    EC_KEY_set_private_key(eckey,priv_key);
    EC_KEY_set_public_key(eckey,pub_key);

    ok = 1;

err:

    if (pub_key)
        EC_POINT_free(pub_key);
    if (ctx != NULL)
        BN_CTX_free(ctx);

    return(ok);
}

// Perform ECDSA key recovery (see SEC1 4.1.6) for curves over (mod p)-fields
// recid selects which key is recovered
// if check is non-zero, additional checks are performed
static int ECDSA_SIG_recover_key_GFp(EC_KEY *eckey, ECDSA_SIG *ecsig, const unsigned char *msg, int msglen, int recid, int check)
{
    if (!eckey) return 0;
	BIGNUM * d = BN_bin2bn (msg, msglen, nullptr); 
	const auto& curve = i2p::crypto::GetGOSTR3410Curve (i2p::crypto::eGOSTR3410CryptoProA);
    const BIGNUM * r, * s;
	ECDSA_SIG_get0 (ecsig, &r, &s);
	EC_POINT * pub = curve->RecoverPublicKey (d, r, s, recid % 2);
	BN_free (d);
	if (!pub) return 0;
	EC_KEY_set_public_key(eckey, pub);	
	EC_POINT_free (pub);
	return 1;
}

// RAII Wrapper around OpenSSL's EC_KEY
class CECKey {
private:
    EC_KEY *pkey;

public:
    CECKey() 
	{
		pkey = EC_KEY_new ();
		// GOST 34.10-2012
		auto ret = EC_KEY_set_group(pkey, i2p::crypto::GetGOSTR3410Curve (i2p::crypto::eGOSTR3410CryptoProA)->GetGroup ());
		assert (ret == 1);
    }

    ~CECKey() {
        EC_KEY_free(pkey);
    }

    void GetSecretBytes(unsigned char vch[32]) const {
        const BIGNUM *bn = EC_KEY_get0_private_key(pkey);
        assert(bn);
        int nBytes = BN_num_bytes(bn);
        int n=BN_bn2bin(bn,&vch[32 - nBytes]);
        assert(n == nBytes);
        memset(vch, 0, 32 - nBytes);
    }

    void SetSecretBytes(const unsigned char vch[32]) {
        BIGNUM * bn = BN_new ();
        assert(BN_bin2bn(vch, 32, bn));
        assert(EC_KEY_regenerate_key(pkey, bn));
        BN_clear_free(bn);
    }

    void GetPrivKey(CPrivKey &privkey, bool fCompressed) {
        EC_KEY_set_conv_form(pkey, fCompressed ? POINT_CONVERSION_COMPRESSED : POINT_CONVERSION_UNCOMPRESSED);
        int nSize = i2d_ECPrivateKey(pkey, NULL);
        assert(nSize);
        privkey.resize(nSize);
        unsigned char* pbegin = &privkey[0];
        int nSize2 = i2d_ECPrivateKey(pkey, &pbegin);
        assert(nSize == nSize2);
    }

    bool SetPrivKey(const CPrivKey &privkey) {
        const unsigned char* pbegin = &privkey[0];
        if (d2i_ECPrivateKey(&pkey, &pbegin, privkey.size())) {
            // d2i_ECPrivateKey returns true if parsing succeeds.
            // This doesn't necessarily mean the key is valid.
            if (EC_KEY_check_key(pkey))
                return true;
        }
        return false;
    }

    void GetPubKey(CPubKey &pubkey, bool fCompressed) {
        EC_KEY_set_conv_form(pkey, fCompressed ? POINT_CONVERSION_COMPRESSED : POINT_CONVERSION_UNCOMPRESSED);
        int nSize = i2o_ECPublicKey(pkey, NULL);
        assert(nSize);
        assert(nSize <= 65);
        unsigned char c[65];
        unsigned char *pbegin = c;
        int nSize2 = i2o_ECPublicKey(pkey, &pbegin);
        assert(nSize == nSize2);
        pubkey.Set(&c[0], &c[nSize]);
    }

    bool SetPubKey(const CPubKey &pubkey) {
        const unsigned char* pbegin = pubkey.begin();
        return o2i_ECPublicKey(&pkey, &pbegin, pubkey.size());
    }

    bool Sign(const uint256 &hash, std::vector<unsigned char>& vchSig) 
	{
		const BIGNUM * priv = EC_KEY_get0_private_key(pkey);
		BIGNUM * d = BN_bin2bn (hash.begin (), 32, nullptr); 
        BIGNUM * r = BN_new (), * s = BN_new ();
		i2p::crypto::GetGOSTR3410Curve (i2p::crypto::eGOSTR3410CryptoProA)->Sign (priv, d, r, s);
        ECDSA_SIG *sig = ECDSA_SIG_new ();	
        ECDSA_SIG_set0 (sig, r, s);
		// encode signature is in DER format		
		auto nSize = ECDSA_size (pkey); // max size
		vchSig.resize(nSize);
		auto p = &vchSig[0];
		nSize = i2d_ECDSA_SIG (sig, &p);
		vchSig.resize(nSize); // acutal size
		BN_free (d);	
		ECDSA_SIG_free(sig);
        return true;
    }

    bool Verify(const uint256 &hash, const std::vector<unsigned char>& vchSig) 
	{
		// decode from DER
		ECDSA_SIG *sig = nullptr;
		auto p = &vchSig[0];	
		d2i_ECDSA_SIG (&sig, &p, vchSig.size());	
		const EC_POINT * pub = EC_KEY_get0_public_key(pkey);
		BIGNUM * d = BN_bin2bn (hash.begin (), 32, nullptr);
        const BIGNUM * r, * s;
		ECDSA_SIG_get0 (sig, &r, &s);
		bool ret = i2p::crypto::GetGOSTR3410Curve (i2p::crypto::eGOSTR3410CryptoProA)->Verify (pub, d, r, s);
		BN_free (d); 
		ECDSA_SIG_free(sig);
		return ret;
    }

    bool SignCompact(const uint256 &hash, unsigned char *p64, int &rec) 
	{
        bool fOk = false;
        ECDSA_SIG *sig = ECDSA_SIG_new ();
		const BIGNUM * priv = EC_KEY_get0_private_key(pkey);
		BIGNUM * d = BN_bin2bn (hash.begin (), 32, nullptr);
        BIGNUM * r = BN_new (), * s = BN_new ();
		i2p::crypto::GetGOSTR3410Curve (i2p::crypto::eGOSTR3410CryptoProA)->Sign (priv, d, r, s);
        ECDSA_SIG_set0 (sig, r, s);
		BN_free (d);
        if (sig==NULL)
            return false;
        memset(p64, 0, 64);
        int nBitsR = BN_num_bits(r);
        int nBitsS = BN_num_bits(s);
        if (nBitsR <= 256 && nBitsS <= 256) {
            CPubKey pubkey;
            GetPubKey(pubkey, true);
            for (int i=0; i<2; i++) {
                CECKey keyRec;
                if (ECDSA_SIG_recover_key_GFp(keyRec.pkey, sig, (unsigned char*)&hash, sizeof(hash), i, 1) == 1) {
                    CPubKey pubkeyRec;
                    keyRec.GetPubKey(pubkeyRec, true);
                    if (pubkeyRec == pubkey) {
                        rec = i;
                        fOk = true;
                        break;
                    }
                }
            }
            assert(fOk);
            BN_bn2bin(r,&p64[32-(nBitsR+7)/8]);
            BN_bn2bin(s,&p64[64-(nBitsS+7)/8]);
        }
        ECDSA_SIG_free(sig);
        return fOk;
    }

    // reconstruct public key from a compact signature
    // This is only slightly more CPU intensive than just verifying it.
    // If this function succeeds, the recovered public key is guaranteed to be valid
    // (the signature is a valid signature of the given data for that key)
    bool Recover(const uint256 &hash, const unsigned char *p64, int rec)
    {
        if (rec<0 || rec>=3)
            return false;
        ECDSA_SIG *sig = ECDSA_SIG_new();
        auto r = BN_bin2bn(&p64[0],  32, NULL);
        auto s = BN_bin2bn(&p64[32], 32, NULL);
        ECDSA_SIG_set0 (sig, r, s);
        bool ret = ECDSA_SIG_recover_key_GFp(pkey, sig, (unsigned char*)&hash, sizeof(hash), rec, 0) == 1;
        ECDSA_SIG_free(sig);
        return ret;
    }
};

}; // end of anonymous namespace

bool CKey::Check(const unsigned char *vch) {
    // Do not convert to OpenSSL's data structures for range-checking keys,
    // it's easy enough to do directly.
    static const unsigned char vchMax[32] = {
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFE,
        0xBA,0xAE,0xDC,0xE6,0xAF,0x48,0xA0,0x3B,
        0xBF,0xD2,0x5E,0x8C,0xD0,0x36,0x41,0x40
    };
    bool fIsZero = true;
    for (int i=0; i<32 && fIsZero; i++)
        if (vch[i] != 0)
            fIsZero = false;
    if (fIsZero)
        return false;
    for (int i=0; i<32; i++) {
        if (vch[i] < vchMax[i])
            return true;
        if (vch[i] > vchMax[i])
            return false;
    }
    return true;
}

void CKey::MakeNewKey(bool fCompressedIn) {
    do {
        RAND_bytes(vch, sizeof(vch));
    } while (!Check(vch));
    fValid = true;
    fCompressed = fCompressedIn;
}

bool CKey::SetPrivKey(const CPrivKey &privkey, bool fCompressedIn) {
    CECKey key;
    if (!key.SetPrivKey(privkey))
        return false;
    key.GetSecretBytes(vch);
    fCompressed = fCompressedIn;
    fValid = true;
    return true;
}

CPrivKey CKey::GetPrivKey() const {
    assert(fValid);
    CECKey key;
    key.SetSecretBytes(vch);
    CPrivKey privkey;
    key.GetPrivKey(privkey, fCompressed);
    return privkey;
}

CPubKey CKey::GetPubKey() const {
    assert(fValid);
    CECKey key;
    key.SetSecretBytes(vch);
    CPubKey pubkey;
    key.GetPubKey(pubkey, fCompressed);
    return pubkey;
}

bool CKey::Sign(const uint256 &hash, std::vector<unsigned char>& vchSig) const {
    if (!fValid)
        return false;
    CECKey key;
    key.SetSecretBytes(vch);
    return key.Sign(hash, vchSig);
}

bool CKey::SignCompact(const uint256 &hash, std::vector<unsigned char>& vchSig) const {
    if (!fValid)
        return false;
    CECKey key;
    key.SetSecretBytes(vch);
    vchSig.resize(65);
    int rec = -1;
    if (!key.SignCompact(hash, &vchSig[1], rec))
        return false;
    assert(rec != -1);
    vchSig[0] = 27 + rec + (fCompressed ? 4 : 0);
    return true;
}

bool CPubKey::Verify(const uint256 &hash, const std::vector<unsigned char>& vchSig) const {
    if (!IsValid())
        return false;
    CECKey key;
    if (!key.SetPubKey(*this))
        return false;
    if (!key.Verify(hash, vchSig))
        return false;
    return true;
}

bool CPubKey::RecoverCompact(const uint256 &hash, const std::vector<unsigned char>& vchSig) {
    if (vchSig.size() != 65)
        return false;
    CECKey key;
    if (!key.Recover(hash, &vchSig[1], (vchSig[0] - 27) & ~4))
        return false;
    key.GetPubKey(*this, (vchSig[0] - 27) & 4);
    return true;
}

bool CPubKey::VerifyCompact(const uint256 &hash, const std::vector<unsigned char>& vchSig) const {
    if (!IsValid())
        return false;
    if (vchSig.size() != 65)
        return false;
    CECKey key;
    if (!key.Recover(hash, &vchSig[1], (vchSig[0] - 27) & ~4))
        return false;
    CPubKey pubkeyRec;
    key.GetPubKey(pubkeyRec, IsCompressed());
    if (*this != pubkeyRec)
        return false;
    return true;
}

bool CPubKey::IsFullyValid() const {
    if (!IsValid())
        return false;
    CECKey key;
    if (!key.SetPubKey(*this))
        return false;
    return true;
}

bool CPubKey::Decompress() {
    if (!IsValid())
        return false;
    CECKey key;
    if (!key.SetPubKey(*this))
        return false;
    key.GetPubKey(*this, false);
    return true;
}
