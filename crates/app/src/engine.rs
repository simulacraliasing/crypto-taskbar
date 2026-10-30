use crypto_taskbar_core::{Config, Quote, Source};
use serde_json::Value;
use std::{
    collections::BTreeMap,
    sync::{
        Arc, Condvar, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    thread::{self, JoinHandle},
    time::{Duration, SystemTime, UNIX_EPOCH},
};

pub fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}
#[derive(Clone)]
pub struct Snapshot {
    pub config: Config,
    pub quotes: Vec<Quote>,
}
struct State {
    snapshot: Snapshot,
    revision: u64,
    refresh: bool,
}
pub struct Engine {
    shared: Arc<(Mutex<State>, Condvar)>,
    stop: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
}
impl Engine {
    pub fn demo(config: Config) -> Self {
        let quotes = config
            .coins
            .iter()
            .enumerate()
            .map(|(i, _)| Quote {
                price: Some([78110., 2246.34, 98.04][i % 3]),
                change: Some(if i % 2 == 0 { -2.19 } else { 1.87 }),
                updated: Some(now()),
                ..Quote::default()
            })
            .collect();
        Self {
            shared: Arc::new((
                Mutex::new(State {
                    snapshot: Snapshot { config, quotes },
                    revision: 0,
                    refresh: false,
                }),
                Condvar::new(),
            )),
            stop: Arc::new(AtomicBool::new(false)),
            worker: None,
        }
    }
    pub fn new(config: Config, notify: impl Fn() + Send + 'static) -> Self {
        let shared = Arc::new((
            Mutex::new(State {
                snapshot: Snapshot {
                    quotes: vec![Quote::default(); config.coins.len()],
                    config,
                },
                revision: 0,
                refresh: true,
            }),
            Condvar::new(),
        ));
        let stop = Arc::new(AtomicBool::new(false));
        let state = shared.clone();
        let cancellation = stop.clone();
        let worker = thread::spawn(move || {
            #[cfg(windows)]
            let mut client = crate::http::Client::new().ok();
            let mut last_revision = u64::MAX;
            let mut retry_after = BTreeMap::new();
            loop {
                if cancellation.load(Ordering::Relaxed) {
                    break;
                }
                let (mut snapshot, revision) = {
                    let mut guard = state.0.lock().unwrap_or_else(|p| p.into_inner());
                    guard.refresh = false;
                    (guard.snapshot.clone(), guard.revision)
                };
                if revision != last_revision {
                    retry_after.clear();
                    last_revision = revision;
                }
                let cancelled = || {
                    cancellation.load(Ordering::Relaxed)
                        || state.0.lock().unwrap_or_else(|p| p.into_inner()).revision != revision
                };
                refresh_snapshot(
                    &mut snapshot,
                    &mut retry_after,
                    &cancelled,
                    |url, source| {
                        #[cfg(windows)]
                        {
                            if client.is_none() {
                                client = crate::http::Client::new().ok();
                            }
                            client
                                .as_ref()
                                .ok_or_else(|| anyhow::anyhow!("无法初始化网络"))?
                                .get(url, source, &cancellation)
                        }
                        #[cfg(not(windows))]
                        {
                            let _ = (url, source);
                            anyhow::bail!("仅支持 Windows")
                        }
                    },
                );
                if cancellation.load(Ordering::Relaxed) {
                    break;
                }
                let interval = snapshot.config.refresh_seconds;
                {
                    let mut guard = state.0.lock().unwrap_or_else(|p| p.into_inner());
                    if guard.revision == revision {
                        guard.snapshot = snapshot;
                    }
                }
                notify();
                let guard = state.0.lock().unwrap_or_else(|p| p.into_inner());
                drop(
                    state
                        .1
                        .wait_timeout_while(guard, Duration::from_secs(interval), |s| {
                            !s.refresh
                                && s.revision == revision
                                && !cancellation.load(Ordering::Relaxed)
                        }),
                );
            }
        });
        Self {
            shared,
            stop,
            worker: Some(worker),
        }
    }
    pub fn snapshot(&self) -> Snapshot {
        self.shared
            .0
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .snapshot
            .clone()
    }
    pub fn refresh(&self) {
        self.shared
            .0
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .refresh = true;
        self.shared.1.notify_all();
    }
    pub fn apply(&self, config: Config) {
        let mut state = self.shared.0.lock().unwrap_or_else(|p| p.into_inner());
        state.snapshot = Snapshot {
            quotes: vec![Quote::default(); config.coins.len()],
            config,
        };
        state.revision += 1;
        state.refresh = true;
        drop(state);
        self.shared.1.notify_all();
    }
}
impl Drop for Engine {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        self.shared.1.notify_all();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

pub fn refresh_snapshot(
    snapshot: &mut Snapshot,
    retry_after: &mut BTreeMap<String, u64>,
    cancelled: &impl Fn() -> bool,
    mut fetch: impl FnMut(&str, &Source) -> anyhow::Result<Value>,
) {
    let config = &snapshot.config;
    let mut cache: BTreeMap<(String, String), Result<Value, String>> = BTreeMap::new();
    let timestamp = now();
    for (index, coin) in config.coins.iter().enumerate() {
        if cancelled() {
            return;
        }
        if retry_after.get(&coin.source).copied().unwrap_or(0) > timestamp {
            snapshot.quotes[index].stale = snapshot.quotes[index].price.is_some();
            continue;
        }
        let source = &config.sources[&coin.source];
        let result = (|| -> anyhow::Result<Quote> {
            let url = config.request_url(coin, false, timestamp as i64 * 1000)?;
            let body = cache
                .entry((coin.source.clone(), url.clone()))
                .or_insert_with(|| fetch(&url, source).map_err(|e| e.to_string()));
            let mut quote = Quote::parse(
                body.as_ref().map_err(|s| anyhow::anyhow!(s.clone()))?,
                source,
                coin,
                &config.change_period,
            )?;
            if config.change_period != "24h" && !source.history_url.is_empty() {
                if cancelled() {
                    anyhow::bail!("请求已取消");
                }
                let history = (|| -> anyhow::Result<()> {
                    let url = config.request_url(coin, true, timestamp as i64 * 1000)?;
                    let body = cache
                        .entry((coin.source.clone(), url.clone()))
                        .or_insert_with(|| fetch(&url, source).map_err(|e| e.to_string()));
                    quote.apply_history(
                        body.as_ref().map_err(|s| anyhow::anyhow!(s.clone()))?,
                        source,
                        coin,
                    )
                })();
                if let Err(error) = history {
                    quote.change = None;
                    quote.change_error = Some(error.to_string());
                }
            }
            quote.updated = Some(timestamp);
            Ok(quote)
        })();
        match result {
            Ok(quote) => snapshot.quotes[index] = quote,
            Err(error) => snapshot.quotes[index].failed(error.to_string()),
        }
    }
    for ((source, _), result) in cache {
        if result.as_ref().is_err_and(|error| error == "HTTP 429") {
            retry_after.insert(source, now() + 60);
        }
    }
}
