#!/usr/bin/env python3
"""Склеить сертификаты DER подряд в PEM, который читает OpenSSL."""
import base64
import sys


def der_certs(data):
    i = 0
    while i < len(data):
        if data[i] != 0x30:
            raise SystemExit(f"{sys.argv[1]}: not a certificate at offset {i}")
        b = data[i + 1]
        if b < 0x80:
            length, header = b, 2
        else:
            nbytes = b & 0x7F
            if nbytes < 1 or nbytes > 3 or i + 2 + nbytes > len(data):
                raise SystemExit(f"{sys.argv[1]}: bad certificate length at offset {i}")
            length = int.from_bytes(data[i + 2:i + 2 + nbytes], "big")
            header = 2 + nbytes
        end = i + header + length
        if length < 0 or end > len(data):
            raise SystemExit(f"{sys.argv[1]}: certificate overruns the file at offset {i}")
        yield data[i:end]
        i = end


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: roots_pem.py roots.der cert.pem")
    data = open(sys.argv[1], "rb").read()
    out = []
    for cert in der_certs(data):
        body = base64.b64encode(cert).decode("ascii")
        lines = "\n".join(body[i:i + 64] for i in range(0, len(body), 64))
        out.append(f"-----BEGIN CERTIFICATE-----\n{lines}\n-----END CERTIFICATE-----\n")
    if not out:
        raise SystemExit(f"{sys.argv[1]}: no certificates")
    open(sys.argv[2], "w").write("".join(out))


if __name__ == "__main__":
    main()
