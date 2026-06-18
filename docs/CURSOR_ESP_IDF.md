# Cursor + Espressif IDF — guida rapida

## 1. Apri il workspace

In Cursor:

**File → Open Workspace from File…** → seleziona:

```
C:\CURSOR\Mrbin\MrBin.code-workspace
```

Il workspace apre due cartelle:
- **MrBin CORE (ESP-IDF)** — progetto firmware (`src/mrbin_core`)
- **MrBin (docs/scripts)** — documentazione e script

## 2. Estensione e barra ESP-IDF

L'estensione **Espressif IDF** (`espressif.esp-idf-extension`) va installata in Cursor (gia' consigliata dal workspace).

Se non compare la **barra ESP-IDF** in basso (chip, build, flash, monitor):

1. Apri **`MrBin.code-workspace`** (non la sola cartella root)
2. `Ctrl+Shift+P` → **Developer: Reload Window**
3. Nel pannello **Explorer**, seleziona un file sotto **MrBin CORE (ESP-IDF)** (es. `main/mrbin_core_main.cpp`)
4. Se ancora assente: `Ctrl+Shift+X` → **ESP-IDF** → verifica abilitata
5. `Ctrl+Shift+P` → **ESP-IDF: Doctor Command** (deve leggere v5.5.4)

La barra compare solo quando l'estensione e' attiva sulla cartella progetto `src/mrbin_core` (impostato con `idf.extensionActivationMode: always` nel workspace).

## 3. Collega ESP-IDF già installato (non reinstallare)

`Ctrl+Shift+P` e scegli:

```
ESP-IDF: Select Current ESP-IDF Version
```

Seleziona **v5.5.4** (`C:\esp\v5.5.4\esp-idf`).

Se non compare in lista, usa:

```
ESP-IDF: Doctor Command
```

e verifica che legga i path dal `settings.json` del workspace.

## 4. Target e build

1. Nella **status bar** in basso, clicca sul chip target → scegli **esp32p4**
   - Oppure: `Ctrl+Shift+P` → **ESP-IDF: Set Espressif Device Target** → `esp32p4`
2. **Build**: icona 🔨 nella barra ESP-IDF, oppure `Ctrl+Shift+P` → **ESP-IDF: Build your project**
3. Collega la board USB

## 5. Flash e monitor

1. Imposta la porta: barra in basso **UART** → seleziona `COMx`
   - Oppure: `Ctrl+Shift+P` → **ESP-IDF: Select port to use**
2. **Flash**: icona ⚡ → **ESP-IDF: Flash your project**
3. **Monitor seriale**: icona 📟 → **ESP-IDF: Monitor your device**
4. **Flash + Monitor**: **ESP-IDF: Flash and start a monitor on your device**

## 6. Barra ESP-IDF (in basso)

| Elemento | Funzione |
|----------|----------|
| 🔥 / chip | Target (`esp32p4`) |
| 🔨 | Build |
| ⚡ | Flash |
| 📟 | Monitor |
| 🗑️ | Full clean |
| COM | Porta seriale |

## 7. Comandi utili (palette)

| Comando | Uso |
|---------|-----|
| `ESP-IDF: Doctor Command` | Diagnostica path e tool |
| `ESP-IDF: SDK Configuration editor (menuconfig)` | Configurazione firmware |
| `ESP-IDF: Build your project` | Compila |
| `ESP-IDF: Flash your project` | Carica firmware |

## Path configurati (già nel workspace)

| Setting | Valore |
|---------|--------|
| ESP-IDF | `C:\esp\v5.5.4\esp-idf` |
| Tools | `C:\Espressif\tools` |
| Python | `C:\Espressif\tools\python\v5.5.4\venv` |

Non serve rieseguire il wizard di installazione ESP-IDF nell'estensione: l'ambiente è già pronto via EIM.

## 8. Verifica rapida e build da terminale

```powershell
cd C:\CURSOR\Mrbin
.\scripts\verify-workspace.ps1          # controlla path, Python, idf.py
.\scripts\verify-workspace.ps1 -Build   # verifica + compila
.\scripts\build-core.ps1                # solo build
.\scripts\build-core.ps1 -Port COM5     # build + flash + monitor
```

Terminale integrato con ESP-IDF già attivo: profilo **ESP-IDF PowerShell** (menu ▼ del terminale).

Task Cursor/VS Code (`Ctrl+Shift+P` → **Tasks: Run Task**):
- **MrBin: Verifica workspace**
- **MrBin: Build CORE** (default build)
- **MrBin: Clean + Build CORE**
