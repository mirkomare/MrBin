# ESP-IDF su Windows — setup MrBin

**ESP-IDF v5.5.4** (ultima stabile v5.5.x) è installato per compilare **MrBin CORE** (ESP32-P4 + `esp_capture`).

## Percorsi installati

| Elemento | Percorso |
|----------|----------|
| ESP-IDF v5.5.4 | `C:\esp\v5.5.4\esp-idf` |
| Toolchain / Python | `C:\Espressif\tools` |
| Python venv IDF | `C:\Espressif\tools\python\v5.5.4\venv` |
| EIM (gestore versioni) | `eim` (winget: Espressif.EIM-CLI) |
| Target installato | **esp32p4** |

## Installazione / aggiornamento ESP-IDF

```powershell
cd C:\CURSOR\Mrbin
.\scripts\install-idf.ps1              # installa v5.5.4 (default)
.\scripts\install-idf.ps1 -Version v5.5.4
```

Configurazione EIM: copia `eim_config.toml.example` → `eim_config.toml` (locale, gitignored).

## Uso rapido da PowerShell

### Opzione A — script MrBin (consigliata in Cursor)

```powershell
cd C:\CURSOR\Mrbin
. .\scripts\activate-idf.ps1           # legge la versione selezionata da EIM
.\scripts\build-core.ps1
.\scripts\build-core.ps1 -Port COM5
```

Lo script attiva ESP-IDF, esegue `set-target esp32p4` (se manca `sdkconfig`) e compila.

### Opzione B — shortcut desktop

L'installer EIM crea un collegamento **ESP-IDF PowerShell** sul desktop.

### Opzione C — EIM run

```powershell
eim run "idf.py build" v5.5.4
```

(da eseguire in `src\mrbin_core`)

## Verifica workspace

```powershell
cd C:\CURSOR\Mrbin
.\scripts\verify-workspace.ps1
.\scripts\verify-workspace.ps1 -Build
```

## Cursor / VS Code

1. Estensione **Espressif IDF**
2. Apri `MrBin.code-workspace`
3. Path attesi:
   - `IDF_PATH` = `C:\esp\v5.5.4\esp-idf`
   - `IDF_TOOLS_PATH` = `C:\Espressif\tools`

## Driver USB

```powershell
eim install-drivers
```

(PowerShell come Amministratore se necessario)

## Comandi utili EIM

```powershell
eim list
eim select v5.5.4
eim run "idf.py --version" v5.5.4
```

## Arduino vs ESP-IDF

Per **ESP32-P4-WIFI6-M** usare **ESP-IDF**, non Arduino IDE. Il firmware MrBin CORE è in `src/mrbin_core/`.
