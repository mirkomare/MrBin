# MrBin

Progetto firmware e documentazione per le **schede dedicate MrBin**.

Repository separato da [BLEmax-M](https://github.com/mirkomare/BLEmax-M): ogni modifica a hardware, sketch o configurazione board MrBin va aggiornata **qui**.

## Struttura

```
MrBin/
├── README.md           # Questo file
├── CHANGELOG.md        # Cronologia modifiche
├── docs/               # Documentazione tecnica
├── schede/             # Definizioni e note per schede Arduino / PCB MrBin
└── src/                # Sketch e firmware (.ino, librerie)
```

## Sviluppo

1. Apri lo sketch in `src/` con **Arduino IDE** (o PlatformIO, se configurato).
2. Seleziona la **scheda MrBin** documentata in `schede/`.
3. Compila e flash sul target.

## Collegamento con BLEmax-M

MrBin è un progetto satellite: integrazioni con gateway BLE, profili device o app Homey restano nel repository principale **BLEmax-M**, salvo diversa indicazione.

## Licenza

Uso privato / progetto personale — vedi repository GitHub.
