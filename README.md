# Win16Sync

Ein sehr einfacher bidirektionaler Dateisync zwischen einem Linux-Server und einem Win16-Client.

## Eigenschaften

- Win16-Client auf OpenWatcom/Winsock 1.1
- Rust-Server mit einfachem TCP-Sync-Port und schlichtem HTML-Webinterface
- bidirektionaler Polling-Sync
- Konflikterkennung statt stiller Ueberschreibung
- Logging auf beiden Seiten
- manueller Client-Auto-Update-Slot auf dem Server ab `v0.2.0`
- nur numerische IPv4-Adressen
- 8.3-/FAT16-konservativ: Namen ausserhalb 8.3 werden ignoriert
- Dateien ueber 2 GiB werden ignoriert

## Struktur

- `server/` Rust-Server
- `client/` Win16/OpenWatcom-Client

## Server

```bash
cd server
cargo run --release -- ../server-config.json
```

Standard-Sync-Ordner:

- `~/Win31Sync`

Standardports:

- Sync: `9071/tcp`
- Web: `9081/tcp`

Manueller Update-Slot fuer den Win16-Client:

- Binary: `updates/W16SYNC.EXE`
- Versionstext: `updates/VERSION.TXT`

Wenn beides vorhanden ist, bietet der Server die Client-Binary per Update-Handshake an. Fehlt eines der beiden Dateien, laeuft alles normal weiter ohne Auto-Update.

## systemd

Die vorbereitete Unit liegt unter `packaging/systemd/win16sync.service`.

```bash
sudo cp packaging/systemd/win16sync.service /etc/systemd/system/win16sync.service
sudo systemctl daemon-reload
sudo systemctl enable --now win16sync.service
```

## Client

```bash
cd client
./build.sh
```

Standard-Sync-Ordner im Client:

- `C:\SYNC`

Build-Artefakte:

- `w16sync.exe`
- `setup.exe`
