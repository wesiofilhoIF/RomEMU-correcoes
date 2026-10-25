#!/usr/bin/env python3
"""
RomEMU MCP Server — Servidor Oficial com o SDK MCP Oficial da Anthropic / Model Context Protocol.
Controlador de bancada do emulador de BIOS RomEMU (ESP32-S3) para agentes de IA autônomos.

Uso:
  python3 mcp_server.py
  ou via stdio configurado no cliente MCP (Claude Code, Hermes, OMP, Cursor, Windsurf).
"""

import os
import json
import urllib.request
import urllib.error
from typing import Optional
from mcp.server.fastmcp import FastMCP

ROMEMU_HOST = os.environ.get("ROMEMU_HOST", "http://192.168.1.248")

mcp = FastMCP("romemu-mcp-server")

def _http_request(endpoint: str, method: str = "GET", payload: Optional[dict] = None, raw_body: Optional[bytes] = None, timeout: int = 5):
    url = f"{ROMEMU_HOST}{endpoint}"
    req = urllib.request.Request(url, method=method)
    req.add_header("User-Agent", "RomEMU-Official-MCP-SDK/1.0")

    if raw_body is not None:
        req.data = raw_body
        req.add_header("Content-Type", "application/octet-stream")
    elif payload is not None:
        req.data = json.dumps(payload).encode("utf-8")
        req.add_header("Content-Type", "application/json")

    with urllib.request.urlopen(req, timeout=timeout) as resp:
        content_type = resp.headers.get("Content-Type", "")
        body = resp.read()
        if "application/json" in content_type:
            return json.loads(body.decode("utf-8"))
        try:
            return json.loads(body.decode("utf-8"))
        except Exception:
            return body.decode("utf-8", errors="replace")


@mcp.tool()
def romemu_get_status() -> str:
    """
    Obtém a telemetria completa em tempo real do RomEMU:
    - Uptime do ESP32-S3
    - Memória Heap interna e PSRAM livres
    - Endereço IP e intensidade de sinal Wi-Fi (dBm)
    - Chip SPI ativo (ex: W25Q128JV) e slot montado
    """
    try:
        data = _http_request("/api/status")
        return json.dumps(data, indent=2)
    except Exception as e:
        return f"Erro ao conectar com RomEMU ({ROMEMU_HOST}): {e}"


@mcp.tool()
def romemu_get_gpio_oscilloscope() -> str:
    """
    Lê os níveis lógicos em tempo real das linhas de barramento de hardware SPI:
    - cs: Chip Select (0 = ativo / em transação, 1 = repouso 3.3V)
    - clk: Clock SPI Mestre gerado pelo PCH Intel C612 (0 ou 1)
    - mosi: Dados Master Out Slave In (saída da placa / entrada da BIOS)
    - miso: Dados Master In Slave Out (saída da BIOS / entrada da placa)
    - wp: Write Protect
    - hd: Hold
    """
    try:
        data = _http_request("/api/gpio")
        return json.dumps(data, indent=2)
    except Exception as e:
        return f"Erro ao ler osciloscópio de GPIO: {e}"


@mcp.tool()
def romemu_get_access_log() -> str:
    """
    Obtém os últimos 100 registros de transações reais executadas pela placa-mãe na BIOS:
    Retorna comandos decodificados (JEDEC Read ID 0x9F, Read Data 0x03, Fast Read 0x0B, etc.),
    endereços lidos e quantidade de bytes transferidos via clock.
    """
    try:
        data = _http_request("/api/log")
        return json.dumps(data, indent=2)
    except Exception as e:
        return f"Erro ao obter log de acessos: {e}"


@mcp.tool()
def romemu_power_control(action: str, duration_ms: Optional[int] = None) -> str:
    """
    Controla o circuito de energia da placa-mãe via pino ATX Open-Drain:
    - action='on': pulso rápido de 300ms no botão PW_ON para ligar a placa
    - action='off': pulso forçado longo de 5000ms para desligamento de emergência
    - action='cycle': ciclo completo de desligar e religar a placa
    """
    try:
        if action == "on":
            endpoint = "/api/gpio/power/on"
            payload = {"duration": duration_ms if duration_ms else 300}
        elif action == "off":
            endpoint = "/api/gpio/power/off"
            payload = {"duration": duration_ms if duration_ms else 5000}
        elif action == "cycle":
            endpoint = "/api/gpio/power/cycle"
            payload = {"duration": duration_ms if duration_ms else 300, "settle": 200}
        else:
            return f"Ação inválida: {action}. Use 'on', 'off' ou 'cycle'."

        data = _http_request(endpoint, method="POST", payload=payload)
        return json.dumps(data, indent=2)
    except Exception as e:
        return f"Erro ao controlar energia: {e}"


@mcp.tool()
def romemu_reset_control(action: str, duration_ms: int = 100) -> str:
    """
    Controla o pino de Reset da placa-mãe:
    - action='pulse': pulso momentâneo de reset (default 100ms)
    - action='assert': trava o alvo em estado de reset permanente
    - action='release': solta e libera o alvo do reset
    """
    try:
        if action == "pulse":
            data = _http_request("/api/gpio/reset/pulse", method="POST", payload={"duration": duration_ms})
        elif action == "assert":
            data = _http_request("/api/gpio/reset/assert", method="POST")
        elif action == "release":
            data = _http_request("/api/gpio/reset/release", method="POST")
        else:
            return f"Ação inválida: {action}. Use 'pulse', 'assert' ou 'release'."
        return json.dumps(data, indent=2)
    except Exception as e:
        return f"Erro no controle de reset: {e}"


@mcp.tool()
def romemu_get_slots() -> str:
    """
    Lista todos os slots de ROM emulados no ESP32-S3:
    Mostra rótulo, tamanho da imagem, CRC32, tipo de chip e status (ativo/inativo).
    """
    try:
        data = _http_request("/api/slots")
        return json.dumps(data, indent=2)
    except Exception as e:
        return f"Erro ao listar slots: {e}"


@mcp.tool()
def romemu_emulation_control(action: str, slot: int = 0) -> str:
    """
    Ativa ou pausa a emulação da BIOS no barramento SPI:
    - action='insert': conecta e ativa a emulação do chip na placa-mãe
    - action='eject': desconecta/interrompe a emulação
    """
    try:
        if action not in ["insert", "eject"]:
            return f"Ação inválida: {action}. Use 'insert' ou 'eject'."
        data = _http_request(f"/api/slots/{slot}/{action}", method="POST")
        return json.dumps(data, indent=2)
    except Exception as e:
        return f"Erro no controle de emulação: {e}"


@mcp.tool()
def romemu_upload_rom_file(file_path: str, slot: int = 0, chip_type: int = 4, label: str = "X99_MOD") -> str:
    """
    Faz o upload de um arquivo de BIOS (.bin ou .rom) local no computador/servidor
    diretamente para o ESP32-S3. Substitui automaticamente a BIOS guardada na Flash interna!
    """
    if not os.path.exists(file_path):
        return f"Arquivo não encontrado: {file_path}"
    try:
        with open(file_path, "rb") as f:
            rom_bytes = f.read()

        endpoint = f"/api/slots/{slot}/upload?chip={chip_type}&label={label}"
        data = _http_request(endpoint, method="POST", raw_body=rom_bytes, timeout=40)
        return json.dumps(data, indent=2)
    except Exception as e:
        return f"Erro no upload da ROM: {e}"


@mcp.tool()
def romemu_wifi_scan() -> str:
    """
    Varre os canais 2.4GHz e lista todas as redes Wi-Fi próximas com:
    SSID, nível de sinal em dBm, canal e tipo de segurança (WPA2, WPA3, Open).
    """
    try:
        data = _http_request("/api/wifi/scan", timeout=15)
        return json.dumps(data, indent=2)
    except Exception as e:
        return f"Erro na varredura Wi-Fi: {e}"


@mcp.tool()
def romemu_wifi_connect(ssid: str, password: str) -> str:
    """
    Configura as credenciais de Wi-Fi no ESP32-S3 e solicita a conexão à rede.
    As credenciais são salvas de forma segura na NVS do microcontrolador.
    """
    try:
        data = _http_request("/api/wifi", method="POST", payload={"ssid": ssid, "password": password})
        return json.dumps(data, indent=2)
    except Exception as e:
        return f"Erro ao conectar Wi-Fi: {e}"


@mcp.tool()
def romemu_get_statistics() -> str:
    """
    Retorna métricas e contadores de ciclos de barramento:
    Quantidade total de leituras, escritas de páginas, apagamentos de setores e bytes trafegados.
    """
    try:
        data = _http_request("/api/stats")
        return json.dumps(data, indent=2)
    except Exception as e:
        return f"Erro ao ler estatísticas: {e}"


if __name__ == "__main__":
    mcp.run()
