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

## 2. Estensione

L’estensione **Espressif IDF** (`espressif.esp-idf-extension`) è già installata in Cursor.

Se non compare la barra ESP-IDF in basso:
- `Ctrl+Shift+X` → cerca **ESP-IDF** → verifica che sia abilitata
- Ricarica la finestra: `Ctrl+Shift+P` → **Developer: Reload Window**

## 3. Collega ESP-IDF già installato (non reinstallare)

`Ctrl+Shift+P` e scegli:

```
ESP-IDF: Select Current ESP-IDF Version
```

Seleziona **v5.5** (`C:\esp\v5.5\esp-idf`).

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
| ESP-IDF | `C:\esp\v5.5\esp-idf` |
| Tools | `C:\Espressif\tools` |
| Python | `C:\Espressif\tools\python\v5.5\venv` |

Non serve rieseguire il wizard di installazione ESP-IDF nell’estensione: l’ambiente è già pronto via EIM.
