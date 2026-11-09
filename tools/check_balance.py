"""Consulta saldo das APIs de LLM usadas no projeto.

DeepSeek tem endpoint público de saldo (`/user/balance`). Anthropic NÃO expõe
saldo via API — a consulta é manual em console.anthropic.com.

Uso (PowerShell):
    $env:DEEPSEEK_API_KEY = "sk-..."
    python tools/check_balance.py

A chave fica no banco do OpenCode (`~/.local/share/opencode/opencode.db`), não
em variável de ambiente; defina DEEPSEEK_API_KEY uma vez (ou adicione ao perfil
do PowerShell) para a consulta funcionar.
"""

import json
import os
import urllib.request

DEEPSEEK_BALANCE = "https://api.deepseek.com/user/balance"


def deepseek_balance() -> None:
    key = os.environ.get("DEEPSEEK_API_KEY", "").strip()
    if not key:
        print("DeepSeek: defina DEEPSEEK_API_KEY para consultar o saldo.")
        return
    req = urllib.request.Request(
        DEEPSEEK_BALANCE,
        headers={"Authorization": f"Bearer {key}", "Accept": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            data = json.load(resp)
    except Exception as exc:  # noqa: BLE001
        print(f"DeepSeek: falha na consulta: {exc}")
        return
    print("DeepSeek saldo:", json.dumps(data, ensure_ascii=False))


def anthropic_hint() -> None:
    print(
        "Anthropic: sem endpoint público de saldo. Veja "
        "https://console.anthropic.com/settings/billing"
    )


if __name__ == "__main__":
    deepseek_balance()
    anthropic_hint()
