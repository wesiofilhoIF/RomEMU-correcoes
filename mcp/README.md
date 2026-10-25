# RomEMU — Model Context Protocol (MCP) Server

Este módulo expõe o **RomEMU** (ESP32-S3) como um servidor MCP oficial padrão da Anthropic para agentes autônomos de IA (como Claude Code, Hermes Agent, OMP, Cursor e Windsurf).

## Requisitos

```bash
pip install mcp
```

## Ferramentas Expostas (11 Tools)

- `romemu_get_status`: Telemetria em tempo real (uptime, heap/PSRAM, IP, RSSI).
- `romemu_get_gpio_oscilloscope`: Osciloscópio digital das linhas de sinal SPI (CS, CLK, MOSI, MISO, WP, HD) e pinos Reset/Power.
- `romemu_get_access_log`: Histórico das transações SPI reais da placa-mãe na BIOS.
- `romemu_power_control`: Ligar (`on`), desligar (`off`) ou ciclo de energia (`cycle`) na placa-mãe.
- `romemu_reset_control`: Pulso de reset (`pulse`), travar (`assert`) ou liberar (`release`).
- `romemu_get_slots`: Listagem das imagens de ROM e slots de emulação.
- `romemu_emulation_control`: Iniciar (`insert`) ou pausar (`eject`) a emulação no barramento.
- `romemu_upload_rom_file`: Upload de BIOS com gravação permanente automática na Flash interna do ESP32!
- `romemu_wifi_scan`: Varredura de redes Wi-Fi 2.4 GHz na bancada.
- `romemu_wifi_connect`: Configuração de Wi-Fi salva em NVS.
- `romemu_get_statistics`: Estatísticas de ciclos de barramento.

## Configuração em Clientes MCP

### Hermes Agent (`hermes mcp add`)
```bash
hermes mcp add romemu --command python3 --args mcp/mcp_server.py --env ROMEMU_HOST=http://192.168.1.248
```

### Claude Desktop / OMP / Cursor (`mcp.json`)
```json
{
  "mcpServers": {
    "romemu": {
      "command": "python3",
      "args": ["/caminho/para/RomEMU/mcp/mcp_server.py"],
      "env": {
        "ROMEMU_HOST": "http://192.168.1.248"
      }
    }
  }
}
```
