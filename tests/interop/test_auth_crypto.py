#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent OpenSSL-backed peer checks for the actual C auth/cipher code.

Requires Python cryptography and an existing build/mbedtls/libmbedcrypto.a.
No phone, coprocessor, private production key or real pair record is used.
"""
import ctypes as C
import datetime
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

from cryptography import x509
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa, utils, x25519
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.x509.oid import NameOID

ROOT = Path(__file__).resolve().parents[2]
SIZE = C.c_size_t
SIGN = C.CFUNCTYPE(C.c_int, C.c_void_p, SIZE, C.c_void_p, SIZE, C.POINTER(SIZE))
CERT = C.CFUNCTYPE(C.c_int, C.c_void_p, SIZE, C.POINTER(SIZE))
RANDOM = C.CFUNCTYPE(C.c_int, C.c_void_p, SIZE, C.c_void_p)


class Mfi(C.Structure):
    _fields_ = [("info", C.c_void_p), ("sign", SIGN), ("read_cert", CERT)]


class Provider(C.Structure):
    _fields_ = [("mfi", C.POINTER(Mfi)), ("major", C.c_uint8),
                ("random", RANDOM), ("context", C.c_void_p)]


def check_auth(lib):
    lib.airplay_mfi_sap_set_provider.argtypes = [C.POINTER(Provider)]
    lib.airplay_mfi_sap_set_provider.restype = C.c_bool
    lib.airplay_mfi_sap_respond.argtypes = [C.c_void_p, SIZE, C.c_void_p, SIZE, C.POINTER(SIZE)]
    # A test-only certificate replaces the chip's PKCS#7 identity. Verify the
    # response using the peer's own X25519 and RSA implementations, not a C
    # encrypt/decrypt round trip with the same possible bug on both sides.
    for major, bits in ((2, 1024), (3, 2048)):
        signer = rsa.generate_private_key(public_exponent=65537, key_size=bits)
        name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "interop test")])
        cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
                .public_key(signer.public_key()).serial_number(1)
                .not_valid_before(datetime.datetime(2020, 1, 1))
                .not_valid_after(datetime.datetime(2030, 1, 1))
                .sign(signer, hashes.SHA256()).public_bytes(serialization.Encoding.DER))
        captured = []
        errors = []
        algorithm = hashes.SHA1() if major == 2 else hashes.SHA256()

        @SIGN
        def sign(data, length, output, cap, out_len):
            try:
                digest = C.string_at(data, length)
                signature = signer.sign(digest, padding.PKCS1v15(), utils.Prehashed(algorithm))
                assert len(signature) <= cap
                captured.append(digest)
                C.memmove(output, signature, len(signature))
                out_len[0] = len(signature)
                return 0
            except Exception as exc:
                errors.append(exc)
                return -1

        @CERT
        def read_cert(output, cap, out_len):
            if len(cert) > cap:
                return -1
            C.memmove(output, cert, len(cert))
            out_len[0] = len(cert)
            return 0

        @RANDOM
        def random(output, length, context):
            C.memmove(output, os.urandom(length), length)
            return 0

        mfi = Mfi(None, sign, read_cert)
        provider = Provider(C.pointer(mfi), major, random, None)
        assert lib.airplay_mfi_sap_set_provider(C.byref(provider))
        phone = x25519.X25519PrivateKey.generate()
        phone_public = phone.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        request = b"\x01" + phone_public
        output = C.create_string_buffer(2048)
        length = SIZE()
        assert lib.airplay_mfi_sap_respond(request, len(request), output, len(output), C.byref(length)) == 0
        assert not errors, errors
        response = output.raw[:length.value]
        ours = response[:32]
        cert_len = int.from_bytes(response[32:36], "big")
        assert response[36:36 + cert_len] == cert
        pos = 36 + cert_len
        sig_len = int.from_bytes(response[pos:pos + 4], "big")
        assert sig_len == bits // 8 and pos + 4 + sig_len == len(response)
        shared = phone.exchange(x25519.X25519PublicKey.from_public_bytes(ours))
        key = hashlib.sha1(b"AES-KEY" + shared).digest()[:16]
        iv = hashlib.sha1(b"AES-IV" + shared).digest()[:16]
        decrypt = Cipher(algorithms.AES(key), modes.CTR(iv)).decryptor()
        signature = decrypt.update(response[pos + 4:]) + decrypt.finalize()
        digest = hashlib.new("sha1" if major == 2 else "sha256", ours + phone_public).digest()
        assert captured == [digest]
        signer.public_key().verify(signature, ours + phone_public, padding.PKCS1v15(), algorithm)
        # Flipped ciphertext must fail independent peer signature validation.
        damaged = bytes([signature[0] ^ 1]) + signature[1:]
        try:
            signer.public_key().verify(damaged, ours + phone_public, padding.PKCS1v15(), algorithm)
        except InvalidSignature:
            pass
        else:
            raise AssertionError("damaged signature accepted")
        print(f"MFiSAP major={major}: peer ECDH, KDF, digest, AES-CTR and RSA verification passed")
    lib.airplay_mfi_sap_clear_provider()


def check_control(lib):
    lib.airplay_control_cipher_create.argtypes = [C.c_void_p, C.c_void_p, C.POINTER(C.c_void_p)]
    lib.airplay_control_cipher_encrypt_message.argtypes = [C.c_void_p, C.c_void_p, SIZE, C.c_void_p, SIZE, C.POINTER(SIZE)]
    lib.airplay_control_cipher_destroy.argtypes = [C.c_void_p]
    read_key, write_key = os.urandom(32), os.urandom(32)
    cipher = C.c_void_p()
    assert lib.airplay_control_cipher_create(read_key, write_key, C.byref(cipher)) == 0
    counter = 0
    try:
        # Same response size as the board, followed by a second response to
        # check directional nonce continuity across message/frame boundaries.
        for count in (1166, 17):
            plaintext = os.urandom(count)
            output = C.create_string_buffer(count + 72)
            length = SIZE()
            assert lib.airplay_control_cipher_encrypt_message(cipher, plaintext, count, output, len(output), C.byref(length)) == 0
            wire = output.raw[:length.value]
            offset, decoded, frames = 0, b"", 0
            while offset < len(wire):
                aad = wire[offset:offset + 2]
                size = int.from_bytes(aad, "little")
                assert size <= 1024
                sealed = wire[offset + 2:offset + 18 + size]
                nonce = b"\x00" * 4 + counter.to_bytes(8, "little")
                decoded += ChaCha20Poly1305(write_key).decrypt(nonce, sealed, aad)
                counter += 1
                frames += 1
                offset += size + 18
            assert decoded == plaintext and offset == len(wire)
            assert frames == (count + 1023) // 1024
        print("RTSP control: independent peer decrypts 1166-byte two-frame response and next nonce")
    finally:
        lib.airplay_control_cipher_destroy(cipher)


def main():
    with tempfile.TemporaryDirectory(prefix="rtcp-auth-interop-") as temp:
        shared = Path(temp) / "auth.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                        "-Iinclude", "-Icore/airplay/include", "-Ithird_party/mbedtls/include",
                        "-Iconfig", '-DMBEDTLS_CONFIG_FILE="mbedtls_rtcp_host.h"',
                        "core/airplay/airplay_mfi_sap.c", "core/airplay/airplay_control_cipher.c",
                        "build/mbedtls/libmbedcrypto.a", "-o", str(shared)], cwd=ROOT, check=True)
        lib = C.CDLL(str(shared))
        check_auth(lib)
        check_control(lib)


if __name__ == "__main__":
    main()
