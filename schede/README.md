# Schede MrBin

Documentazione e configurazione delle **schede Arduino / PCB** usate nel progetto MrBin.

## Contenuto previsto

Per ogni scheda, aggiungere un file (es. `scheda-principale.md`) con:

| Campo | Descrizione |
|-------|-------------|
| Nome scheda | Nome in Arduino IDE (Board Manager) |
| MCU | Modello e package |
| Pinout | Mappatura GPIO rilevante |
| Periferiche | Display, sensori, alimentazione |
| Note build | Core, frequenza, partizioni, flag compile |

## Esempio

```markdown
# MrBin — scheda principale

- **Arduino IDE**: Seeed XIAO MG24
- **Core**: Silicon Labs Arduino
- **Note**: ...
```

Aggiornare questa cartella ad ogni modifica hardware o cambio target.
