#!/usr/bin/env python3
"""
Upload de BIOS de 16MB (W25Q128) para RomEMU no ESP32-S3 com compressao LZ4 Zero-Copy.
Ideal para placas-mae X99 (Jginyue, Huananzhi, Machinist, etc.) e placas ESP32-S3 com 8MB PSRAM.
"""

import sys
import os
import struct
import argparse
import lz4.block
import urllib.request
import urllib.parse
import json

BLOCK_SIZE = 4096

def pack_lz4_archive(raw_bytes, chip_size=16777216):
    if len(raw_bytes) < chip_size:
        raw_bytes = raw_bytes + (b'\xFF' * (chip_size - len(raw_bytes)))
    elif len(raw_bytes) > chip_size:
        raw_bytes = raw_bytes[:chip_size]

    num_blocks = len(raw_bytes) // BLOCK_SIZE
    comp_sizes = []
    payload_parts = []

    erased_count = 0
    raw_count = 0

    for i in range(num_blocks):
        chunk = raw_bytes[i * BLOCK_SIZE : (i + 1) * BLOCK_SIZE]
        if chunk == b'\xFF' * BLOCK_SIZE:
            comp_sizes.append(0)
            erased_count += 1
        else:
            c = lz4.block.compress(chunk, store_size=False)
            if len(c) >= BLOCK_SIZE:
                comp_sizes.append(BLOCK_SIZE)
                payload_parts.append(chunk)
                raw_count += 1
            else:
                comp_sizes.append(len(c))
                payload_parts.append(c)

    hdr = struct.pack('<I', num_blocks) + struct.pack(f'<{num_blocks}H', *comp_sizes)
    archive = hdr + b''.join(payload_parts)
    print(f"[*] BIOS empacotada: {len(raw_bytes)} -> {len(archive)} bytes ({len(archive)/1024/1024:.2f} MB)")
    print(f"[*] Estatisticas: {num_blocks} blocos ({erased_count} vazios/0xFF, {raw_count} incompressiveis)")
    return archive

def main():
    parser = argparse.ArgumentParser(description="Upload de BIOS X99 16MB para RomEMU (ESP32-S3)")
    parser.add_argument("--ip", required=True, help="IP do ESP32-S3 (ex: 192.168.1.248)")
    parser.add_argument("--rom", required=True, help="Caminho do arquivo .rom ou .bin")
    parser.add_argument("--label", default="X99_BIOS", help="Nome do slot")
    parser.add_argument("--slot", type=int, default=0, help="Numero do slot (default: 0)")
    parser.add_argument("--chip", type=int, default=4, help="ID do chip (4 = W25Q128 16MB)")
    parser.add_argument("--insert", action="store_true", default=True, help="Inserir automaticamente no barramento SPI")

    args = parser.parse_args()

    if not os.path.isfile(args.rom):
        print(f"[-] Arquivo nao encontrado: {args.rom}")
        sys.exit(1)

    with open(args.rom, "rb") as f:
        raw_data = f.read()

    archive = pack_lz4_archive(raw_data, chip_size=16777216)

    url = f"http://{args.ip}/api/slots/{args.slot}/upload?chip={args.chip}&label={urllib.parse.quote(args.label)}"
    print(f"[*] Enviando arquivo para {url}...")

    req = urllib.request.Request(
        url,
        data=archive,
        headers={
            "Content-Type": "application/octet-stream",
            "X-Compressed": "lz4",
            "X-Original-Size": "16777216",
        },
        method="POST"
    )

    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            data = resp.read().decode()
            print(f"[+] Upload realizado com sucesso! Resposta: {data}")
    except Exception as e:
        print(f"[-] Erro durante o upload: {e}")
        sys.exit(1)

    if args.insert:
        insert_url = f"http://{args.ip}/api/slots/{args.slot}/insert"
        print(f"[*] Inserindo chip no barramento SPI ({insert_url})...")
        try:
            req_insert = urllib.request.Request(insert_url, data=b"", method="POST")
            with urllib.request.urlopen(req_insert, timeout=10) as resp:
                print(f"[+] Chip inserido com sucesso! O ESP32-S3 agora responde como W25Q128!")
        except Exception as e:
            print(f"[-] Erro ao inserir chip: {e}")

if __name__ == "__main__":
    main()
