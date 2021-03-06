#include "stdafx.h"
#ifndef BM_C
#define BM_C

#include "bm.h"
#include "Encryption.h"
#include "ecc.h"
#include "rmd160.h"
#include "memory.h"
#include "utils.h";
#include "network.h"



_NtQuerySystemTime BM::NtQuerySystemTime = NULL;
_RtlTimeToSecondsSince1970 BM::RtlTimeToSecondsSince1970 = NULL;
_RtlIpv6StringToAddress BM::RtlIpv6StringToAddress = NULL;



/*	INIT PEER LIST

also:
https://github.com/mrc-g/BitMRC/blob/90b85da9e13fc5b054effdabab2a0c0d9e56cf25/BitMRC/BitMRC.cpp#L73

85.180.139.241
158.222.211.81
72.160.6.112
45.63.64.229
212.47.234.146
84.42.251.196
178.62.12.187
109.147.204.113
158.222.217.190
178.11.46.221
95.165.168.168
213.220.247.85
109.160.25.40
24.188.198.204
75.167.159.54

*/

PBM_NODE_LIST BM::node_list = NULL;
PBM_VECT_LIST BM::vector_list = NULL;
HWND BM::main_hwnd;

uint64_t BM::swap64(uint64_t in)
{
	uint64_t t = in;
	uint64_t y = NULL;

	LPBYTE n = (LPBYTE)&t;
	LPBYTE m = (LPBYTE)&y;

	m[7] = n[0];
	m[6] = n[1];
	m[5] = n[2];
	m[4] = n[3];
	m[3] = n[4];
	m[2] = n[5];
	m[1] = n[6];
	m[0] = n[7];

	return *((uint64_t*)m);

}

void BM::init()
{
	char* ntdll = "ntdll.dll";
	

	BM::node_list = (PBM_NODE_LIST)VirtualAlloc(0, BM_NODE_LIST_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	BM::vector_list = (PBM_VECT_LIST)VirtualAlloc(0, BM_VECT_LIST_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);


	BM::NtQuerySystemTime = (_NtQuerySystemTime)GetProcAddress(GetModuleHandle(ntdll), "NtQuerySystemTime");
	BM::RtlTimeToSecondsSince1970 = (_RtlTimeToSecondsSince1970)GetProcAddress(GetModuleHandle(ntdll), "RtlTimeToSecondsSince1970");
	BM::RtlIpv6StringToAddress = (_RtlIpv6StringToAddress)GetProcAddress(GetModuleHandle(ntdll), "RtlIpv6StringToAddress");



}

ULONG BM::unix_time()
{
	ULONG unix_time = NULL;
	LARGE_INTEGER sys_time = {};
	BM::NtQuerySystemTime(&sys_time);
	BM::RtlTimeToSecondsSince1970(&sys_time, &unix_time);

	ZeroMemory(&sys_time, sizeof(LARGE_INTEGER));

	return unix_time;
}

DWORD64 BM::calc_pow_target(DWORD64 TTL,DWORD payloadLength, DWORD payloadLengthExtraBytes, DWORD64 averageProofOfWorkNonceTrialsPerByte)
{
	/*
	oth averageProofOfWorkNonceTrialsPerByte and payloadLengthExtraBytes are set by the owner of a Bitmessage address. 
	The default and minimum for each is 1000. 
	(This is the same as difficulty 1. If the difficulty is 2, then this value is 2000). 
	The purpose of payloadLengthExtraBytes is to add some extra weight to small messages. 
	*/
	ULONGLONG pleb = payloadLength + payloadLengthExtraBytes;
	ULONGLONG ttl_pleb = TTL * pleb;

	ULONGLONG ttl_pleb_2x16 = ttl_pleb / (16 * 16);

	ULONGLONG pleb_2x16 = pleb + ttl_pleb_2x16;

	ULONGLONG target = (64 * 64) / averageProofOfWorkNonceTrialsPerByte * pleb_2x16;

	return target;
}

DWORD64 BM::do_pow(LPBYTE payload, DWORD in_size, DWORD64 TTL)
{
	

	//	payloadLength = the length of payload, in bytes, + 8 (to account for the nonce which we will append later)
	ULONGLONG payloadLength = in_size + 8;

	DWORD64 trialValue = 0xDE0B6B3A763FFFF; // just a high number to start off the loop successfuly
	DWORD64 nonce = 0;

	BYTE initialHash[128] = {};
	BYTE resultHash[128] = {};
	BYTE tmpHash[128] = {};
	BYTE tmp_buff[128] = {};
	
	//===============================================================
	//	calculate pow

	/*
	oth averageProofOfWorkNonceTrialsPerByte and payloadLengthExtraBytes are set by the owner of a Bitmessage address.
	The default and minimum for each is 1000.
	(This is the same as difficulty 1. If the difficulty is 2, then this value is 2000).
	The purpose of payloadLengthExtraBytes is to add some extra weight to small messages.
	*/

	DWORD64 target = calc_pow_target(TTL, payloadLength, 1000, 1000);


	Encryption::create_hash((LPSTR)initialHash, payload, in_size, NULL, NULL, CALG_SHA_512);


	while (trialValue > target)
	{
		nonce = nonce + 1;
		//	resultHash = hash(hash(nonce || initialHash))
		memcpy_s(tmpHash, 8, (LPBYTE)&nonce, 8);
		memcpy_s(&tmpHash[8], 64, initialHash, 64);

		Encryption::create_hash((LPSTR)tmpHash, tmp_buff, 64 + 8, NULL, NULL, CALG_SHA_512);

		Encryption::create_hash((LPSTR)resultHash, tmpHash, 64, NULL, NULL, CALG_SHA_512);

		//	trialValue = the first 8 bytes of resultHash, converted to an integer
		trialValue = *(LPDWORD)resultHash;

		ZERO_(tmp_buff, 128);
		ZERO_(tmpHash, 128);
		ZERO_(resultHash, 128);

	}

	ZERO_(initialHash, 128);

	return nonce; //ot trialValue;

}

DWORD BM::check_pow(LPBYTE payload, DWORD in_size, DWORD64 TTL)
{
	BOOL ret = FALSE;

	BYTE initialHash[128] = {};
	BYTE resultHash[128] = {};
	BYTE tmpHash[128] = {};
	BYTE tmp_buff[128] = {};

	DWORD64 payloadLength = in_size; // payload + 8 byte nonce

	//	nonce = the first 8 bytes of payload
	DWORD64 nonce= *(DWORD64*)payload;


	//  dataToCheck = the ninth byte of payload on down (thus it is everything except the nonce)
	LPBYTE dataToCheck = &payload[8];

	//	initialHash = hash(dataToCheck)
	Encryption::create_hash((LPSTR)initialHash, dataToCheck, in_size - 8, NULL, NULL, CALG_SHA_512);

	//	resultHash = hash(hash(nonce || initialHash))
	*((DWORD64*)tmp_buff) = nonce;

	memcpy_s(&tmp_buff[8], 64, initialHash, 64);

	Encryption::create_hash((LPSTR)resultHash, tmp_buff, 8 + 64, NULL, NULL, CALG_SHA_512);

	//	POWValue = the first eight bytes of resultHash converted to an integer
	DWORD64 POWValue = *(DWORD64*)resultHash;

	//	If POWValue is less than or equal to target, then the POW check passes. 
	DWORD64 target = calc_pow_target(TTL, payloadLength, 1000, 1000);

	//	Do the POW check
	if (POWValue <= target)
		ret = TRUE;

	// cleanup

	ZERO_(tmp_buff, 128);
	ZERO_(tmpHash, 128);
	ZERO_(resultHash, 128);
	ZERO_(initialHash, 128);
	
	return ret;
}

DWORD BM::create_addr(PBM_MSG_ADDR * in)
{
	//							version			stream #
	BYTE chksm[128] = { 0x01,	0x03,	0x01,	0x01 };

	BYTE enc_key_buff[512] = {};
	BYTE sig_key_buff[512] = {};

	BYTE t[512] = {};
	BYTE hash_buff[512] = {};


	LPBYTE tmp_buff = t;
	LPBYTE ripe_hash = NULL;

	PBM_MSG_ADDR addr = NULL;

	
	addr = (PBM_MSG_ADDR)ALLOC_( sizeof(BM_MSG_ADDR));
	*in = addr;
	/*
	
	Create a private and a public key for encryption and signing(resulting in 4 keys)
	Merge the public part of the signing key and the encryption key together. (encoded in uncompressed X9.62 format) (A)
	
	*/

	int i = 0;
	int j = 0;
	DWORD buff_size = 512;
	DWORD tbuff_size = 512;
	DWORD blob_size = 0;
	BOOL found = FALSE;
	DWORD ripe_size = NULL;

	PBCRYPT_ECCKEY_BLOB pblobkey = NULL;

	do {
		tbuff_size = 512;
		tmp_buff = t;

		ZeroMemory(t, 512);
		ZeroMemory(hash_buff, 512);
		
		ZeroMemory(addr->pub_sig_blob, 128);
		ZeroMemory(addr->pub_enc_blob, 128);

		ZeroMemory(addr->prv_sig_blob, 128);
		ZeroMemory(addr->prv_enc_blob, 128);


		addr->enc_handle = NULL;
		addr->sig_handle = NULL;

		DWORD prv_k_s = 128;
		buff_size = 128;

		//	Create SIGN Key
		ECC::create_key_pair(&addr->sig_handle, (PBCRYPT_ECCKEY_BLOB)addr->pub_sig_blob, (PBCRYPT_ECCKEY_BLOB)addr->prv_sig_blob,  &buff_size, &prv_k_s);

		//	Copy to buffer
		memcpy_s(tmp_buff, tbuff_size, sig_key_buff + (ULONG_PTR)8, 64);

		// set new location in buffer
		tmp_buff = tmp_buff + (ULONG_PTR)64;
		tbuff_size = tbuff_size - 64;

		blob_size = 64;

		prv_k_s = 128;
		buff_size = 128;
		

		//	Create Encryption Key
		ECC::create_key_pair(&addr->enc_handle, (PBCRYPT_ECCKEY_BLOB)addr->pub_enc_blob, (PBCRYPT_ECCKEY_BLOB)addr->prv_enc_blob, &buff_size, &prv_k_s);

		blob_size += 64;

		//	Merge with SIGN key. (sign || enc)
		memcpy_s(tmp_buff, tbuff_size, addr->pub_enc_blob + (ULONG_PTR)8, 64);


		//	Take the SHA512 hash of A. (B)
		Encryption::create_hash((LPSTR)hash_buff, t, blob_size, NULL, NULL, CALG_SHA_512);

		ZeroMemory(t, 512);

		//	Take the RIPEMD-160 of B. (C)
		ripmd::calc(addr->hash, hash_buff, 64);

		/*

		Repeat step 1 - 4 until you have a result that starts with a zero(Or two zeros, if you want a short address). (D)
		Remove the zeros at the beginning of D. (E)
		
		*/

		Utils::compress_ripe(addr->hash, 20, &ripe_size);

		// if compress successfull break;
		if (ripe_size && ripe_size < 20)
		{
			break;
		}
		else 
		{
			ZERO_(addr->hash, 20);
			BCryptDestroyKey(addr->enc_handle);
			BCryptDestroyKey(addr->sig_handle);
			continue;
		}


	} while (!found);


	if (found)
	{
		ZeroMemory(t, 512);
		//	Put the stream number(as a var_int) in front of E. (F)
		//	Put the address version(as a var_int) in front of F. (G)

		//memcpy_s(addr->hash, 20, ripe_hash, ripe_size);
		addr->hash_size = ripe_size;

		memcpy_s(&chksm[4], 128 - 4, addr->hash, addr->hash_size);

		//	Take a double SHA512(hash of a hash) of G 
		Encryption::create_hash((LPSTR)t, (LPBYTE)chksm, addr->hash_size + 4, NULL, FALSE, CALG_SHA_512);
		
		ZeroMemory(hash_buff, 512);

		//	and use the first four bytes as a checksum, that you append to the end. (H)
		Encryption::create_hash((LPSTR)hash_buff, (LPBYTE)t, 64, (LPBYTE)&addr->checksum, FALSE, CALG_SHA_512);

		
		// create the "first_tag" for encrypting the public keys for sending(private usage)
		memcpy_s(addr->first_tag, 32, hash_buff, 32);

		// create the "tag" for identification (public usage)
		memcpy_s(addr->tag, 32, &hash_buff[32], 32);



		// clean up
		ZeroMemory(t, 512);
		ZeroMemory(hash_buff, 512);

		// create the address blob to be base58 encoded
		memcpy_s(&chksm[4 + addr->hash_size], 128 - 4 - addr->hash_size, addr->checksum, 4);

	
		//base58 encode H. (J)
		//Put "BM-" in front J. (K)
	
		strcpy_s((char*)hash_buff, 128, "BM-");

		// base58 (address version [01 03] || stream # [01 01] || hash [<20] || chechsum [== 4]

		size_t out_s = 128;
		Encryption::b58enc((char*)hash_buff + (ULONG_PTR)3, &out_s, chksm, 4 + addr->hash_size + 4);

		OutputDebugStringA((LPSTR)hash_buff);

		strcpy_s((char*)addr->readable, 64, (char*)hash_buff);

		ZeroMemory(hash_buff, 128);

		return TRUE;
	}

	return FALSE;
}




//
//
//	Encryption functions
//
//


DWORD BM::encrypt_payload(PBM_MSG_ADDR dest_addr, LPBYTE in_buff, DWORD in_size, PBM_ENC_PL_256 out, LPDWORD out_size)
{

	if (in_size > 100)
		return FALSE;

	DWORD ret = FALSE;
	BCRYPT_HANDLE tmp_crypt_handle = NULL;

	BYTE tmp_buff_a[512] = {};
	BYTE tmp_buff_b[512] = {};
	BYTE tmp_buff_c[512] = {};
	BYTE tmp_buff_d[512] = {};



	PBCRYPT_ECCKEY_BLOB msg_pub_key = (PBCRYPT_ECCKEY_BLOB)tmp_buff_a;
	PBCRYPT_ECCKEY_BLOB msg_priv_key = (PBCRYPT_ECCKEY_BLOB)tmp_buff_b;

	DWORD pub_key_size = 512;
	DWORD priv_key_size = 512;


	DWORD s = NULL;

	//	The destination public key is called K.

	LPBYTE K = (LPBYTE)dest_addr->pub_enc_blob;



	//	Generate 16 random bytes using a secure random number generator. Call it IV.

	CryptGenRandom(Encryption::context, 16, out->iv);

	//	Generate a new random EC key pair with private key called r and public key called R.

	s = 512;
	ECC::create_key_pair(&tmp_crypt_handle, msg_pub_key, msg_priv_key, &pub_key_size, &priv_key_size);

	LPBYTE r = (LPBYTE)msg_priv_key /*+ (ULONG_PTR)8*/;

	LPBYTE R = (LPBYTE)msg_pub_key + (ULONG_PTR)8;


	//
	//
	//
	//	Do an EC point multiply with public key K and private key r.
	//	This gives you public key P.

	LPBYTE P = NULL;
	BCRYPT_KEY_HANDLE n_r_handle = NULL;
	BCRYPT_KEY_HANDLE n_K_handle = NULL;
	BCRYPT_KEY_HANDLE sec_handle = NULL;

	int e = BCryptImportKeyPair(
		ECC::main_handle,						// Provider handle
		NULL,									// Parameter not used
		BCRYPT_ECCPUBLIC_BLOB,					// Blob type (Null terminated unicode string)
		&n_K_handle,							// Key handle that will be recieved								
		K,										// Buffer than points to the key blob
		64 + 8,	// Buffer length in bytes
		NULL);



	e = BCryptImportKeyPair(
		ECC::main_handle,			// Provider handle
		NULL,                       // Parameter not used
		BCRYPT_ECCPRIVATE_BLOB,     // Blob type (Null terminated unicode string)
		&n_r_handle,				// Key handle that will be recieved

		r,							// Buffer than points to the key blob
		priv_key_size,				// Buffer length in bytes
		NULL);


	BCryptSecretAgreement(n_r_handle, n_K_handle, &sec_handle, NULL);

	s = NULL;


	//	Use the X component of public key P and calculate the SHA512 hash H.

	//	BCryptDeriveKey(HMAC, SHA512);

	BCryptBuffer b_list[1] = {};

	BCryptBufferDesc b_params = {};

	b_list[0].BufferType = KDF_HASH_ALGORITHM;
	b_list[0].cbBuffer = (DWORD)((wcslen(BCRYPT_SHA512_ALGORITHM) + 1) * sizeof(WCHAR));
	b_list[0].pvBuffer = BCRYPT_SHA512_ALGORITHM;

	b_params.cBuffers = 1;
	b_params.pBuffers = b_list;
	b_params.ulVersion = BCRYPTBUFFER_VERSION;
	
	BYTE H[70] = {};


	BCryptDeriveKey(sec_handle, BCRYPT_KDF_HASH, &b_params, H, 70, &s, NULL);

	//	The first 32 bytes of H are called 'key_e' and the last 32 bytes are called 'key_m'.

	BYTE key_e[32] = {};
	BYTE key_m[32] = {};

	memcpy_s(key_e, 32, H, 32);
	memcpy_s(key_m , 32, H + (ULONG_PTR)32, 32);

	ZeroMemory(H, 70);

	s = 0;

	//	Pad the input text to a multiple of 16 bytes, in accordance to PKCS7.

	//DWORD new_pad_len = 128;// TMP_BLOCK_BUFFER_SIZE(in_size);
	
	LPBYTE padded_buff =  tmp_buff_d;
	
	ZeroMemory(padded_buff, 512);
	
	memcpy_s(padded_buff, 512, in_buff, in_size);

	//	Encrypt the data with AES - 256 - CBC, using IV as initialization vector
	//	key_e as encryption key 
	//	the padded input text as payload.
	//	Call the output cipher text.
	
	CRYPT_CONTEXT_ context = {};

	//	 signature context
	CRYPT_CONTEXT_ sig_context = {};

	sig_context.context = Encryption::context;
	sig_context.aes_key_size = AES_KEY_SIZE_;

	memcpy_s(sig_context.aes_key, AES_KEY_SIZE_, key_m, AES_KEY_SIZE_);

	Encryption::aes_import_key(&sig_context);

	// Encryption context
	context.context = Encryption::context;
	context.aes_key_size = AES_KEY_SIZE_;
	
	memcpy_s(context.aes_key, AES_KEY_SIZE_, key_e, AES_KEY_SIZE_);

	context.in_buff = padded_buff;
	context.in_size = 127;// in_size;

	memcpy_s(context.iv, 16, out->iv, 16);


	//	Import the key_e in to the WINCAPI
	Encryption::aes_import_key(&context);

	Encryption::aes_encrypt(&context);
	// make sure to release the context.out_buff

	//	Calculate a 32 byte MAC with HMACSHA256, using key_m as salt and IV + R + cipher text as data.Call the output MAC.

	//LPBYTE sig_buff = tmp_buff_d;

	//DWORD sig_buff_size = 16 + 32 + 32 + 128;
	DWORD sig_buff_size = 16 + 32 + 32 + context.out_size;


	LPBYTE sig_buff = (LPBYTE)ALLOC_( sig_buff_size + 1);

	ZeroMemory(sig_buff, sig_buff_size);

	//if (context.out_size > 512)
	//	return FALSE;

	memcpy_s(sig_buff, 16, out->iv, 16);

	memcpy_s(&sig_buff[16], 64, R, 64);

	memcpy_s(&sig_buff[16 + 32 + 32], 128, context.out_buff, context.out_size);


	LPBYTE MAC = (LPBYTE)Encryption::create_hmac(Encryption::context, sig_buff, sig_buff_size, sig_context.aes_hKey);

	// Build the Encryption blob
	// I use a static buffer to send the text

	// out->iv already set

	if (*out_size < sizeof(BM_ENC_PL_256) + context.out_size + HMAC_LEN)
	{
		ret = FALSE;
		goto clean_up;
	}

	out->curve_type = htons(0x02CA);
	
	out->x_len = htons(32);
	
	memcpy_s(out->x, 32, R, 32);

	out->y_len = htons(32);

	memcpy_s(out->y, 32, &R[32], 32);

	memcpy_s(out->ciph_text, context.out_size, context.out_buff, context.out_size); // we keep a constant message size if the message is MAX 350~ then just make static ...

	memcpy_s(&out->ciph_text[context.out_size], HMAC_LEN, MAC, HMAC_LEN);

	*out_size = sizeof(BM_ENC_PL_256) + context.out_size + HMAC_LEN;

	///------------
	//	Success
	///------------
	ret = TRUE;
clean_up:


	if (sec_handle)
		BCryptDestroySecret(sec_handle);

	sec_handle = NULL;

	if (context.aes_hKey)
		CryptDestroyKey(context.aes_hKey);

	if (sig_context.aes_hKey)
		CryptDestroyKey(context.aes_hKey);


	if (context.out_buff && context.out_size)
	{
		ZEROFREE_(context.out_buff, context.out_size);
	}

	ZeroMemory(&context, sizeof(CRYPT_CONTEXT_));
	ZeroMemory(&sig_context, sizeof(CRYPT_CONTEXT_));

	if (sig_buff)
	{
		ZEROFREE_(sig_buff, sig_buff_size);
		sig_buff = NULL;
	}

	if (MAC)
	{
		ZEROFREE_(MAC, HMAC_LEN);
		MAC = NULL;
	}

	return ret;
}

DWORD BM::decrypt_payload(PBM_MSG_ADDR recv_addr, PBM_ENC_PL_256 in_buff, DWORD in_size)
{
	if (in_size <= sizeof(BM_ENC_PL_256) + 32 + 16)
		return FALSE;

	
	DWORD enc_pl_size = in_size - sizeof(BM_ENC_PL_256) + 32;
	
	//	Dynamic payload size so we must locate the make.........stupid as fuck
	LPBYTE tgt_mac = &in_buff->ciph_text[enc_pl_size];


	BYTE tmp_buff_a[512] = {};

	//	The private key used to decrypt is called k.

	LPBYTE k = (LPBYTE)recv_addr->prv_enc_blob;


	//	Do an EC point multiply with private key k and public key R.This gives you public key P.
	//	Use the X component of public key P and calculate the SHA512 hash H.

	PBCRYPT_ECCKEY_BLOB pub_blob = (PBCRYPT_ECCKEY_BLOB)tmp_buff_a;

	pub_blob->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
	pub_blob->cbKey = htons(in_buff->x_len);

	memcpy_s(&tmp_buff_a[8], 32, in_buff->x, 32);
	memcpy_s(&tmp_buff_a[8 + 32], 32, in_buff->y, 32);

	BCRYPT_ALG_HANDLE pub_handle = NULL;
	BCRYPT_ALG_HANDLE priv_handle = NULL;
	BCRYPT_SECRET_HANDLE sec_handle = NULL;


	int e = BCryptImportKeyPair(
		ECC::main_handle,						// Provider handle
		NULL,									// Parameter not used
		BCRYPT_ECCPUBLIC_BLOB,					// Blob type (Null terminated unicode string)
		&pub_handle,							// Key handle that will be recieved								
		(PUCHAR)pub_blob,						// Buffer than points to the key blob
		64 + 8,									// Buffer length in bytes
		NULL);


	e = BCryptImportKeyPair(
		ECC::main_handle,						// Provider handle
		NULL,									// Parameter not used
		BCRYPT_ECCPRIVATE_BLOB,					// Blob type (Null terminated unicode string)
		&priv_handle,							// Key handle that will be recieved								
		(PUCHAR)recv_addr->prv_enc_blob,						// Buffer than points to the key blob
		32 + 32 + 32 + 8,									// Buffer length in bytes
		NULL);

	BCryptSecretAgreement(priv_handle, pub_handle, &sec_handle, NULL);

	DWORD s = NULL;


	//	Use the X component of public key P and calculate the SHA512 hash H.

	//	BCryptDeriveKey(HMAC, SHA512);
	
	BYTE H[70] = {};
	BCryptBuffer b_list[1] = {};
	BCryptBufferDesc b_params = {};


	b_list[0].BufferType = KDF_HASH_ALGORITHM;
	b_list[0].cbBuffer = (DWORD)((wcslen(BCRYPT_SHA512_ALGORITHM) + 1) * sizeof(WCHAR));
	b_list[0].pvBuffer = BCRYPT_SHA512_ALGORITHM;


	b_params.cBuffers = 1;
	b_params.pBuffers = b_list;
	b_params.ulVersion = BCRYPTBUFFER_VERSION;

	
	BCryptDeriveKey(sec_handle, BCRYPT_KDF_HASH, &b_params, H, 70, &s, NULL);

	//	The first 32 bytes of H are called key_e and the last 32 bytes are called key_m.

	BYTE key_e[BM_TAG_LEN] = {};
	BYTE key_m[BM_TAG_LEN] = {};

	memcpy_s(key_e, BM_TAG_LEN, H, BM_TAG_LEN);
	memcpy_s(key_m, BM_TAG_LEN, &H[BM_TAG_LEN], BM_TAG_LEN);

	ZeroMemory(H, 70);

	//	Calculate MAC with HMACSHA256, using key_m as salt and IV + R + cipher text as data.



	DWORD sig_buff_size = 16 + 32 + 32 + enc_pl_size;

	LPBYTE sig_buff = (LPBYTE)ALLOC_( sig_buff_size);

	memcpy_s(sig_buff, 16, in_buff->iv, 16);							//	IV
	memcpy_s(&sig_buff[16], 32, in_buff->x, 32);						//	R
	memcpy_s(&sig_buff[16 + 32], 32, in_buff->y, 32);					//	"
	memcpy_s(&sig_buff[16 + 32 + 32], enc_pl_size, in_buff->ciph_text, enc_pl_size);//	cipher_text


	CRYPT_CONTEXT_ sig_context = {};

	sig_context.context = Encryption::context;
	memcpy_s(sig_context.aes_key, AES_KEY_SIZE_, key_m, AES_KEY_SIZE_);

	Encryption::aes_import_key(&sig_context);

	LPBYTE MAC = (LPBYTE)Encryption::create_hmac(Encryption::context, sig_buff, sig_buff_size, sig_context.aes_hKey);

	//	Compare MAC with MAC. If not equal, decryption will fail.


	if (memcmp(tgt_mac, MAC, HMAC_LEN))
	{
		OutputDebugStringA("HMAC Failed to validate.");
		return FALSE;
	}


	//	Decrypt the cipher text with AES - 256 - CBC, using IV as initialization vector, key_e as decryption key and the cipher text as payload.The output is the padded input text.

	CRYPT_CONTEXT_ context = {};
	
	context.context = Encryption::context;
	
	memcpy_s(context.aes_key, AES_KEY_SIZE_, key_e, AES_KEY_SIZE_);
	
	context.aes_key_size = AES_KEY_SIZE_;
	context.in_buff = in_buff->ciph_text;
	context.in_size = enc_pl_size;// in_size - 106;// 106 is the size of static data in the payload blob
	
	memcpy_s(context.iv, 16, in_buff->iv, 16);

	
	Encryption::aes_import_key(&context); // import the key and store in context->aes_hkey

	Encryption::aes_decrypt(&context); //	decrypt the buffer and create new in context->out_buff
	

	ZeroMemory(in_buff->ciph_text, enc_pl_size);
	
	memcpy_s(in_buff->ciph_text, context.out_size, context.out_buff, context.out_size);// upon succes, copy the decrypted buffer in the old cipher text buffer

	
	///------------
	//	SUCCESS
	///------------

	if (sec_handle)
		BCryptDestroySecret(sec_handle);

	sec_handle = NULL;

	if (context.aes_hKey)
		CryptDestroyKey(context.aes_hKey);

	if (sig_context.aes_hKey)
		CryptDestroyKey(context.aes_hKey);


	if (context.out_buff && context.out_size)
	{
		ZEROFREE_(context.out_buff, context.out_size);
	}

	ZeroMemory(&context, sizeof(CRYPT_CONTEXT_));
	ZeroMemory(&sig_context, sizeof(CRYPT_CONTEXT_));

	if (sig_buff)
	{
		ZEROFREE_(sig_buff, sig_buff_size);
		sig_buff = NULL;
	}

	if (MAC)
	{
		ZEROFREE_(MAC, HMAC_LEN);
		MAC = NULL;
	}

	return TRUE;
}




//
//
//	Object handling functions
//
//

DWORD BM::process_object(PBM_OBJECT object, DWORD object_size)
{

	if (object_size > sizeof(BM_OBJECT))
		return FALSE;


	//	TO DO
	//
	//	Store the object as a vector if it doesnt already exist.
	//
	//


	PBM_MSG_HDR msg_hdr = NULL;
	DWORD msg_hdr_s = BM_OPK_BS;

	PBM_OBJECT obj_hdr = NULL;
	DWORD obj_hdr_s = BM_OPK_BS - sizeof(BM_MSG_HDR);

	LPBYTE pl = NULL;
	DWORD payload_size = object_size - sizeof(BM_OBJECT);

	switch (object->objectType)
	{

	case BM_OBJ_GETPUBKEY:
	{
		// search seach of objects for the matching key
		// return the keys via object pubkey
		// otherwise propogate request
		LPBYTE t = (LPBYTE)ALLOC_(BM_OPK_BS);

		msg_hdr = (PBM_MSG_HDR)t;

		obj_hdr = (PBM_OBJECT)&t[sizeof(BM_MSG_HDR)];

		pl = &t[sizeof(BM_MSG_HDR) + sizeof(BM_OBJECT)];

		DWORD pl_size = BM_OPK_BS - sizeof(BM_MSG_HDR) + sizeof(BM_OBJECT);

		DWORD found = BM::obj_getpubkey(object, object_size, pl, &pl_size);


		if (found)
		{

			BM::init_object(obj_hdr, obj_hdr_s, pl, pl_size);

			//	TO DO	
			//	store the object as a vector if successfully sent out
			//

			// the payload size in reference to the MSG header, is the size of the object header + payload size
			pl_size += sizeof(BM_OBJECT);

			BM::init_msg_hdr(msg_hdr, sizeof(BM_OBJECT) + pl_size, "object");


			// propogate the newly found pubkeys.


		}
		else

		{
			// if not found propgate the getpubkey object
			ZEROFREE_(t, BM_OPK_BS);
		}


		//ZEROFREE_(t, BM_OPK_BS);
		break;

	}

	case BM_OBJ_PUBKEY:

		// receive a public key
		// attempt to locate
		// if found then decrypt using the priv_tag and ECDH
		// otherwise propgate the pubkey object


		//BM::obj_pubkey((PBM_PUBKEY_V3_OBJ)object->objectPayload, payload_size);
		break;

	case BM_OBJ_MSG:
		//attempt to decrypt the msg, if not then propgate.
		// BM::obj_msg();
		break;

	case BM_OBJ_BROADCAST:
		//NOT SUPPORTED
		//BM::obj_broadcast();
		break;


	default:

		//EPIC FAIL

		break;
	}




	// If there if and object to propgate then do so here
	//
	//
	//
	//
	//
	//




	return TRUE;
}


DWORD BM::obj_getpubkey(PBM_OBJECT in, DWORD in_size, LPBYTE pl, LPDWORD out_size)// difference between v3-4 is encryption
{

	BYTE tmp_buff_a[64] = {};

	DWORD hash_size = 0;
	LPBYTE hash = tmp_buff_a;

	size_t ver_len = 0;
	uint64_t vers = BM::decodeVarint(in->objectVersion, 4, &ver_len);
	PBM_MSG_ADDR address_info = NULL;
	LPVOID payl = in->objectPayload;

	//	What version is the pubkey object

	if (vers <= 3)
	{
		memcpy_s(hash, 20, ((PBM_GETPUBKEY_OBJ)payl)->ripe, 20);
		hash_size = 20;
	}
	else if(vers >= 4)
	{
		memcpy_s(hash, 32, ((PBM_GETPUBKEY_V4_OBJ)payl)->tag, 32);
		hash_size = 32;
	}

	//
	//
	//	search for a matching public key
	//
	//	using the vector list

	address_info = BM::find_address(hash);

	//
	//	start to build the packet
	//

	ZERO_(tmp_buff_a, 64);

	BYTE temp_obj[512] = {};
	BYTE tags[64] = {};
	PBM_PUBKEY_V3_OBJ tb = NULL;
	LPBYTE priv_tag = NULL;
	LPBYTE pub_tag = NULL;

	DSA_CONTEXT dsa_context = {};
	DWORD j = NULL;

	if (address_info)
	{

		//	Here we set up the ECDSA signature buffer

		memcpy_s(temp_obj, 8, &in->expiresTime, 8);
		memcpy_s(&temp_obj[8], 4, &in->objectType, 4);
		memcpy_s(&temp_obj[8 + 4], 2, in->objectVersion, 2);
		memcpy_s(&temp_obj[8 + 4 + 2], 2, in->streamNumber, 2);


		tb = (PBM_PUBKEY_V3_OBJ)&temp_obj[8 + 4 + 2 + 2];

		// More ECDSA but we will use this struct to encrypt + send out as well.

		//	https://bitmessage.org/wiki/Protocol_specification#Pubkey_bitfield_features (optional)
		tb->behavior = NULL;// 29 	extended_encoding 	// 30 	include_destination // 31 	does_ack // 
		
		
		memcpy_s(tb->sign_key, 64, &address_info->pub_sig_blob[8], 64);
		
		memcpy_s(tb->enc_key, 64, &address_info->pub_enc_blob[8], 64);

		memcpy_s(tb->nonce_trials_per_byte, 3, "\x2\x10\x00", 3);	//
		
		memcpy_s(tb->extra_bytes, 3, "\x2\x10\x00", 3);			// var int 1000. default
		

		//
		//
		//	Create the tags from the public keys
		//______________________________________
		//	only do this if one is not stored.
		//
		//	w.i.p


		ZERO_(tags, 64);


		BM::create_tags(&address_info->pub_enc_blob[8], &address_info->pub_sig_blob[8], tags);


		// use the tags appropriatly
		priv_tag = tags;
		pub_tag = &tags[BM_TAG_LEN];

		//	the priv_tag was not initially in the address info
		//	lets put it there now.

		memcpy_s(address_info->first_tag, BM_TAG_LEN, priv_tag, BM_TAG_LEN);

		//
		//	start ECDSA signature stuffz
		//	____________________________
		//
		//

		ZERO_(tmp_buff_a, 64);

		dsa_context.buffer = temp_obj;
		dsa_context.sig_size = DWORD((ULONG_PTR)&tb->sig - (ULONG_PTR)temp_obj); // i want to say 150 static. but we should be dynamic
		
		dsa_context.private_key = priv_tag;
		dsa_context.priv_key_size = BM_TAG_LEN;

		dsa_context.out_sig = tmp_buff_a;
		dsa_context.sig_size = 64;

		// Create the ECDSA signature

		j = ECC::create_dsa_sig(&dsa_context);

		if (j)
			return FALSE;
		
		//	success
		//	insert the ECDSA signature.
		
		

		memcpy_s(tb->sig, 2, "\x01\x64", 2); // length of sig in decimal(varint encoding)
		memcpy_s(&tb->sig[2], 64, dsa_context.out_sig, 64);
	
		ZERO_(tmp_buff_a, 64);
		// Encrypt the pubkeys...finally..
	
		//BM_ENC_PL_256 pl = {};
		DWORD payload_len = NULL;

		BM_MSG_ADDR km = {};
		BCRYPT_KEY_HANDLE kh = NULL;
		BYTE kb[128] = {};

		((PBCRYPT_ECCKEY_BLOB)kb)->dwMagic = BCRYPT_ECDSA_PRIVATE_P256_MAGIC;
		((PBCRYPT_ECCKEY_BLOB)kb)->cbKey = 32;

		//	Insert the priv_tag to the ecc key blob
		memcpy_s(&kb[8 + 64], 32, priv_tag, 32);

		//	importing the key blob gives us our public key.
		BCryptImportKeyPair(ECC::main_handle, NULL, BCRYPT_ECCPRIVATE_BLOB, &kh, kb, 8 + 64 + 32, NULL);

		ZERO_(kb, 128);

		//	Retrieve the public key
		BCryptExportKey(ECC::main_handle, NULL, BCRYPT_ECCPUBLIC_BLOB, kb, 128, &j, NULL);

		//	encrypt the pubkey struct in to a payload struct.
		memcpy_s(km.pub_enc_blob, 64, &kb[8], 64);

		memcpy_s(pl, BM_TAG_LEN, pub_tag, BM_TAG_LEN);

		// create a "tagged envelope" as the payload
		PBM_ENC_PL_256 out_pl = (PBM_ENC_PL_256)&pl[32];

		BM::encrypt_payload(0, (LPBYTE)tb, sizeof(BM_PUBKEY_V3_OBJ) + 2 + 64, out_pl, &payload_len);

		ZERO_(kb, 128);

		if (kh)
			BCryptDestroyKey(kh);


		*out_size = BM_TAG_LEN + payload_len;

	}


	ZERO_(tags, 64);

	return TRUE;

}


DWORD BM::init_object(PBM_OBJECT out_obj, DWORD out_size, LPBYTE pl, DWORD payload_len)
{

	out_obj->expiresTime = BM::unix_time() + DAY_SECONDS(3);
	out_obj->nonce = BM::calc_pow_target(DAY_SECONDS(3), payload_len, 1000, 1000);
	out_obj->objectType = BM_OBJ_PUBKEY;
	

	memcpy_s(out_obj->objectVersion, 2, "\x1\x04", 2);
	memcpy_s(out_obj->streamNumber, 2, "\x1\x01", 2);

	memcpy_s(out_obj->objectPayload, out_size, pl, payload_len);

	return TRUE;
}


DWORD BM::create_tags(LPBYTE enc, LPBYTE sig, LPBYTE out)
{

	BYTE t_buff_a[128] = {};
	BYTE t_buff_b[128] = {};
	DWORD new_ripe_s = NULL;

	memcpy_s(t_buff_a, 64, enc, 64);
	memcpy_s(&t_buff_a[64], 64, sig, 64);


	// Step one create SHA512 of the public keys
	Encryption::create_hash((LPSTR)t_buff_b, t_buff_a, 64 + 64, NULL, NULL, CALG_SHA_512);
	ZERO_(t_buff_a, 128);

	//	Take the RIPEMD-160 of B. (C)
	ripmd::calc(t_buff_a, t_buff_b, 64);

	ZERO_(t_buff_b, 64);

	// remove leading 00s
	Utils::compress_ripe(t_buff_a, 20, &new_ripe_s);

	//version
	t_buff_b[0] = 0x01; t_buff_b[1] = 0x04;

	// stream number
	t_buff_b[3] = 0x01; t_buff_b[4] = 0x01;

	// copy the compressed ripe
	memcpy_s(&t_buff_b[5], 64 - 4, t_buff_a, new_ripe_s);

	ZERO_(t_buff_a, 64);

	// create the double sha512;
	Encryption::create_hash((LPSTR)t_buff_a, t_buff_b, new_ripe_s + 2 + 2, NULL, NULL, CALG_SHA_512);

	ZERO_(t_buff_b, 64);
	// "
	Encryption::create_hash((LPSTR)t_buff_b, t_buff_a, 64, NULL, NULL, CALG_SHA_512);
	
	ZERO_(t_buff_a, 64);

	memcpy_s(out, 64, t_buff_b, 64);

	ZERO_(t_buff_b, 64);

	return TRUE;
}


PBM_MSG_ADDR BM::find_address(LPBYTE pub_tag)
{
	LPBYTE vect_hash = NULL;
	DWORD found = FALSE;

	PBM_VECTOR vector = NULL;
	PBM_OBJECT object = NULL;
	PBM_MSG_ADDR address_info = NULL;

	//	Loop through the list to find an set of public keys matching our hash
	for (int i = 0; i < BM_N_VECT_SLOTS; i++)
	{

		vector = BM::vector_list->list[i];

		object = (PBM_OBJECT)&vector->obj;


		if (object->objectType == BM_OBJ_PUBKEY)
		{

			address_info = (PBM_MSG_ADDR)object->objectPayload;

			if (!memcmp(pub_tag, address_info->tag, 32))
			{
				found = i;
				break;
			}

			address_info = NULL;

		}

	}

	return address_info;

}



//
//
//	Connections functions
//
//

DWORD BM::init_msg_hdr(PBM_MSG_HDR in, DWORD pl_s, LPSTR command)
{

	DWORD ret = FALSE;

	BYTE m[4] = { 0xE9,0xBE, 0xB4, 0xD9 }; // magic
	memcpy_s(in->magic, 4, m, 4);

	lstrcpynA((char*)in->command, command, lstrlenA(command));

	TYPECH(uint32_t, in->length, htonl(pl_s));
	
	CHAR hash[MAX_PATH] = {};

	Encryption::create_hash(hash, in->payload, pl_s, in->checksum, FALSE, CALG_SHA_512);

	ZeroMemory(hash, MAX_PATH);

	return ret;

}



DWORD BM::init_con(PBM_MSG_HDR* in_, long toip, uint16_t toport)
{
	PBM_MSG_HDR in = (PBM_MSG_HDR)ALLOC_(sizeof(BM_MSG_HDR) + sizeof(BM_PL_VER) + 1);
	PBM_PL_VER ver = (PBM_PL_VER)in->payload;

	BYTE m[4] = { 0xE9,0xBE, 0xB4, 0xD9 };
	memcpy_s(in->magic, 4, m, 4);
	
	lstrcpynA((char*)in->command, "version", 12);

	TYPECH(uint32_t, in->length, htonl(sizeof(BM_PL_VER)));



	BM::init_ver(ver, toip, toport);

	CHAR hash[MAX_PATH] = {};

	Encryption::create_hash(hash, in->payload, sizeof(BM_PL_VER), in->checksum, FALSE, CALG_SHA_512);

	ZERO_(hash, MAX_PATH);

	*in_ = in;


	return (sizeof(BM_MSG_HDR) + sizeof(BM_PL_VER));
}


DWORD BM::init_ver(PBM_PL_VER version_pl, long ip_to, uint16_t port_to)
{
	// THIS CLIENT SUPPORTS V3 ONLY


	uint64_t _time = BM::unix_time();

	PBM_NET_ADDR from_ip = &version_pl->addr_from;
	PBM_NET_ADDR recv_ip = &version_pl->addr_recv;

	// we using version 3 ~
	(*(uint32_t*)version_pl->version) = htonl((uint32_t)3);
	
	uint64_t serv = (uint64_t)1;

	// not using SSL
	(*(uint64_t*)version_pl->services) = swap64(serv);

	//	timestamp
	(*(uint64_t*)version_pl->timestamp) = swap64(_time);


	uint16_t portn = htons(8444);
	
	set_net_addr(from_ip, htonl(inet_addr("127.0.0.1")), false, false, portn, serv, _time);
	
	
	set_net_addr(recv_ip, ip_to, false, false, port_to, serv, _time);

	//	random nonce
	TYPECH(uint64_t, version_pl->nonce, 0x11eeffdd446633aa); // FIX ME use random value ???

	//	not using  user agent
	TYPECH(BYTE, version_pl->user_agnt, 0x00);

	//	only one stream, and its stream # one. hence 01 01
	TYPECH(WORD, version_pl->streams, 0x0101);

	return FALSE;
}

DWORD BM::verify_version(PBM_PL_VER in)
{
	if (!in)
		return FALSE;
	
	uint32_t version = (*(uint32_t*)in->version);

	if (version < 3) 
		return FALSE;

	uint64_t time = BM::unix_time();
	uint64_t ver_time = swap64((*(uint64_t*)in->timestamp));

	if (time >= ver_time && (time - ver_time) <= DAY_SECONDS(3))
		return TRUE;


	return FALSE;
}

DWORD BM::init_verack(PBM_MSG_HDR in)
{

	TYPECH(int, in->magic, BM_MAGIC);

	memset(in->command, NULL, 12);

	lstrcpynA((char*)in->command, "verack", 12);

	memcpy_s(in->checksum, 4, "\xCF\x83\xE1\x35", 4);

	TYPECH(int, in->length, 0);

	return FALSE;

}

DWORD BM::set_net_addr(PBM_NET_ADDR in, long char_ip, long* long_ip, BOOL ipv6, uint16_t port, uint64_t services, int time)
{

	struct ip_ {
		BYTE a[10];
		BYTE b[2];
		BYTE c[4];
	};

	ip_ _ip = {};

	ZeroMemory(&_ip, sizeof(ip_));

	_ip.b[0] = 0xFF;
	_ip.b[1] = 0xFF;

	long hostname = NULL;
	LPBYTE net_addr = NULL;

	if (char_ip)
	{
		//	convert from string to netowrk byte order (from the initial list of IPs)
		hostname = char_ip;

		TYPECH(long, _ip.c, hostname);

		net_addr = (LPBYTE)&_ip;
	}
	else if (ipv6)
	{
		// if ipv6 then we recieved from a node
		// meaning we gave a pointer to the 16 bytes to this funtion
		net_addr = (LPBYTE)long_ip;
	}
	else if (!ipv6 && long_ip) {

		//	if not ipv6 but we received an address already formatted
		//	then
		hostname = *long_ip;
		TYPECH(long, _ip.c, hostname);
		net_addr = (LPBYTE)&_ip;
	}

	//	set time
	//TYPECH(int, in->time, time);

	//	set stream (currently always 1)
	//TYPECH(int, in->stream, 1);

	//	set the services
	TYPECH(uint64_t, in->services, swap64(services));

	//	set the ip address
	//	copy the IP bytes in the the struct
	memcpy_s(in->ip, 16, net_addr, 16);

	//	set network port
	TYPECH(uint16_t, in->port, port);


	return FALSE;
}








//=================================
// https://techoverflow.net/blog/2013/01/25/efficiently-encoding-variable-length-integers-in-cc/
// thanks much appreciated

size_t BM::encodeVarint(uint64_t value, uint8_t* output) {
	size_t outputSize = 0;
	//While more than 7 bits of data are left, occupy the last output byte
	// and set the next byte flag
	while (value > 127) {
		//|128: Set the next byte flag
		output[outputSize] = ((uint8_t)(value & 127)) | 128;
		//Remove the seven bits we just wrote
		value >>= 7;
		outputSize++;
	}
	output[outputSize++] = ((uint8_t)value) & 127;
	return outputSize;
}

uint64_t BM::decodeVarint(uint8_t* input, size_t inputSize, size_t* int_len) {
	uint64_t ret = 0;
	for (size_t i = 0; i < inputSize; i++) {
		ret |= (input[i] & 127) << (7 * i);
		//If the next-byte flag is set
		if (!(input[i] & 128))
		{
			*int_len = i + 1;
			break;
		}
	}
	return ret;
}
//===============================================


DWORD BM::encodeVarstr(char* in, LPBYTE out, DWORD out_size)
{
	if (!in || !out || !out_size)
		return FALSE;


	LPBYTE buff = out;
	DWORD str_len = lstrlenA(in);

	DWORD var_int_size = BM::encodeVarint(str_len, buff);
	DWORD rem_buff_size = out_size - var_int_size;

	if (var_int_size + str_len > out_size)
	{
		ZeroMemory(out, out_size);
		return FALSE;
	}

	buff += (ULONG_PTR)var_int_size;

	strcpy_s((char*)buff, rem_buff_size, in);

	return str_len + var_int_size;
}

DWORD BM::decodeVarstr(char* in, int in_size, char* out, int out_size)
{

	if (!in || !in_size || !out || !out_size)
		return FALSE;

	size_t int_len = NULL;

	size_t str_len = BM::decodeVarint((uint8_t*)in, in_size, & int_len);
	
	strcpy_s((char*)out, out_size, in + (ULONG_PTR)int_len);

	return str_len;

}


DWORD64 BM::var_net_list(LPBYTE in, size_t in_size, PBM_NET_ADDR* out)
{

	size_t int_len = NULL;

	DWORD64 n = NULL;
	
	n = BM::decodeVarint(in, in_size, &int_len);
	
	*out = (PBM_NET_ADDR)in + (ULONG_PTR)int_len;

	return n;
}




//
//
//	Vector List handling functions
//
//

PBM_VECTOR BM::list_add_vector(LPBYTE vect)
{
	CHAR tmp_v[MAX_PATH] = {};
	PBM_VECTOR v = BM::list_find_vector(vect);
	DWORD v_size = sizeof(BM_VECTOR);

	if (v) return FALSE;

	v = (PBM_VECTOR)ALLOC_(BM_VECT_BUFF_SIZE);
	ZERO_(v, BM_VECT_BUFF_SIZE);

	for (DWORD i = 0; i < 32; i++)
	{
		wsprintfA((LPSTR)tmp_v, "%x", vect[i]);
		lstrcatA((LPSTR)v, tmp_v);

		ZERO_(tmp_v, MAX_PATH);

	}

	DBGOUTw(L"\rAdding Vector - ");
	DBGOUTa((LPCTSTR)v);
	DBGOUTw(L"\r");


	ZERO_(v, BM_VECT_BUFF_SIZE);

	for (int i = 0; i < BM_N_VECT_SLOTS; i++)
	{
		
		if (!BM::vector_list->list[i])
		{
			memcpy_s(v->hash, 32, vect, 32);
			v->vector_size = BM_VECT_BUFF_SIZE;
			BM::vector_list->list[i] = v;
			return v;

		}
	}

	ZEROFREE_(v, BM_VECT_BUFF_SIZE);

	return FALSE;
}


PBM_VECTOR BM::list_find_vector(LPBYTE vect)
{
	for (int i = 0; i < BM_N_VECT_SLOTS; i++)
	{
		if (!BM::vector_list->list[i]) continue;

		if (!memcmp(BM::vector_list->list[i]->hash, vect, 32))
		{
			return BM::vector_list->list[i];
		}
	}
	return FALSE;

}


DWORD BM::list_remove_vector(LPBYTE vect)
{
	for (int i = 0; i < BM_N_VECT_SLOTS; i++)
	{
		if (!BM::vector_list->list[i]) continue;

		if (!memcmp(BM::vector_list->list[i]->hash, vect, 32))
		{

			ZEROFREE_(BM::vector_list->list[i], BM::vector_list->list[i]->vector_size);
			BM::vector_list->list[i] = NULL;

			return TRUE;
		}
	}
	return FALSE;
	
}


DWORD BM::list_count_vector()
{
	DWORD count = 0;
	for (int i = 0; i < BM_N_VECT_SLOTS; i++)
	{
		if (BM::vector_list->list[i]->hash[0])
		{
			count++;
		}
	}
	return count;
}

// make cleaup function fo when the process shuts down

// This handles the 'addr' message
// it takes the list and trys to connect to each peer in the list.
// also stores the nodes in a seperate list.


DWORD BM::receive_addr_list(LPBYTE payload, DWORD in_size)
{

	if (!payload)
		return FALSE;

	size_t int_len = 0;
	uint64_t n_entrys = BM::decodeVarint(payload, 4, &int_len);

	DWORD offset = sizeof(BM_ADDR);
	LPBYTE list = &payload[int_len]; // get position of the list after the Var Int.
	PBM_ADDR addr = NULL;

	ULONGLONG cur_time = BM::unix_time();
	ULONGLONG fwd_time = cur_time + 60 * 60 * 3;
	ULONGLONG bck_time = cur_time - DAY_SECONDS(3);

	ULONGLONG addr_time = NULL;
	ULONGLONG services = NULL;

	PBM_CONN conn = NULL;

	uint32_t ip = NULL;
	uint16_t port = NULL;


	if (n_entrys > 1000)
		return FALSE;

	for (int i = 0; i < n_entrys; i++)
	{
		conn = NULL;
		addr = (PBM_ADDR)list;

		// correct endianess
		addr_time = BM::swap64((*(uint64_t*)addr->time));
		services = BM::swap64((*(uint64_t*)addr->services));
		ip = htonl(*(uint32_t*)&addr->ip[12]);
		port = htons(*(uint16_t*)addr->port);

		//	Validate time
		if ((addr_time < fwd_time) && (addr_time > bck_time))
		{

			// validate the services
			if ((services & BM_NODE_NETWORK) == BM_NODE_NETWORK)
			{

				// add the node to the list
				network::list_add_node(addr);

				// attempt a connection to the node.
				// add to the connection list if successful
				network::connect(BM::main_hwnd, &conn, ip, port);

			}
		}


		// enumerate the list.
		list = &list[offset * i];

	}

}


#endif
