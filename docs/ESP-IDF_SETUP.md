# ESP-IDF su Windows — setup MrBin

ESP-IDF **v5.5** è installato su questo PC per compilare **MrBin CORE** (ESP32-P4).

## Percorsi installati

| Elemento | Percorso |
|----------|----------|
| ESP-IDF v5.5 | `C:\esp\v5.5\esp-idf` |
| Toolchain / Python | `C:\Espressif\tools` |
| EIM (gestore versioni) | `eim` (winget: Espressif.EIM-CLI) |
| Target installato | **esp32p4** |

## Uso rapido da PowerShell

### Opzione A — script MrBin (consigliata in Cursor)

```powershell
cd C:\CURSOR\Mrbin
. .\scripts\activate-idf.ps1
cd src\mrbin_core
idf.py set-target esp32p4   # solo la prima volta
idf.py build
idf.py -p COMx flash monitor
```

### Opzione B — shortcut desktop

L'installer EIM ha creato un collegamento **ESP-IDF PowerShell** sul desktop: apre un terminale con ambiente già attivo.

### Opzione C — comando singolo senza attivare shell

```powershell
eim run "idf.py build" v5.5
```
(da eseguire nella cartella del progetto)

### Opzione C — build automatico

```powershell
.\scripts\build-core.ps1
.\scripts\build-core.ps1 -Port COM5
```

## Cursor / VS Code

1. Installa l'estensione **Espressif IDF** (se non presente).
2. Apri la cartella `C:\CURSOR\Mrbin` o `src\mrbin_core`.
3. L'estensione dovrebbe rilevare:
   - `IDF_PATH` = `C:\esp\v5.5\esp-idf`
   - `IDF_TOOLS_PATH` = `C:\Espressif\tools`

Impostazioni in `.vscode/settings.json` già configurate nel repo.

## Driver USB

`eim install-drivers` può richiedere **PowerShell come Amministratore** (errore exit code 5 senza privilegi). Se la porta COM non compare, riesegui da terminale elevato:

```powershell
eim install-drivers
```

## Risoluzione problemi toolchain

Se `idf.py build` segnala `cannot execute 'cc1'`, reinstallare i tool:

```powershell
$env:IDF_TOOLS_PATH = "C:\Espressif\tools"
& "C:\Espressif\tools\python\v5.5\venv\Scripts\python.exe" "C:\esp\v5.5\esp-idf\tools\idf_tools.py" install --targets=esp32p4
```

I tool completi devono trovarsi in `C:\Espressif\tools\riscv32-esp-elf\...` (non in `tools\tools\`).

## Comandi utili EIM

```powershell
eim list                  # versioni installate
eim select v5.5           # versione attiva
eim run "idf.py --version" v5.5
```

## Arduino vs ESP-IDF

Per **ESP32-P4-WIFI6-M** (camera H.264, SDIO, Wi-Fi6) usare **ESP-IDF**, non Arduino IDE. Il progetto MrBin CORE è già strutturato come progetto ESP-IDF in `src/mrbin_core/`.
