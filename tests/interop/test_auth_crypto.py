#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent OpenSSL-backed peer checks for the actual C auth/cipher code.

Requires Python cryptography and a PIC mbedTLS archive from the Make build.
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
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa, utils, x25519
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.x509.oid import NameOID

ROOT = Path(__file__).resolve().parents[2]
SIZE = C.c_size_t
RANDOM = C.CFUNCTYPE(C.c_bool, C.c_void_p, SIZE, C.c_void_p)


class SoftwareConfig(C.Structure):
    _fields_ = [("certificate", C.c_void_p), ("certificate_len", SIZE),
                ("private_key", C.c_void_p), ("private_key_len", SIZE),
                ("protocol_major", C.c_uint8), ("random", C.c_void_p),
                ("random_context", C.c_void_p), ("os", C.c_void_p)]


def check_auth(lib):
    lib.rtcp_mfi_software_create.argtypes = [C.POINTER(SoftwareConfig), C.POINTER(C.c_void_p)]
    lib.rtcp_mfi_software_provider.argtypes = [C.c_void_p]
    lib.rtcp_mfi_software_provider.restype = C.c_void_p
    lib.rtcp_mfi_software_destroy.argtypes = [C.c_void_p]
    lib.rtcp_mfi_software_destroy.restype = None
    lib.airplay_mfi_sap_respond_with_provider.argtypes = [
        C.c_void_p, RANDOM, C.c_void_p, C.c_void_p, SIZE, C.c_void_p,
        SIZE, C.POINTER(SIZE),
    ]
    for major in (2, 3):
        signer = (rsa.generate_private_key(public_exponent=65537, key_size=1024)
                  if major == 2 else ec.generate_private_key(ec.SECP256R1()))
        name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "interop test")])
        cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
                .public_key(signer.public_key()).serial_number(1)
                .not_valid_before(datetime.datetime(2020, 1, 1))
                .not_valid_after(datetime.datetime(2030, 1, 1))
                .sign(signer, hashes.SHA256()).public_bytes(serialization.Encoding.DER))
        algorithm = hashes.SHA1() if major == 2 else hashes.SHA256()
        key = signer.private_bytes(serialization.Encoding.DER,
                                   serialization.PrivateFormat.PKCS8,
                                   serialization.NoEncryption())
        cert_buffer = C.create_string_buffer(cert)
        key_buffer = C.create_string_buffer(key)
        linux_os = C.addressof(C.c_char.in_dll(lib, "rtcp_os_linux"))
        config = SoftwareConfig(C.addressof(cert_buffer), len(cert),
                                C.addressof(key_buffer), len(key), major, None, None, linux_os)
        software = C.c_void_p()
        assert lib.rtcp_mfi_software_create(C.byref(config), C.byref(software)) == 0
        C.memset(key_buffer, 0, len(key_buffer))

        @RANDOM
        def random(output, length, context):
            C.memmove(output, os.urandom(length), length)
            return True

        phone = x25519.X25519PrivateKey.generate()
        phone_public = phone.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        request = b"\x01" + phone_public
        output = C.create_string_buffer(2048)
        length = SIZE()
        try:
            provider = lib.rtcp_mfi_software_provider(software)
            assert lib.airplay_mfi_sap_respond_with_provider(
                provider, random, None, request, len(request), output, len(output),
                C.byref(length)) == 0
        finally:
            lib.rtcp_mfi_software_destroy(software)
        response = output.raw[:length.value]
        ours = response[:32]
        cert_len = int.from_bytes(response[32:36], "big")
        assert response[36:36 + cert_len] == cert
        pos = 36 + cert_len
        sig_len = int.from_bytes(response[pos:pos + 4], "big")
        assert sig_len == (128 if major == 2 else 64) and pos + 4 + sig_len == len(response)
        shared = phone.exchange(x25519.X25519PublicKey.from_public_bytes(ours))
        key = hashlib.sha1(b"AES-KEY" + shared).digest()[:16]
        iv = hashlib.sha1(b"AES-IV" + shared).digest()[:16]
        decrypt = Cipher(algorithms.AES(key), modes.CTR(iv)).decryptor()
        signature = decrypt.update(response[pos + 4:]) + decrypt.finalize()

        def verify(wire_signature):
            if major == 2:
                signer.public_key().verify(wire_signature, ours + phone_public,
                                           padding.PKCS1v15(), algorithm)
            else:
                first = int.from_bytes(wire_signature[:32], "big")
                second = int.from_bytes(wire_signature[32:], "big")
                signer.public_key().verify(utils.encode_dss_signature(first, second),
                                           ours + phone_public, ec.ECDSA(algorithm))

        verify(signature)
        # Flipped ciphertext must fail independent peer signature validation.
        damaged = bytes([signature[0] ^ 1]) + signature[1:]
        try:
            verify(damaged)
        except InvalidSignature:
            pass
        else:
            raise AssertionError("damaged signature accepted")
        print(f"MFiSAP major={major}: peer ECDH, KDF, AES-CTR and signature verification passed")


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
    crypto_source = os.environ.get("RTCP_MBEDTLS_DIR", "third_party/mbedtls")
    crypto_archive = os.environ.get("RTCP_INTEROP_CRYPTO_LIB", "build/mbedtls/libmbedcrypto.a")
    with tempfile.TemporaryDirectory(prefix="rtcp-auth-interop-") as temp:
        shared = Path(temp) / "auth.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                        "-Iinclude", "-Icore/airplay/include", "-Iport/linux/include",
                        "-I" + str(Path(crypto_source) / "include"), "-Iconfig",
                        '-DMBEDTLS_CONFIG_FILE="mbedtls_rtcp_host.h"', "-DRTCP_MFI_SOFTWARE",
                        "core/airplay/airplay_mfi_sap.c", "core/airplay/airplay_control_cipher.c",
                        "core/mfi/mfi_software.c", "port/linux/rtcp_os_linux.c",
                        crypto_archive, "-lpthread", "-o", str(shared)], cwd=ROOT, check=True)
        lib = C.CDLL(str(shared))
        check_auth(lib)
        check_control(lib)


if __name__ == "__main__":
    main()
