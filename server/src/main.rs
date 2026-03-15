use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, HashMap, VecDeque};
use std::fs::{self, File, OpenOptions};
use std::io::{self, BufRead, BufReader, Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const APP_VERSION: &str = env!("CARGO_PKG_VERSION");
const PROTO_VERSION: &str = "1";
const MAX_FILE_SIZE: u64 = 2 * 1024 * 1024 * 1024;
const DEFAULT_CONFIG_PATH: &str = "server-config.json";
const LOG_TAIL_LIMIT: usize = 64;

#[derive(Clone, Serialize, Deserialize)]
#[serde(default)]
struct Config {
    sync_bind: String,
    http_bind: String,
    root_dir: String,
    log_file: String,
    state_file: String,
    update_file: String,
    update_version_file: String,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            sync_bind: "0.0.0.0:9071".to_string(),
            http_bind: "0.0.0.0:9081".to_string(),
            root_dir: "~/Win31Sync".to_string(),
            log_file: "win16sync-server.log".to_string(),
            state_file: "win16sync-state.json".to_string(),
            update_file: "updates/W16SYNC.EXE".to_string(),
            update_version_file: "updates/VERSION.TXT".to_string(),
        }
    }
}

#[derive(Clone)]
struct UpdatePackage {
    version: String,
    path: PathBuf,
    size: u64,
    crc32: u32,
}

#[derive(Clone, Default)]
struct RuntimeState {
    last_client: String,
    last_sync_started: String,
    last_sync_finished: String,
    last_result: String,
    last_error: String,
    recent_logs: VecDeque<String>,
    conflicts: Vec<ConflictRecord>,
}

#[derive(Clone, Default, Serialize, Deserialize)]
struct SyncState {
    files: BTreeMap<String, SyncStateEntry>,
}

#[derive(Clone, Default, Serialize, Deserialize)]
struct SyncStateEntry {
    server: Option<Fingerprint>,
    client: Option<Fingerprint>,
    #[serde(default)]
    server_dos_time: Option<u32>,
    #[serde(default)]
    client_dos_time: Option<u32>,
}

#[derive(Clone, Default, Serialize, Deserialize, PartialEq, Eq)]
struct Fingerprint {
    size: u64,
    crc32: u32,
    #[serde(default = "default_true")]
    crc_known: bool,
}

#[derive(Clone, Default)]
struct ManifestItem {
    path: String,
    fingerprint: Fingerprint,
    dos_time: u32,
}

#[derive(Clone)]
struct ServerItem {
    manifest: ManifestItem,
    actual_rel: PathBuf,
}

#[derive(Clone, Default)]
struct ConflictRecord {
    path: String,
    server: String,
    client: String,
}

#[derive(Clone)]
enum SyncAction {
    Download { item: ServerItem },
    Upload { item: ManifestItem },
    DeleteLocal { path: String },
    DeleteRemote { path: String },
    Conflict {
        path: String,
        server: Option<ManifestItem>,
        client: Option<ManifestItem>,
    },
}

struct Shared {
    config_path: PathBuf,
    inner: Mutex<SharedInner>,
    sync_guard: Mutex<()>,
}

struct SharedInner {
    config: Config,
    runtime: RuntimeState,
}

impl Shared {
    fn load(config_path: PathBuf) -> io::Result<Self> {
        let config = if config_path.exists() {
            let text = fs::read_to_string(&config_path)?;
            serde_json::from_str(&text).unwrap_or_else(|_| Config::default())
        } else {
            let config = Config::default();
            write_json_file(&config_path, &config)?;
            config
        };

        Ok(Self {
            config_path,
            inner: Mutex::new(SharedInner {
                config,
                runtime: RuntimeState {
                    last_result: "Noch kein Sync".to_string(),
                    ..RuntimeState::default()
                },
            }),
            sync_guard: Mutex::new(()),
        })
    }

    fn config(&self) -> Config {
        self.inner.lock().unwrap().config.clone()
    }

    fn update_config(&self, config: Config) -> io::Result<()> {
        write_json_file(&self.config_path, &config)?;
        self.inner.lock().unwrap().config = config;
        Ok(())
    }

    fn config_dir(&self) -> PathBuf {
        self.config_path
            .parent()
            .map(Path::to_path_buf)
            .unwrap_or_else(|| PathBuf::from("."))
    }

    fn log_path(&self) -> PathBuf {
        resolve_path(&self.config_dir(), &self.config().log_file)
    }

    fn state_path(&self) -> PathBuf {
        resolve_path(&self.config_dir(), &self.config().state_file)
    }

    fn root_dir(&self) -> PathBuf {
        resolve_path(&self.config_dir(), &self.config().root_dir)
    }

    fn update_file_path(&self) -> PathBuf {
        resolve_path(&self.config_dir(), &self.config().update_file)
    }

    fn update_version_path(&self) -> PathBuf {
        resolve_path(&self.config_dir(), &self.config().update_version_file)
    }

    fn update_package(&self) -> io::Result<Option<UpdatePackage>> {
        load_update_package(self)
    }

    fn log(&self, message: &str) {
        let line = format!("[{}] {}", unix_now_string(), message);
        {
            let mut inner = self.inner.lock().unwrap();
            inner.runtime.recent_logs.push_back(line.clone());
            while inner.runtime.recent_logs.len() > LOG_TAIL_LIMIT {
                inner.runtime.recent_logs.pop_front();
            }
        }

        let log_path = self.log_path();
        if let Some(parent) = log_path.parent() {
            let _ = fs::create_dir_all(parent);
        }
        if let Ok(mut file) = OpenOptions::new().create(true).append(true).open(log_path) {
            let _ = writeln!(file, "{line}");
        }
        println!("{line}");
    }

    fn set_sync_started(&self, peer: &str) {
        let mut inner = self.inner.lock().unwrap();
        inner.runtime.last_client = peer.to_string();
        inner.runtime.last_sync_started = unix_now_string();
        inner.runtime.last_error.clear();
    }

    fn set_sync_finished(&self, result: &str, conflicts: Vec<ConflictRecord>) {
        let mut inner = self.inner.lock().unwrap();
        inner.runtime.last_sync_finished = unix_now_string();
        inner.runtime.last_result = result.to_string();
        inner.runtime.conflicts = conflicts;
    }

    fn set_sync_error(&self, error: &str) {
        let mut inner = self.inner.lock().unwrap();
        inner.runtime.last_sync_finished = unix_now_string();
        inner.runtime.last_error = error.to_string();
        inner.runtime.last_result = "Fehler".to_string();
    }

    fn snapshot(&self) -> (Config, RuntimeState) {
        let inner = self.inner.lock().unwrap();
        (inner.config.clone(), inner.runtime.clone())
    }
}

fn main() -> io::Result<()> {
    let config_path = std::env::args()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(DEFAULT_CONFIG_PATH));

    let shared = Arc::new(Shared::load(config_path)?);
    ensure_root_exists(&shared)?;
    shared.log(&format!(
        "Win16Sync-Server {} startet | Sync={} | Web={} | Root={}",
        APP_VERSION,
        shared.config().sync_bind,
        shared.config().http_bind,
        shared.root_dir().display()
    ));

    let sync_shared = shared.clone();
    let http_shared = shared.clone();

    let sync_thread = thread::spawn(move || run_sync_server(sync_shared));
    let http_thread = thread::spawn(move || run_http_server(http_shared));

    match sync_thread.join() {
        Ok(result) => result?,
        Err(_) => {
            return Err(io::Error::new(
                io::ErrorKind::Other,
                "Sync-Thread wurde unerwartet beendet",
            ))
        }
    }
    match http_thread.join() {
        Ok(result) => result?,
        Err(_) => {
            return Err(io::Error::new(
                io::ErrorKind::Other,
                "HTTP-Thread wurde unerwartet beendet",
            ))
        }
    }

    Ok(())
}

fn run_sync_server(shared: Arc<Shared>) -> io::Result<()> {
    let bind = shared.config().sync_bind;
    let listener = TcpListener::bind(&bind)?;
    shared.log(&format!("Sync-Port aktiv auf {bind}"));

    for stream in listener.incoming() {
        match stream {
            Ok(stream) => {
                let peer = stream
                    .peer_addr()
                    .map(|value| value.to_string())
                    .unwrap_or_else(|_| "unbekannt".to_string());
                let shared_clone = shared.clone();
                thread::spawn(move || {
                    if let Err(err) = handle_sync_client(shared_clone, stream, &peer) {
                        eprintln!("Sync-Fehler mit {peer}: {err}");
                    }
                });
            }
            Err(err) => shared.log(&format!("Listener-Fehler: {err}")),
        }
    }

    Ok(())
}

fn handle_sync_client(shared: Arc<Shared>, stream: TcpStream, peer: &str) -> io::Result<()> {
    let _sync_lock = shared.sync_guard.lock().unwrap();
    shared.set_sync_started(peer);
    shared.log(&format!("Client {peer} verbunden"));

    let root_dir = shared.root_dir();
    fs::create_dir_all(&root_dir)?;

    let reader_stream = stream.try_clone()?;
    let mut reader = BufReader::new(reader_stream);
    let mut writer = stream;

    send_line(
        &mut writer,
        &format!(
            "HELLO proto={} role=server version={}",
            PROTO_VERSION, APP_VERSION
        ),
    )?;

    let hello = read_line(&mut reader)?;
    let (command, values) = parse_line(&hello);
    if command != "HELLO" {
        return sync_fail(&shared, "Unerwarteter Verbindungsstart");
    }
    if values.get("proto").map(String::as_str) != Some(PROTO_VERSION) {
        return sync_fail(&shared, "Nicht unterstützte Protokollversion");
    }

    let mut next_line = read_line(&mut reader)?;
    let (next_command, next_values) = parse_line(&next_line);
    if next_command == "UPDATECHECK" {
        let client_version = next_values
            .get("version")
            .map(String::as_str)
            .unwrap_or("")
            .trim();
        if let Some(update) = shared.update_package()? {
            shared.log(&format!(
                "Update-Check von {peer}: Client={} | Angebot={}",
                if client_version.is_empty() {
                    "unbekannt"
                } else {
                    client_version
                },
                update.version
            ));
            send_line(
                &mut writer,
                &format!(
                    "UPDATE status=ready version={} size={} crc={:08X}",
                    update.version, update.size, update.crc32
                ),
            )?;
        } else {
            send_line(&mut writer, "UPDATE status=none")?;
        }

        next_line = read_line(&mut reader)?;
        let (follow_command, _) = parse_line(&next_line);
        if follow_command == "GETUPDATE" {
            let update = shared
                .update_package()?
                .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "Kein Update-Paket vorhanden"))?;
            shared.log(&format!(
                "Client {peer} lädt Update {} ({:08X})",
                update.version, update.crc32
            ));
            send_update_file(&mut writer, &mut reader, &update)?;
            let result = format!("Update {} an {} ausgeliefert", update.version, peer);
            shared.set_sync_finished(&result, Vec::new());
            shared.log(&result);
            let _ = writer.shutdown(Shutdown::Both);
            return Ok(());
        }
    }

    if next_line.trim() != "MANIFEST" {
        return sync_fail(&shared, "MANIFEST erwartet");
    }

    let client_manifest = read_manifest(&mut reader)?;
    shared.log(&format!(
        "Manifest von {peer}: {} Einträge",
        client_manifest.len()
    ));

    let mut sync_state = load_sync_state(&shared)?;
    let mut server_manifest = scan_server_tree(&shared, &root_dir)?;
    let actions = build_sync_actions(&sync_state, &server_manifest, &client_manifest);
    let action_count = actions.len();
    let mut conflicts = Vec::new();

    for action in &actions {
        match action {
            SyncAction::Download { item } => {
                shared.log(&format!("Download an Client: {}", item.manifest.path));
                send_action_download(&mut writer, &mut reader, &root_dir, item)?;
            }
            SyncAction::Upload { item } => {
                shared.log(&format!("Upload vom Client: {}", item.path));
                let stored = receive_upload(&mut writer, &mut reader, &root_dir, item)?;
                server_manifest.insert(stored.manifest.path.clone(), stored);
            }
            SyncAction::DeleteLocal { path } => {
                shared.log(&format!("Client löscht lokal: {path}"));
                send_line(
                    &mut writer,
                    &format!("ACTION kind=delete_local path={}", encode_hex(path.as_bytes())),
                )?;
                expect_simple_ok(&mut reader)?;
            }
            SyncAction::DeleteRemote { path } => {
                shared.log(&format!("Server löscht Datei: {path}"));
                delete_server_path(&root_dir, path)?;
                server_manifest.remove(path);
                send_line(
                    &mut writer,
                    &format!("ACTION kind=delete_remote path={}", encode_hex(path.as_bytes())),
                )?;
                expect_simple_ok(&mut reader)?;
            }
            SyncAction::Conflict { path, server, client } => {
                let server_text = server
                    .as_ref()
                    .map(describe_manifest)
                    .unwrap_or_else(|| "fehlt".to_string());
                let client_text = client
                    .as_ref()
                    .map(describe_manifest)
                    .unwrap_or_else(|| "fehlt".to_string());
                conflicts.push(ConflictRecord {
                    path: path.clone(),
                    server: server_text.clone(),
                    client: client_text.clone(),
                });
                shared.log(&format!(
                    "Konflikt erkannt: {path} | Server={server_text} | Client={client_text}"
                ));
                send_line(
                    &mut writer,
                    &format!("ACTION kind=conflict path={}", encode_hex(path.as_bytes())),
                )?;
                expect_simple_ok(&mut reader)?;
            }
        }
    }

    let final_client_manifest = apply_actions_to_client_manifest(client_manifest, &actions, &server_manifest);
    sync_state.files = build_state_after_sync(&server_manifest, &final_client_manifest);
    save_sync_state(&shared, &sync_state)?;

    send_line(
        &mut writer,
        &format!(
            "DONE actions={} conflicts={}",
            action_count,
            conflicts.len()
        ),
    )?;
    let _ = writer.shutdown(Shutdown::Both);

    let result = format!(
        "Sync ok | Aktionen={} | Konflikte={} | Dateien={}",
        action_count,
        conflicts.len(),
        sync_state.files.len()
    );
    shared.set_sync_finished(&result, conflicts);
    shared.log(&format!("Client {peer} abgeschlossen: {result}"));
    Ok(())
}

fn sync_fail(shared: &Shared, message: &str) -> io::Result<()> {
    shared.set_sync_error(message);
    shared.log(message);
    Err(io::Error::new(io::ErrorKind::Other, message.to_string()))
}

fn default_true() -> bool {
    true
}

fn read_manifest(reader: &mut BufReader<TcpStream>) -> io::Result<BTreeMap<String, ManifestItem>> {
    let mut manifest = BTreeMap::new();

    loop {
        let line = read_line(reader)?;
        if line.trim() == "END" {
            break;
        }

        let (command, values) = parse_line(&line);
        if command != "ITEM" {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "Ungültiger Manifest-Eintrag",
            ));
        }

        let path_hex = values
            .get("path")
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "Pfad fehlt"))?;
        let path_text = decode_hex_to_string(path_hex)?;
        let path = normalize_protocol_path(&path_text)?;
        let size = parse_u64(values.get("size"), "size")?;
        let crc_known = parse_bool_flag(values.get("crc_known"), true, "crc_known")?;
        let crc32 = if crc_known {
            parse_u32_hex(values.get("crc"), "crc")?
        } else {
            parse_u32_hex_or_default(values.get("crc"), 0, "crc")?
        };
        let dos_time = parse_u32_hex(values.get("dos"), "dos")?;

        if size > MAX_FILE_SIZE {
            continue;
        }

        manifest.insert(
            path.clone(),
            ManifestItem {
                path,
                fingerprint: Fingerprint {
                    size,
                    crc32,
                    crc_known,
                },
                dos_time,
            },
        );
    }

    Ok(manifest)
}

fn build_sync_actions(
    sync_state: &SyncState,
    server_manifest: &BTreeMap<String, ServerItem>,
    client_manifest: &BTreeMap<String, ManifestItem>,
) -> Vec<SyncAction> {
    let mut paths = BTreeMap::<String, ()>::new();
    let mut actions = Vec::new();

    for path in sync_state.files.keys() {
        paths.insert(path.clone(), ());
    }
    for path in server_manifest.keys() {
        paths.insert(path.clone(), ());
    }
    for path in client_manifest.keys() {
        paths.insert(path.clone(), ());
    }

    for path in paths.keys() {
        let previous = sync_state.files.get(path);
        let server_item = server_manifest.get(path);
        let client_item = client_manifest.get(path);

        let server_fp = server_item.map(|item| item.manifest.fingerprint.clone());
        let client_fp = client_item.map(|item| item.fingerprint.clone());

        if let (Some(server), Some(client)) = (server_item, client_item) {
            if same_live_items(Some(&server.manifest), Some(client)) {
                continue;
            }
        }

        if previous.is_none() {
            match (server_item, client_item) {
                (Some(server), None) => actions.push(SyncAction::Download { item: server.clone() }),
                (None, Some(client)) => actions.push(SyncAction::Upload { item: client.clone() }),
                (Some(server), Some(client)) => {
                    if !same_live_items(Some(&server.manifest), Some(client)) {
                        actions.push(SyncAction::Conflict {
                            path: path.clone(),
                            server: Some(server.manifest.clone()),
                            client: Some(client.clone()),
                        });
                    }
                }
                (None, None) => {}
            }
            continue;
        }

        let previous = previous.unwrap();
        let server_changed = !same_state_item(
            server_fp.as_ref(),
            server_item.map(|item| item.manifest.dos_time),
            previous.server.as_ref(),
            previous.server_dos_time,
        );
        let client_changed = !same_state_item(
            client_fp.as_ref(),
            client_item.map(|item| item.dos_time),
            previous.client.as_ref(),
            previous.client_dos_time,
        );

        match (server_changed, client_changed) {
            (false, false) => {}
            (true, false) => match server_item {
                Some(server) => actions.push(SyncAction::Download { item: server.clone() }),
                None => actions.push(SyncAction::DeleteLocal { path: path.clone() }),
            },
            (false, true) => match client_item {
                Some(client) => actions.push(SyncAction::Upload { item: client.clone() }),
                None => actions.push(SyncAction::DeleteRemote { path: path.clone() }),
            },
            (true, true) => {
                if same_live_items(server_item.map(|item| &item.manifest), client_item) {
                    continue;
                }
                actions.push(SyncAction::Conflict {
                    path: path.clone(),
                    server: server_item.map(|item| item.manifest.clone()),
                    client: client_item.cloned(),
                });
            }
        }
    }

    actions
}

fn apply_actions_to_client_manifest(
    mut client_manifest: BTreeMap<String, ManifestItem>,
    actions: &[SyncAction],
    server_manifest: &BTreeMap<String, ServerItem>,
) -> BTreeMap<String, ManifestItem> {
    for action in actions {
        match action {
            SyncAction::Download { item } => {
                client_manifest.insert(item.manifest.path.clone(), item.manifest.clone());
            }
            SyncAction::Upload { item } => {
                client_manifest.insert(item.path.clone(), item.clone());
            }
            SyncAction::DeleteLocal { path } => {
                client_manifest.remove(path);
            }
            SyncAction::DeleteRemote { path } => {
                client_manifest.remove(path);
            }
            SyncAction::Conflict { path, .. } => {
                if let Some(server) = server_manifest.get(path) {
                    let _ = server;
                }
            }
        }
    }
    client_manifest
}

fn build_state_after_sync(
    server_manifest: &BTreeMap<String, ServerItem>,
    client_manifest: &BTreeMap<String, ManifestItem>,
) -> BTreeMap<String, SyncStateEntry> {
    let mut state = BTreeMap::new();
    let mut paths = BTreeMap::<String, ()>::new();

    for path in server_manifest.keys() {
        paths.insert(path.clone(), ());
    }
    for path in client_manifest.keys() {
        paths.insert(path.clone(), ());
    }

    for path in paths.keys() {
        let server = server_manifest
            .get(path)
            .map(|item| item.manifest.fingerprint.clone());
        let client = client_manifest
            .get(path)
            .map(|item| item.fingerprint.clone());

        if same_live_items(server_manifest.get(path).map(|item| &item.manifest), client_manifest.get(path)) && server.is_some() {
            state.insert(
                path.clone(),
                SyncStateEntry {
                    server,
                    client,
                    server_dos_time: server_manifest.get(path).map(|item| item.manifest.dos_time),
                    client_dos_time: client_manifest.get(path).map(|item| item.dos_time),
                },
            );
        }
    }

    state
}

fn send_action_download(
    writer: &mut TcpStream,
    reader: &mut BufReader<TcpStream>,
    root_dir: &Path,
    item: &ServerItem,
) -> io::Result<()> {
    send_line(
        writer,
        &format!(
            "ACTION kind=download path={} size={} crc={:08X} dos={:08X} crc_known=1",
            encode_hex(item.manifest.path.as_bytes()),
            item.manifest.fingerprint.size,
            item.manifest.fingerprint.crc32,
            item.manifest.dos_time
        ),
    )?;
    expect_command(reader, "READY")?;

    send_line(
        writer,
        &format!(
            "FILE size={} crc={:08X} dos={:08X} crc_known=1",
            item.manifest.fingerprint.size,
            item.manifest.fingerprint.crc32,
            item.manifest.dos_time
        ),
    )?;

    let full_path = root_dir.join(&item.actual_rel);
    let mut file = File::open(full_path)?;
    io::copy(&mut file, writer)?;
    writer.flush()?;
    expect_simple_ok(reader)?;
    Ok(())
}

fn receive_upload(
    writer: &mut TcpStream,
    reader: &mut BufReader<TcpStream>,
    root_dir: &Path,
    requested: &ManifestItem,
) -> io::Result<ServerItem> {
    send_line(
        writer,
        &format!(
            "ACTION kind=upload path={} size={} crc={:08X} crc_known={}",
            encode_hex(requested.path.as_bytes()),
            requested.fingerprint.size,
            requested.fingerprint.crc32,
            if requested.fingerprint.crc_known { 1 } else { 0 }
        ),
    )?;

    let header = read_line(reader)?;
    let (command, values) = parse_line(&header);
    if command != "PUT" {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "PUT erwartet",
        ));
    }

    let path = normalize_protocol_path(
        &decode_hex_to_string(
            values
                .get("path")
                .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "Pfad fehlt"))?,
        )?,
    )?;
    if path != requested.path {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Pfad aus PUT passt nicht",
        ));
    }

    let size = parse_u64(values.get("size"), "size")?;
    let crc_known = parse_bool_flag(values.get("crc_known"), true, "crc_known")?;
    let crc32 = if crc_known {
        parse_u32_hex(values.get("crc"), "crc")?
    } else {
        parse_u32_hex_or_default(values.get("crc"), 0, "crc")?
    };
    let dos_time = parse_u32_hex(values.get("dos"), "dos")?;
    if size != requested.fingerprint.size
        || crc_known != requested.fingerprint.crc_known
        || (crc_known && crc32 != requested.fingerprint.crc32)
    {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "PUT-Metadaten passen nicht zur Aktion",
        ));
    }

    let actual_rel = PathBuf::from(path.replace('/', std::path::MAIN_SEPARATOR_STR));
    let full_path = root_dir.join(&actual_rel);
    if let Some(parent) = full_path.parent() {
        fs::create_dir_all(parent)?;
    }
    let tmp_path = full_path.with_extension("part");
    let mut file = File::create(&tmp_path)?;
    copy_exact(reader, &mut file, size)?;
    drop(file);

    let file_crc = crc32_file(&tmp_path)?;
    if crc_known && file_crc != crc32 {
        let _ = fs::remove_file(&tmp_path);
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "CRC-Fehler beim Upload",
        ));
    }

    fs::rename(&tmp_path, &full_path)?;
    send_line(writer, "OK")?;

    Ok(ServerItem {
        manifest: ManifestItem {
            path,
            fingerprint: Fingerprint {
                size,
                crc32: file_crc,
                crc_known: true,
            },
            dos_time,
        },
        actual_rel,
    })
}

fn delete_server_path(root_dir: &Path, path: &str) -> io::Result<()> {
    let actual_rel = PathBuf::from(path.replace('/', std::path::MAIN_SEPARATOR_STR));
    let full_path = root_dir.join(actual_rel);
    if full_path.exists() {
        fs::remove_file(full_path)?;
    }
    Ok(())
}

fn scan_server_tree(shared: &Shared, root_dir: &Path) -> io::Result<BTreeMap<String, ServerItem>> {
    let mut items = BTreeMap::new();
    scan_dir_recursive(shared, root_dir, root_dir, "", &mut items)?;
    Ok(items)
}

fn scan_dir_recursive(
    shared: &Shared,
    root_dir: &Path,
    current_dir: &Path,
    prefix: &str,
    items: &mut BTreeMap<String, ServerItem>,
) -> io::Result<()> {
    let mut entries = Vec::new();
    for entry in fs::read_dir(current_dir)? {
        entries.push(entry?);
    }
    entries.sort_by_key(|entry| entry.file_name());

    for entry in entries {
        let file_type = entry.file_type()?;
        let name_os = entry.file_name();
        let name = match name_os.to_str() {
            Some(text) => text,
            None => {
                shared.log("Nicht-ASCII-Pfad ignoriert");
                continue;
            }
        };
        if !is_valid_fat_component(name, file_type.is_file()) {
            shared.log(&format!("Nicht FAT16-tauglicher Name ignoriert: {name}"));
            continue;
        }
        let key = join_protocol_path(prefix, &name.to_ascii_uppercase());
        let entry_path = entry.path();
        let actual_rel = entry_path
            .strip_prefix(root_dir)
            .unwrap_or(entry_path.as_path())
            .to_path_buf();

        if file_type.is_dir() {
            scan_dir_recursive(shared, root_dir, &entry_path, &key, items)?;
            continue;
        }
        if !file_type.is_file() {
            continue;
        }

        let metadata = entry.metadata()?;
        let size = metadata.len();
        if size > MAX_FILE_SIZE {
            shared.log(&format!("Zu große Datei ignoriert: {}", actual_rel.display()));
            continue;
        }

        let crc32 = crc32_file(&entry_path)?;
        let dos_time = metadata
            .modified()
            .map(system_time_to_dos)
            .unwrap_or(0);

        let item = ServerItem {
            manifest: ManifestItem {
                path: key.clone(),
                fingerprint: Fingerprint {
                    size,
                    crc32,
                    crc_known: true,
                },
                dos_time,
            },
            actual_rel,
        };
        if items.insert(key.clone(), item).is_some() {
            shared.log(&format!("FAT16-Namenskollision ignoriert: {key}"));
        }
    }

    Ok(())
}

fn ensure_root_exists(shared: &Shared) -> io::Result<()> {
    let root = shared.root_dir();
    fs::create_dir_all(&root)?;
    let log_path = shared.log_path();
    if let Some(parent) = log_path.parent() {
        fs::create_dir_all(parent)?;
    }
    Ok(())
}

fn load_sync_state(shared: &Shared) -> io::Result<SyncState> {
    let path = shared.state_path();
    if !path.exists() {
        return Ok(SyncState::default());
    }
    let text = fs::read_to_string(path)?;
    Ok(serde_json::from_str(&text).unwrap_or_default())
}

fn save_sync_state(shared: &Shared, state: &SyncState) -> io::Result<()> {
    write_json_file(&shared.state_path(), state)
}

fn load_update_package(shared: &Shared) -> io::Result<Option<UpdatePackage>> {
    let version_path = shared.update_version_path();
    let update_path = shared.update_file_path();
    let version = match fs::read_to_string(&version_path) {
        Ok(text) => text.trim().to_string(),
        Err(err) if err.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(err) => return Err(err),
    };

    if version.is_empty() || version.chars().any(char::is_whitespace) {
        shared.log(&format!(
            "Update-Version ignoriert: ungültiger Inhalt in {}",
            version_path.display()
        ));
        return Ok(None);
    }
    if !update_path.exists() {
        return Ok(None);
    }

    let metadata = fs::metadata(&update_path)?;
    if !metadata.is_file() || metadata.len() > MAX_FILE_SIZE {
        shared.log(&format!(
            "Update-Datei ignoriert: {}",
            update_path.display()
        ));
        return Ok(None);
    }

    Ok(Some(UpdatePackage {
        version,
        size: metadata.len(),
        crc32: crc32_file(&update_path)?,
        path: update_path,
    }))
}

fn write_json_file<T: Serialize>(path: &Path, value: &T) -> io::Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let text = serde_json::to_string_pretty(value)
        .map_err(|err| io::Error::new(io::ErrorKind::Other, err.to_string()))?;
    fs::write(path, text)
}

fn run_http_server(shared: Arc<Shared>) -> io::Result<()> {
    let bind = shared.config().http_bind;
    let listener = TcpListener::bind(&bind)?;
    shared.log(&format!("Webinterface aktiv auf http://{bind}/"));

    for stream in listener.incoming() {
        match stream {
            Ok(stream) => {
                let shared_clone = shared.clone();
                thread::spawn(move || {
                    let _ = handle_http_client(shared_clone, stream);
                });
            }
            Err(err) => shared.log(&format!("HTTP-Listener-Fehler: {err}")),
        }
    }

    Ok(())
}

fn handle_http_client(shared: Arc<Shared>, mut stream: TcpStream) -> io::Result<()> {
    stream.set_read_timeout(Some(Duration::from_secs(5)))?;
    let reader_stream = stream.try_clone()?;
    let mut reader = BufReader::new(reader_stream);
    let request_line = read_line(&mut reader)?;
    if request_line.trim().is_empty() {
        return Ok(());
    }

    let mut parts = request_line.split_whitespace();
    let method = parts.next().unwrap_or("");
    let path = parts.next().unwrap_or("/");

    let mut content_length = 0usize;
    loop {
        let line = read_line(&mut reader)?;
        if line.trim().is_empty() {
            break;
        }
        let lower = line.to_ascii_lowercase();
        if let Some(value) = lower.strip_prefix("content-length:") {
            content_length = value.trim().parse::<usize>().unwrap_or(0);
        }
    }

    let mut body = vec![0u8; content_length];
    if content_length > 0 {
        reader.read_exact(&mut body)?;
    }

    match (method, path) {
        ("GET", "/") => respond_html(&mut stream, &render_index(&shared)),
        ("POST", "/save") => {
            save_from_form(&shared, &String::from_utf8_lossy(&body))?;
            respond_redirect(&mut stream, "/")
        }
        _ => respond_not_found(&mut stream),
    }
}

fn render_index(shared: &Shared) -> String {
    let (config, runtime) = shared.snapshot();
    let update_package = shared.update_package().ok().flatten();
    let mut body = String::new();

    body.push_str("<html><head><title>Win16Sync Server</title></head><body bgcolor=\"#ffffff\" text=\"#000000\">");
    body.push_str("<h1>Win16Sync Server</h1>");
    body.push_str("<p>Einfaches HTML ohne Skript, IE3-kompatibel.</p>");
    body.push_str("<table border=\"1\" cellpadding=\"4\" cellspacing=\"0\">");
    body.push_str("<tr><th align=\"left\">Status</th><td>");
    body.push_str(&html_escape(&runtime.last_result));
    body.push_str("</td></tr>");
    body.push_str("<tr><th align=\"left\">Letzter Client</th><td>");
    body.push_str(&html_escape(&runtime.last_client));
    body.push_str("</td></tr>");
    body.push_str("<tr><th align=\"left\">Start</th><td>");
    body.push_str(&html_escape(&runtime.last_sync_started));
    body.push_str("</td></tr>");
    body.push_str("<tr><th align=\"left\">Ende</th><td>");
    body.push_str(&html_escape(&runtime.last_sync_finished));
    body.push_str("</td></tr>");
    body.push_str("<tr><th align=\"left\">Letzter Fehler</th><td>");
    body.push_str(&html_escape(&runtime.last_error));
    body.push_str("</td></tr>");
    body.push_str("</table>");

    body.push_str("<h2>Konfiguration</h2>");
    body.push_str("<form method=\"post\" action=\"/save\">");
    body.push_str("<table border=\"0\" cellpadding=\"4\" cellspacing=\"0\">");
    body.push_str("<tr><td>Sync Bind</td><td><input name=\"sync_bind\" size=\"40\" value=\"");
    body.push_str(&html_escape(&config.sync_bind));
    body.push_str("\"></td></tr>");
    body.push_str("<tr><td>Web Bind</td><td><input name=\"http_bind\" size=\"40\" value=\"");
    body.push_str(&html_escape(&config.http_bind));
    body.push_str("\"></td></tr>");
    body.push_str("<tr><td>Zielordner</td><td><input name=\"root_dir\" size=\"40\" value=\"");
    body.push_str(&html_escape(&config.root_dir));
    body.push_str("\"></td></tr>");
    body.push_str("<tr><td>Logdatei</td><td><input name=\"log_file\" size=\"40\" value=\"");
    body.push_str(&html_escape(&config.log_file));
    body.push_str("\"></td></tr>");
    body.push_str("<tr><td>State-Datei</td><td><input name=\"state_file\" size=\"40\" value=\"");
    body.push_str(&html_escape(&config.state_file));
    body.push_str("\"></td></tr>");
    body.push_str("<tr><td>Update-EXE</td><td><input name=\"update_file\" size=\"40\" value=\"");
    body.push_str(&html_escape(&config.update_file));
    body.push_str("\"></td></tr>");
    body.push_str("<tr><td>Update-Version</td><td><input name=\"update_version_file\" size=\"40\" value=\"");
    body.push_str(&html_escape(&config.update_version_file));
    body.push_str("\"></td></tr>");
    body.push_str("<tr><td></td><td><input type=\"submit\" value=\"Speichern\"></td></tr>");
    body.push_str("</table></form>");

    body.push_str("<h2>Client-Update</h2>");
    body.push_str("<p>Manuell bereitlegen, kein Build auf dem Server nötig.</p>");
    body.push_str("<table border=\"1\" cellpadding=\"4\" cellspacing=\"0\">");
    body.push_str("<tr><th align=\"left\">Status</th><td>");
    if let Some(update) = &update_package {
        body.push_str("Bereit");
        body.push_str("</td></tr><tr><th align=\"left\">Version</th><td>");
        body.push_str(&html_escape(&update.version));
        body.push_str("</td></tr><tr><th align=\"left\">Datei</th><td>");
        body.push_str(&html_escape(&shared.update_file_path().display().to_string()));
        body.push_str("</td></tr><tr><th align=\"left\">Groesse</th><td>");
        body.push_str(&html_escape(&update.size.to_string()));
        body.push_str(" Bytes</td></tr><tr><th align=\"left\">CRC32</th><td>");
        body.push_str(&format!("{:08X}", update.crc32));
    } else {
        body.push_str("Kein Update bereitgelegt");
        body.push_str("</td></tr><tr><th align=\"left\">Erwartet</th><td>");
        body.push_str(&html_escape(&shared.update_file_path().display().to_string()));
        body.push_str("<br>");
        body.push_str(&html_escape(
            &shared.update_version_path().display().to_string(),
        ));
    }
    body.push_str("</td></tr></table>");

    body.push_str("<h2>Konflikte</h2>");
    if runtime.conflicts.is_empty() {
        body.push_str("<p>Keine Konflikte.</p>");
    } else {
        body.push_str("<table border=\"1\" cellpadding=\"4\" cellspacing=\"0\">");
        body.push_str("<tr><th align=\"left\">Pfad</th><th align=\"left\">Server</th><th align=\"left\">Client</th></tr>");
        for conflict in &runtime.conflicts {
            body.push_str("<tr><td>");
            body.push_str(&html_escape(&conflict.path));
            body.push_str("</td><td>");
            body.push_str(&html_escape(&conflict.server));
            body.push_str("</td><td>");
            body.push_str(&html_escape(&conflict.client));
            body.push_str("</td></tr>");
        }
        body.push_str("</table>");
    }

    body.push_str("<h2>Log</h2><pre>");
    for line in runtime.recent_logs {
        body.push_str(&html_escape(&line));
        body.push('\n');
    }
    body.push_str("</pre>");
    body.push_str("</body></html>");
    body
}

fn save_from_form(shared: &Shared, body: &str) -> io::Result<()> {
    let values = parse_form(body);
    let mut config = shared.config();

    if let Some(value) = values.get("sync_bind") {
        config.sync_bind = value.clone();
    }
    if let Some(value) = values.get("http_bind") {
        config.http_bind = value.clone();
    }
    if let Some(value) = values.get("root_dir") {
        config.root_dir = value.clone();
    }
    if let Some(value) = values.get("log_file") {
        config.log_file = value.clone();
    }
    if let Some(value) = values.get("state_file") {
        config.state_file = value.clone();
    }
    if let Some(value) = values.get("update_file") {
        config.update_file = value.clone();
    }
    if let Some(value) = values.get("update_version_file") {
        config.update_version_file = value.clone();
    }

    shared.update_config(config)?;
    ensure_root_exists(shared)?;
    shared.log("Konfiguration über Webinterface gespeichert");
    Ok(())
}

fn respond_html(stream: &mut TcpStream, body: &str) -> io::Result<()> {
    let header = format!(
        "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=iso-8859-1\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        body.as_bytes().len()
    );
    stream.write_all(header.as_bytes())?;
    stream.write_all(body.as_bytes())?;
    stream.flush()
}

fn respond_redirect(stream: &mut TcpStream, location: &str) -> io::Result<()> {
    let response = format!(
        "HTTP/1.0 302 Found\r\nLocation: {}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n",
        location
    );
    stream.write_all(response.as_bytes())?;
    stream.flush()
}

fn respond_not_found(stream: &mut TcpStream) -> io::Result<()> {
    let body = "<html><body><h1>404</h1></body></html>";
    let header = format!(
        "HTTP/1.0 404 Not Found\r\nContent-Type: text/html\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        body.len()
    );
    stream.write_all(header.as_bytes())?;
    stream.write_all(body.as_bytes())?;
    stream.flush()
}

fn parse_form(body: &str) -> HashMap<String, String> {
    let mut values = HashMap::new();
    for pair in body.split('&') {
        if let Some((key, value)) = pair.split_once('=') {
            values.insert(url_decode(key), url_decode(value));
        }
    }
    values
}

fn url_decode(text: &str) -> String {
    let mut out = String::new();
    let bytes = text.as_bytes();
    let mut index = 0usize;
    while index < bytes.len() {
        match bytes[index] {
            b'+' => {
                out.push(' ');
                index += 1;
            }
            b'%' if index + 2 < bytes.len() => {
                if let (Some(left), Some(right)) = (
                    hex_value(bytes[index + 1]),
                    hex_value(bytes[index + 2]),
                ) {
                    out.push((left * 16 + right) as char);
                    index += 3;
                } else {
                    out.push('%');
                    index += 1;
                }
            }
            byte => {
                out.push(byte as char);
                index += 1;
            }
        }
    }
    out
}

fn parse_line(line: &str) -> (String, HashMap<String, String>) {
    let trimmed = line.trim();
    let mut parts = trimmed.split_whitespace();
    let command = parts.next().unwrap_or("").to_string();
    let mut values = HashMap::new();

    for part in parts {
        if let Some((key, value)) = part.split_once('=') {
            values.insert(key.to_string(), value.to_string());
        }
    }

    (command, values)
}

fn expect_command(reader: &mut BufReader<TcpStream>, expected: &str) -> io::Result<()> {
    let line = read_line(reader)?;
    let (command, _) = parse_line(&line);
    if command != expected {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("{expected} erwartet"),
        ));
    }
    Ok(())
}

fn expect_simple_ok(reader: &mut BufReader<TcpStream>) -> io::Result<()> {
    let line = read_line(reader)?;
    if line.trim() != "OK" {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("OK erwartet, erhalten: {}", line.trim()),
        ));
    }
    Ok(())
}

fn send_update_file(
    writer: &mut TcpStream,
    reader: &mut BufReader<TcpStream>,
    update: &UpdatePackage,
) -> io::Result<()> {
    send_line(
        writer,
        &format!(
            "UPDATEFILE version={} size={} crc={:08X}",
            update.version, update.size, update.crc32
        ),
    )?;
    let mut file = File::open(&update.path)?;
    io::copy(&mut file, writer)?;
    writer.flush()?;
    expect_simple_ok(reader)
}

fn read_line(reader: &mut BufReader<TcpStream>) -> io::Result<String> {
    let mut line = String::new();
    let bytes = reader.read_line(&mut line)?;
    if bytes == 0 {
        return Err(io::Error::new(
            io::ErrorKind::UnexpectedEof,
            "Verbindung unerwartet beendet",
        ));
    }
    while line.ends_with('\n') || line.ends_with('\r') {
        line.pop();
    }
    Ok(line)
}

fn send_line(stream: &mut TcpStream, line: &str) -> io::Result<()> {
    stream.write_all(line.as_bytes())?;
    stream.write_all(b"\r\n")?;
    stream.flush()
}

fn copy_exact<R: Read, W: Write>(reader: &mut R, writer: &mut W, size: u64) -> io::Result<()> {
    let mut remaining = size;
    let mut buffer = [0u8; 8192];

    while remaining > 0 {
        let chunk = remaining.min(buffer.len() as u64) as usize;
        reader.read_exact(&mut buffer[..chunk])?;
        writer.write_all(&buffer[..chunk])?;
        remaining -= chunk as u64;
    }
    writer.flush()
}

fn crc32_file(path: &Path) -> io::Result<u32> {
    let mut file = File::open(path)?;
    let mut buffer = [0u8; 8192];
    let mut crc = 0xFFFF_FFFFu32;

    loop {
        let read = file.read(&mut buffer)?;
        if read == 0 {
            break;
        }
        crc = crc32_update(crc, &buffer[..read]);
    }

    Ok(!crc)
}

fn crc32_update(mut crc: u32, data: &[u8]) -> u32 {
    for byte in data {
        crc ^= *byte as u32;
        for _ in 0..8 {
            if crc & 1 != 0 {
                crc = (crc >> 1) ^ 0xEDB8_8320;
            } else {
                crc >>= 1;
            }
        }
    }
    crc
}

fn system_time_to_dos(time: SystemTime) -> u32 {
    let unix = time
        .duration_since(UNIX_EPOCH)
        .unwrap_or_else(|_| Duration::from_secs(0))
        .as_secs();
    let (year, month, day, hour, minute, second) = unix_to_ymdhms(unix);
    if year < 1980 {
        return 0;
    }

    (((year - 1980) as u32) << 25)
        | ((month as u32) << 21)
        | ((day as u32) << 16)
        | ((hour as u32) << 11)
        | ((minute as u32) << 5)
        | ((second as u32 / 2) & 0x1F)
}

fn unix_to_ymdhms(unix: u64) -> (i32, i32, i32, i32, i32, i32) {
    let days = (unix / 86_400) as i64;
    let seconds = (unix % 86_400) as i64;
    let hour = (seconds / 3_600) as i32;
    let minute = ((seconds % 3_600) / 60) as i32;
    let second = (seconds % 60) as i32;

    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = mp + if mp < 10 { 3 } else { -9 };
    let year = y + if month <= 2 { 1 } else { 0 };

    (
        year as i32,
        month as i32,
        day as i32,
        hour,
        minute,
        second,
    )
}

fn normalize_protocol_path(path: &str) -> io::Result<String> {
    if path.is_empty() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Leerer Pfad ist ungültig",
        ));
    }

    let mut components = Vec::new();
    for raw in path.split('/') {
        if raw.is_empty() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "Doppelte Trenner im Pfad",
            ));
        }
        if !is_valid_fat_component(raw, true) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "Pfad ist nicht FAT16-tauglich",
            ));
        }
        components.push(raw.to_ascii_uppercase());
    }
    Ok(components.join("/"))
}

fn is_valid_fat_component(name: &str, is_file: bool) -> bool {
    if name.is_empty() || !name.is_ascii() || name == "." || name == ".." {
        return false;
    }

    let upper = name.to_ascii_uppercase();
    let mut parts = upper.split('.');
    let base = parts.next().unwrap_or("");
    let ext = parts.next();
    if parts.next().is_some() {
        return false;
    }
    if base.is_empty() || base.len() > 8 {
        return false;
    }
    if let Some(ext) = ext {
        if ext.is_empty() || ext.len() > 3 {
            return false;
        }
        if !ext.bytes().all(is_valid_fat_char) {
            return false;
        }
    } else if !is_file && upper.contains('.') {
        return false;
    }

    base.bytes().all(is_valid_fat_char)
}

fn is_valid_fat_char(byte: u8) -> bool {
    matches!(
        byte,
        b'A'..=b'Z'
            | b'0'..=b'9'
            | b'$'
            | b'%'
            | b'\''
            | b'-'
            | b'_'
            | b'@'
            | b'~'
            | b'`'
            | b'!'
            | b'('
            | b')'
            | b'{'
            | b'}'
            | b'^'
            | b'#'
            | b'&'
    )
}

fn join_protocol_path(prefix: &str, component: &str) -> String {
    if prefix.is_empty() {
        component.to_string()
    } else {
        format!("{prefix}/{component}")
    }
}

fn resolve_path(base_dir: &Path, value: &str) -> PathBuf {
    if value == "~" {
        return home_dir().unwrap_or_else(|| PathBuf::from("."));
    }
    if let Some(rest) = value.strip_prefix("~/") {
        if let Some(home) = home_dir() {
            return home.join(rest);
        }
    }

    let path = PathBuf::from(value);
    if path.is_absolute() {
        path
    } else {
        base_dir.join(path)
    }
}

fn home_dir() -> Option<PathBuf> {
    std::env::var_os("HOME").map(PathBuf::from)
}

fn unix_now_string() -> String {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_secs().to_string())
        .unwrap_or_else(|_| "0".to_string())
}

fn html_escape(text: &str) -> String {
    text.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
}

fn describe_manifest(item: &ManifestItem) -> String {
    if item.fingerprint.crc_known {
        format!("size={} crc={:08X}", item.fingerprint.size, item.fingerprint.crc32)
    } else {
        format!("size={} crc=OFF dos={:08X}", item.fingerprint.size, item.dos_time)
    }
}

fn parse_u64(value: Option<&String>, field: &str) -> io::Result<u64> {
    value
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, format!("{field} fehlt")))?
        .parse::<u64>()
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, format!("{field} ungültig")))
}

fn parse_u32_hex(value: Option<&String>, field: &str) -> io::Result<u32> {
    u32::from_str_radix(
        value
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, format!("{field} fehlt")))?,
        16,
    )
    .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, format!("{field} ungültig")))
}

fn parse_u32_hex_or_default(value: Option<&String>, default: u32, field: &str) -> io::Result<u32> {
    match value {
        Some(text) => u32::from_str_radix(text, 16)
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, format!("{field} ungültig"))),
        None => Ok(default),
    }
}

fn parse_bool_flag(value: Option<&String>, default: bool, field: &str) -> io::Result<bool> {
    match value.map(String::as_str) {
        Some("1") => Ok(true),
        Some("0") => Ok(false),
        Some(_) => Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("{field} ungültig"),
        )),
        None => Ok(default),
    }
}

fn same_state_item(
    current_fp: Option<&Fingerprint>,
    current_dos: Option<u32>,
    previous_fp: Option<&Fingerprint>,
    previous_dos: Option<u32>,
) -> bool {
    match (current_fp, previous_fp) {
        (None, None) => true,
        (Some(current), Some(previous)) => same_revision(
            current,
            current_dos.unwrap_or(0),
            previous,
            previous_dos.unwrap_or(0),
        ),
        _ => false,
    }
}

fn same_live_items(left: Option<&ManifestItem>, right: Option<&ManifestItem>) -> bool {
    match (left, right) {
        (Some(left), Some(right)) => same_revision(
            &left.fingerprint,
            left.dos_time,
            &right.fingerprint,
            right.dos_time,
        ),
        (None, None) => true,
        _ => false,
    }
}

fn same_revision(left: &Fingerprint, left_dos: u32, right: &Fingerprint, right_dos: u32) -> bool {
    if left.size != right.size {
        return false;
    }
    if left.crc_known && right.crc_known {
        return left.crc32 == right.crc32;
    }
    left_dos == right_dos
}

fn encode_hex(data: &[u8]) -> String {
    let mut out = String::with_capacity(data.len() * 2);
    for byte in data {
        out.push(hex_digit(byte >> 4));
        out.push(hex_digit(byte & 0x0F));
    }
    out
}

fn hex_digit(value: u8) -> char {
    match value {
        0..=9 => (b'0' + value) as char,
        _ => (b'A' + (value - 10)) as char,
    }
}

fn decode_hex_to_string(text: &str) -> io::Result<String> {
    let bytes = decode_hex(text)?;
    String::from_utf8(bytes)
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "Hex-Pfad ist kein UTF-8"))
}

fn decode_hex(text: &str) -> io::Result<Vec<u8>> {
    let bytes = text.as_bytes();
    if bytes.len() % 2 != 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Hex-Text hat ungerade Länge",
        ));
    }

    let mut out = Vec::with_capacity(bytes.len() / 2);
    let mut index = 0usize;
    while index < bytes.len() {
        let left = hex_value(bytes[index])
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "Ungültiges Hex"))?;
        let right = hex_value(bytes[index + 1])
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "Ungültiges Hex"))?;
        out.push(left * 16 + right);
        index += 2;
    }
    Ok(out)
}

fn hex_value(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}
